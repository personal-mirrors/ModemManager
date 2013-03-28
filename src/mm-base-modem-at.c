/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2011 Aleksander Morgado <aleksander@gnu.org>
 */

#include <glib.h>
#include <glib-object.h>

#include <ModemManager.h>

#include "mm-base-modem-at.h"
#include "mm-errors-types.h"

static gboolean
abort_async_if_port_unusable (MMBaseModem *self,
                              MMAtSerialPort *port,
                              GAsyncReadyCallback callback,
                              gpointer user_data)
{
    GError *error = NULL;
    gboolean init_sequence_enabled = FALSE;

    /* If no port given, probably the port dissapeared */
    if (!port) {
        g_simple_async_report_error_in_idle (
            G_OBJECT (self),
            callback,
            user_data,
            MM_CORE_ERROR,
            MM_CORE_ERROR_NOT_FOUND,
            "Cannot run sequence: port not given");
        return FALSE;
    }

    /* Ensure we don't try to use a connected port */
    if (mm_port_get_connected (MM_PORT (port))) {
        g_simple_async_report_error_in_idle (
            G_OBJECT (self),
            callback,
            user_data,
            MM_CORE_ERROR,
            MM_CORE_ERROR_CONNECTED,
            "Cannot run sequence: port is connected");
        return FALSE;
    }

    /* Temporarily disable init sequence if we're just sending a
     * command to a just opened port */
    g_object_get (port, MM_AT_SERIAL_PORT_INIT_SEQUENCE_ENABLED, &init_sequence_enabled, NULL);
    g_object_set (port, MM_AT_SERIAL_PORT_INIT_SEQUENCE_ENABLED, FALSE, NULL);

    /* Ensure we have a port open during the sequence */
    if (!mm_serial_port_open (MM_SERIAL_PORT (port), &error)) {
        g_simple_async_report_error_in_idle (
            G_OBJECT (self),
            callback,
            user_data,
            MM_CORE_ERROR,
            MM_CORE_ERROR_CONNECTED,
            "Cannot run sequence: '%s'",
            error->message);
        g_error_free (error);
        return FALSE;
    }

    /* Reset previous init sequence state */
    g_object_set (port, MM_AT_SERIAL_PORT_INIT_SEQUENCE_ENABLED, init_sequence_enabled, NULL);

    return TRUE;
}

static void
modem_cancellable_cancelled (GCancellable *modem_cancellable,
                             GCancellable *user_cancellable)
{
    g_cancellable_cancel (user_cancellable);
}

/*****************************************************************************/
/* AT sequence handling */

typedef struct {
    MMBaseModem *self;
    MMAtSerialPort *port;
    GCancellable *cancellable;
    gulong cancelled_id;
    GCancellable *modem_cancellable;
    GCancellable *user_cancellable;
    const MMBaseModemAtCommand *current;
    const MMBaseModemAtCommand *sequence;
    GSimpleAsyncResult *simple;
    gpointer response_processor_context;
    GDestroyNotify response_processor_context_free;
    GVariant *result;
} AtSequenceContext;

static void
at_sequence_context_free (AtSequenceContext *ctx)
{
    mm_serial_port_close (MM_SERIAL_PORT (ctx->port));
    g_object_unref (ctx->port);
    g_object_unref (ctx->self);

    if (ctx->response_processor_context &&
        ctx->response_processor_context_free)
        ctx->response_processor_context_free (ctx->response_processor_context);

    if (ctx->cancelled_id)
        g_cancellable_disconnect (ctx->modem_cancellable,
                                  ctx->cancelled_id);
    if (ctx->user_cancellable)
        g_object_unref (ctx->user_cancellable);
    g_object_unref (ctx->modem_cancellable);
    g_object_unref (ctx->cancellable);

    if (ctx->result)
        g_variant_unref (ctx->result);
    if (ctx->simple)
        g_object_unref (ctx->simple);
    g_free (ctx);
}

GVariant *
mm_base_modem_at_sequence_full_finish (MMBaseModem *self,
                                       GAsyncResult *res,
                                       gpointer *response_processor_context,
                                       GError **error)
{
    AtSequenceContext *ctx;

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res),
                                               error))
        return NULL;

    ctx = g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res));
    if (response_processor_context)
        /* transfer none, no need to free the context ourselves, if
         * we gave a response_processor_context_free callback */
        *response_processor_context = ctx->response_processor_context;

    /* transfer-none! (so that we can ignore it) */
    return ctx->result;
}

static void
at_sequence_parse_response (MMAtSerialPort *port,
                            GString *response,
                            GError *error,
                            AtSequenceContext *ctx)
{
    GVariant *result = NULL;
    GError *result_error = NULL;
    gboolean continue_sequence;
    GSimpleAsyncResult *simple;

    /* Cancelled? */
    if (g_cancellable_is_cancelled (ctx->cancellable)) {
        g_simple_async_result_set_error (ctx->simple,
                                         MM_CORE_ERROR,
                                         MM_CORE_ERROR_CANCELLED,
                                         "AT sequence was cancelled");
        g_simple_async_result_complete (ctx->simple);
        at_sequence_context_free (ctx);
        return;
    }

    if (!ctx->current->response_processor)
        /* No need to process response, go on to next command */
        continue_sequence = TRUE;
    else {
        const MMBaseModemAtCommand *next = ctx->current + 1;

        /* Response processor will tell us if we need to keep on the sequence */
        continue_sequence = !ctx->current->response_processor (
            ctx->self,
            ctx->response_processor_context,
            ctx->current->command,
            response ? response->str : NULL,
            next->command ? FALSE : TRUE,  /* Last command in sequence? */
            error,
            &result,
            &result_error);
        /* Were we told to abort the sequence? */
        if (result_error) {
            g_assert (result == NULL);
            g_simple_async_result_take_error (ctx->simple, result_error);
            g_simple_async_result_complete (ctx->simple);
            at_sequence_context_free (ctx);
            return;
        }
    }

    if (continue_sequence) {
        g_assert (result == NULL);
        ctx->current++;
        if (ctx->current->command) {
            /* Schedule the next command in the probing group */
            if (ctx->current->allow_cached)
                mm_at_serial_port_queue_command_cached (
                    ctx->port,
                    ctx->current->command,
                    ctx->current->timeout,
                    FALSE,
                    ctx->cancellable,
                    (MMAtSerialResponseFn)at_sequence_parse_response,
                    ctx);
            else
                mm_at_serial_port_queue_command (
                    ctx->port,
                    ctx->current->command,
                    ctx->current->timeout,
                    FALSE,
                    ctx->cancellable,
                    (MMAtSerialResponseFn)at_sequence_parse_response,
                    ctx);
            return;
        }

        /* On last command, end. */
    }

    /* If we got a response, set it as result */
    if (result)
        /* transfer-full */
        ctx->result = result;

    /* Set the whole context as result, in order to pass the response
     * processor context during finish(). We do remove the simple async result
     * from the context as well, so that we control its last unref. */
    simple = ctx->simple;
    ctx->simple = NULL;
    g_simple_async_result_set_op_res_gpointer (
        simple,
        ctx,
        (GDestroyNotify)at_sequence_context_free);

    /* And complete. The whole context is owned by the result, and it will
     * be freed when completed. */
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

void
mm_base_modem_at_sequence_full (MMBaseModem *self,
                                MMAtSerialPort *port,
                                const MMBaseModemAtCommand *sequence,
                                gpointer response_processor_context,
                                GDestroyNotify response_processor_context_free,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data)
{
    AtSequenceContext *ctx;

    /* Ensure that we have an open port */
    if (!abort_async_if_port_unusable (self, port, callback, user_data))
        return;

    /* Setup context */
    ctx = g_new0 (AtSequenceContext, 1);
    ctx->self = g_object_ref (self);
    ctx->port = g_object_ref (port);
    ctx->simple = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             mm_base_modem_at_sequence_full);
    ctx->current = ctx->sequence = sequence;
    ctx->response_processor_context = response_processor_context;
    ctx->response_processor_context_free = response_processor_context_free;

    /* Setup cancellables */
    ctx->modem_cancellable = mm_base_modem_get_cancellable (self);
    ctx->user_cancellable = cancellable ? g_object_ref (cancellable) : NULL;
    if (!ctx->user_cancellable)
        /* Just the modem-wide one, use it directly */
        ctx->cancellable = g_object_ref (ctx->modem_cancellable);
    else {
        /* Use the user provided one, which will also get cancelled if the modem
         * wide-one gets cancelled */
        ctx->cancellable = g_object_ref (ctx->user_cancellable);
        ctx->cancelled_id = g_cancellable_connect (ctx->modem_cancellable,
                                                   G_CALLBACK (modem_cancellable_cancelled),
                                                   ctx->user_cancellable,
                                                   NULL);
    }

    /* Go on with the first one in the sequence */
    mm_at_serial_port_queue_command (
        ctx->port,
        ctx->current->command,
        ctx->current->timeout,
        FALSE,
        ctx->cancellable,
        (MMAtSerialResponseFn)at_sequence_parse_response,
        ctx);
}

GVariant *
mm_base_modem_at_sequence_finish (MMBaseModem *self,
                                  GAsyncResult *res,
                                  gpointer *response_processor_context,
                                  GError **error)
{
    return (mm_base_modem_at_sequence_full_finish (
                self,
                res,
                response_processor_context,
                error));
}

void
mm_base_modem_at_sequence (MMBaseModem *self,
                           const MMBaseModemAtCommand *sequence,
                           gpointer response_processor_context,
                           GDestroyNotify response_processor_context_free,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
    MMAtSerialPort *port;
    GError *error = NULL;

    /* No port given, so we'll try to guess which is best */
    port = mm_base_modem_peek_best_at_port (self, &error);
    if (!port) {
        g_assert (error != NULL);
        g_simple_async_report_take_gerror_in_idle (G_OBJECT (self),
                                                   callback,
                                                   user_data,
                                                   error);
        return;
    }

    mm_base_modem_at_sequence_full (
        self,
        port,
        sequence,
        response_processor_context,
        response_processor_context_free,
        NULL,
        callback,
        user_data);
}

/*****************************************************************************/
/* Response processor helpers */

gboolean
mm_base_modem_response_processor_string (MMBaseModem *self,
                                         gpointer none,
                                         const gchar *command,
                                         const gchar *response,
                                         gboolean last_command,
                                         const GError *error,
                                         GVariant **result,
                                         GError **result_error)
{
    if (error) {
        *result_error = g_error_copy (error);
        return FALSE;
    }

    *result = g_variant_new_string (response);
    return TRUE;
}

gboolean
mm_base_modem_response_processor_no_result (MMBaseModem *self,
                                            gpointer none,
                                            const gchar *command,
                                            const gchar *response,
                                            gboolean last_command,
                                            const GError *error,
                                            GVariant **result,
                                            GError **result_error)
{
    if (error) {
        *result_error = g_error_copy (error);
        return FALSE;
    }

    *result = NULL;
    return TRUE;
}

gboolean
mm_base_modem_response_processor_no_result_continue (MMBaseModem *self,
                                                     gpointer none,
                                                     const gchar *command,
                                                     const gchar *response,
                                                     gboolean last_command,
                                                     const GError *error,
                                                     GVariant **result,
                                                     GError **result_error)
{
    if (error)
        *result_error = g_error_copy (error);

    /* Return FALSE so that we keep on with the next steps in the sequence */
    return FALSE;
}

gboolean
mm_base_modem_response_processor_continue_on_error (MMBaseModem *self,
                                                    gpointer none,
                                                    const gchar *command,
                                                    const gchar *response,
                                                    gboolean last_command,
                                                    const GError *error,
                                                    GVariant **result,
                                                    GError **result_error)
{
    if (error)
        return FALSE;

    *result = NULL;
    return TRUE;
}

/*****************************************************************************/
/* Single AT command handling */

typedef struct {
    MMBaseModem *self;
    MMAtSerialPort *port;
    GCancellable *cancellable;
    gulong cancelled_id;
    GCancellable *modem_cancellable;
    GCancellable *user_cancellable;
    GSimpleAsyncResult *result;
} AtCommandContext;

static void
at_command_context_free (AtCommandContext *ctx)
{
    mm_serial_port_close (MM_SERIAL_PORT (ctx->port));

    if (ctx->cancelled_id)
        g_cancellable_disconnect (ctx->modem_cancellable,
                                  ctx->cancelled_id);
    if (ctx->user_cancellable)
        g_object_unref (ctx->user_cancellable);
    g_object_unref (ctx->modem_cancellable);
    g_object_unref (ctx->cancellable);

    g_object_unref (ctx->port);
    g_object_unref (ctx->result);
    g_object_unref (ctx->self);
    g_free (ctx);
}

const gchar *
mm_base_modem_at_command_full_finish (MMBaseModem *self,
                                      GAsyncResult *res,
                                      GError **error)
{
    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return NULL;

    return g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res));
}

static void
at_command_parse_response (MMAtSerialPort *port,
                           GString *response,
                           GError *error,
                           AtCommandContext *ctx)
{
    /* Cancelled? */
    if (g_cancellable_is_cancelled (ctx->cancellable))
        g_simple_async_result_set_error (ctx->result,
                                         MM_CORE_ERROR,
                                         MM_CORE_ERROR_CANCELLED,
                                         "AT command was cancelled");

    /* Error coming from the serial port? */
    else if (error)
        g_simple_async_result_set_from_error (ctx->result, error);

    /* Valid string response */
    else if (response && response->str)
        g_simple_async_result_set_op_res_gpointer (ctx->result,
                                                   g_strdup (response->str),
                                                   g_free);

    /* No response */
    else
        g_simple_async_result_set_op_res_gpointer (ctx->result, NULL, NULL);

    g_simple_async_result_complete (ctx->result);
    at_command_context_free (ctx);
}

void
mm_base_modem_at_command_full (MMBaseModem *self,
                               MMAtSerialPort *port,
                               const gchar *command,
                               guint timeout,
                               gboolean allow_cached,
                               gboolean is_raw,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
    AtCommandContext *ctx;

    /* Ensure that we have an open port */
    if (!abort_async_if_port_unusable (self, port, callback, user_data))
        return;

    ctx = g_new0 (AtCommandContext, 1);
    ctx->self = g_object_ref (self);
    ctx->port = g_object_ref (port);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             mm_base_modem_at_command_full);

    /* Setup cancellables */
    ctx->modem_cancellable = mm_base_modem_get_cancellable (self);
    ctx->user_cancellable = cancellable ? g_object_ref (cancellable) : NULL;
    if (!ctx->user_cancellable)
        /* Just the modem-wide one, use it directly */
        ctx->cancellable = g_object_ref (ctx->modem_cancellable);
    else {
        /* Use the user provided one, which will also get cancelled if the modem
         * wide-one gets cancelled */
        ctx->cancellable = g_object_ref (ctx->user_cancellable);
        ctx->cancelled_id = g_cancellable_connect (ctx->modem_cancellable,
                                                   G_CALLBACK (modem_cancellable_cancelled),
                                                   ctx->user_cancellable,
                                                   NULL);
    }

    /* Go on with the command */
    if (allow_cached)
        mm_at_serial_port_queue_command_cached (
            port,
            command,
            timeout,
            is_raw,
            ctx->cancellable,
            (MMAtSerialResponseFn)at_command_parse_response,
            ctx);
    else
        mm_at_serial_port_queue_command (
            port,
            command,
            timeout,
            is_raw,
            ctx->cancellable,
            (MMAtSerialResponseFn)at_command_parse_response,
            ctx);
}

const gchar *
mm_base_modem_at_command_finish (MMBaseModem *self,
                                 GAsyncResult *res,
                                 GError **error)
{
    return mm_base_modem_at_command_full_finish (self, res, error);
}

static void
_at_command (MMBaseModem *self,
             const gchar *command,
             guint timeout,
             gboolean allow_cached,
             gboolean is_raw,
             GAsyncReadyCallback callback,
             gpointer user_data)
{
    MMAtSerialPort *port;
    GError *error = NULL;

    /* No port given, so we'll try to guess which is best */
    port = mm_base_modem_peek_best_at_port (self, &error);
    if (!port) {
        g_assert (error != NULL);
        g_simple_async_report_take_gerror_in_idle (G_OBJECT (self),
                                                   callback,
                                                   user_data,
                                                   error);
        return;
    }

    mm_base_modem_at_command_full (self,
                                   port,
                                   command,
                                   timeout,
                                   allow_cached,
                                   is_raw,
                                   NULL,
                                   callback,
                                   user_data);
}

void
mm_base_modem_at_command (MMBaseModem *self,
                          const gchar *command,
                          guint timeout,
                          gboolean allow_cached,
                          GAsyncReadyCallback callback,
                          gpointer user_data)
{
    _at_command (self, command, timeout, allow_cached, FALSE, callback, user_data);
}

void
mm_base_modem_at_command_raw (MMBaseModem *self,
                              const gchar *command,
                              guint timeout,
                              gboolean allow_cached,
                              GAsyncReadyCallback callback,
                              gpointer user_data)
{
    _at_command (self, command, timeout, allow_cached, TRUE, callback, user_data);
}
