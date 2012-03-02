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
 * Copyright (C) 2012 Google, Inc.
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#include <ModemManager.h>
#include <libmm-common.h>

#include "mm-base-modem-at.h"
#include "mm-broadband-bearer-samsung.h"
#include "mm-log.h"
#include "mm-modem-helpers.h"
#include "mm-utils.h"

G_DEFINE_TYPE (MMBroadbandBearerSamsung, mm_broadband_bearer_samsung, MM_TYPE_BROADBAND_BEARER);



/*****************************************************************************/


typedef struct {
    MMBroadbandBearerSamsung *self;
    MMBaseModem *modem;
    MMAtSerialPort *primary;
    GCancellable *cancellable;
    GSimpleAsyncResult *result;
    GError *saved_error;

    guint cid;
    guint timeout_id;
} DialContext;

struct _MMBroadbandBearerSamsungPrivate {
    guint cid;
    DialContext *pending_dial;
    GSimpleAsyncResult *pending_disconnect;
};

static DialContext *
dial_context_new (MMBroadbandBearerSamsung *self,
                  MMBaseModem *modem,
                  MMAtSerialPort *primary,
                  GCancellable *cancellable,
                  guint cid,
                  GAsyncReadyCallback callback,
                  gpointer user_data)
{
    DialContext *ctx;

    ctx = g_new0 (DialContext, 1);
    ctx->self = g_object_ref (self);
    ctx->modem = g_object_ref (modem);
    ctx->primary = g_object_ref (primary);
    ctx->cancellable = g_object_ref (cancellable);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             dial_context_new);
    ctx->cid = cid;
    ctx->timeout_id = 0;
    return ctx;
}

static void
dial_context_complete_and_free (DialContext *ctx)
{
    if (ctx->saved_error)
        g_error_free (ctx->saved_error);
    g_simple_async_result_complete_in_idle (ctx->result);
    g_object_unref (ctx->result);
    g_object_unref (ctx->cancellable);
    g_object_unref (ctx->primary);
    g_object_unref (ctx->modem);
    g_object_unref (ctx->self);
    g_free (ctx);
}

/*
 * "dial" steps:
 *    %IPDPCFG=<cid>,0,0,"",""
 * or %IPDPCFG=<cid>,0,1,"username","password"  (may need a retry)
 * %IPDPACT=<cid>,0 (optional, generates annoying error message)
 * %IPDPACT=<cid>,1
 * wait for unsolicited %IPDPACT=<cid>,1
 */

static gboolean
dial_3gpp_finish (MMBroadbandBearer *self,
                  GAsyncResult *res,
                  GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}


static gboolean
dial_3gpp_timeout (DialContext *ctx)
{

    g_simple_async_result_set_error (ctx->result,
                                     MM_SERIAL_ERROR,
                                     MM_SERIAL_ERROR_RESPONSE_TIMEOUT,
                                     "Timed out waiting for connection to complete");
    dial_context_complete_and_free (ctx);

    return FALSE;
}


static void
dial_3gpp_done (MMBroadbandBearerSamsung *self,
                DialContext *ctx)
{

    self->priv->cid = self->priv->pending_dial->cid;

    if (ctx->timeout_id) {
        g_source_remove (ctx->timeout_id);
        ctx->timeout_id = 0;
    }

    dial_context_complete_and_free (ctx);
    self->priv->pending_dial = NULL;
}

static void
disconnect_3gpp_done (MMBroadbandBearerSamsung *self,
                      GSimpleAsyncResult *result);

static void
ipdpact_received (MMAtSerialPort *port,
                  GMatchInfo *info,
                  MMBroadbandBearerSamsung *self)
{
    char *str;
    int cid, status;

    str = g_match_info_fetch (info, 1);
    g_return_if_fail (str != NULL);
    cid = atoi (str);
    g_free (str);

    if (cid != self->priv->cid) {
        mm_dbg ("Recieved %%IPDPACT message for CID other than the current one (%d).",
                self->priv->cid);
        return;
    }

    str = g_match_info_fetch (info, 2);
    g_return_if_fail (str != NULL);
    status = atoi (str);
    g_free (str);

    switch (status) {
        case 0:
            /* deactivated */
            if (self->priv->pending_disconnect == NULL) {
                mm_dbg ("Recieved %%IPDPACT disconnect while not disconnecting.");
                /* TODO(njw): How do we tell the system that this disconnected? */
                return;
            }
            disconnect_3gpp_done (self, self->priv->pending_disconnect);
            break;
        case 1:
            /* activated */
            if (self->priv->pending_dial == NULL) {
                mm_dbg ("Recieved %%IPDPACT connect while not connecting.");
                return;
            }
            dial_3gpp_done (self, self->priv->pending_dial);
            break;
        case 2:
            /* activating */
            break;
        case 3:
            /* activation failed */
            break;
        default:
            mm_warn ("Unknown connect status %d", status);
            break;
    }
}

static void
dial_3gpp_wait (MMBaseModem *modem,
                GAsyncResult *res,
                DialContext *ctx)
{
    mm_base_modem_at_command_finish (modem, res, &ctx->saved_error);

    if (ctx->saved_error) {
        g_simple_async_result_take_error (ctx->result,
                                          ctx->saved_error);
        dial_context_complete_and_free (ctx);
        return;
    }

    /* Set a 60-second connection-failure timeout */
    ctx->timeout_id = g_timeout_add_seconds (60, (GSourceFunc)dial_3gpp_timeout, ctx);
}

static void
dial_3gpp_activate (MMBaseModem *modem,
                    GAsyncResult *res,
                    DialContext *ctx)
{
    gchar *command;

    mm_base_modem_at_command_finish (modem, res, &ctx->saved_error);

    if (ctx->saved_error) {
        /* TODO(njw): retry up to 3 times with a 1-second delay */
        /* Return an error */
        g_simple_async_result_take_error (ctx->result,
                                          ctx->saved_error);
        dial_context_complete_and_free (ctx);
        return;
    }

    command = g_strdup_printf ("%%IPDPACT=%d,1", ctx->cid);
    mm_base_modem_at_command_in_port (
        ctx->modem,
        ctx->primary,
        command,
        60,
        FALSE,
        NULL, /* cancellable */
        (GAsyncReadyCallback)dial_3gpp_wait,
        ctx);
    g_free (command);

    /* The unsolicited response to %IPDPACT may come before the OK does */
    ctx->self->priv->pending_dial = ctx;
    ctx->self->priv->cid = ctx->cid;
}

static void
dial_3gpp (MMBroadbandBearer *self,
           MMBaseModem *modem,
           MMAtSerialPort *primary,
           guint cid,
           GCancellable *cancellable,
           GAsyncReadyCallback callback,
           gpointer user_data)
{
    DialContext *ctx;
    gchar *command;

    ctx = dial_context_new (MM_BROADBAND_BEARER_SAMSUNG (self),
                            modem,
                            primary,
                            cancellable,
                            cid,
                            callback,
                            user_data);

    /* TODO(njw): fetch user/password properties and use if present */
    command = g_strdup_printf ("%%IPDPCFG=%d,0,0,\"\",\"\"", cid);
    mm_base_modem_at_command_in_port (
        ctx->modem,
        ctx->primary,
        command,
        60,
        FALSE,
        NULL, /* cancellable */
        (GAsyncReadyCallback)dial_3gpp_activate,
        ctx);
    g_free (command);
}

static gboolean
disconnect_3gpp_finish (MMBroadbandBearer *self,
                        GAsyncResult *res,
                        GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
disconnect_3gpp_done (MMBroadbandBearerSamsung *self,
                      GSimpleAsyncResult *result)
{
    g_simple_async_result_set_op_res_gboolean (result, TRUE);
    g_simple_async_result_complete_in_idle (result);

    self->priv->pending_disconnect = NULL;
    self->priv->cid = 0;
}

static void
disconnect_3gpp_ready (MMBaseModem *modem,
                       GAsyncResult *res,
                       GSimpleAsyncResult *result)
{
    GError *error = NULL;

    mm_base_modem_at_command_finish (MM_BASE_MODEM (modem), res, &error);
    if (error) {
        mm_dbg ("PDP context deactivation failed (not fatal): %s", error->message);
        g_error_free (error);
    }
}

static void
disconnect_3gpp (MMBroadbandBearer *bearer,
                 MMBroadbandModem *modem,
                 MMAtSerialPort *primary,
                 MMAtSerialPort *secondary,
                 MMPort *data,
                 GAsyncReadyCallback callback,
                 gpointer user_data)
{
    MMBroadbandBearerSamsung *self = MM_BROADBAND_BEARER_SAMSUNG (bearer);
    gchar *command;
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        disconnect_3gpp);

    command = g_strdup_printf ("%%IPDPACT=%d,0", self->priv->cid);
    mm_base_modem_at_command_in_port (
        MM_BASE_MODEM (modem),
        primary,
        command,
        60,
        FALSE,
        NULL, /* cancellable */
        (GAsyncReadyCallback)disconnect_3gpp_ready,
        result);
    g_free (command);

    self->priv->pending_disconnect = result;
}

static void
set_unsolicited_result_codes (MMBroadbandBearerSamsung *self, gboolean enable)
{
    MMBroadbandModemSamsung *modem;
    MMAtSerialPort *ports[2];
    GRegex *ipdpact_regex;
    guint i;

    g_object_get (self,
                  MM_BEARER_MODEM, &modem,
                  NULL);
    g_assert (modem != NULL);

    ipdpact_regex = g_regex_new(
        "\\r\\n%IPDPACT:\\s*(\\d+),\\s*(\\d+),\\s*(\\d+)\\r\\n",
        G_REGEX_RAW | G_REGEX_OPTIMIZE,
        0,
        NULL);

    ports[0] = mm_base_modem_get_port_primary (MM_BASE_MODEM (modem));
    ports[1] = mm_base_modem_get_port_secondary (MM_BASE_MODEM (modem));
    for (i = 0; ports[i] && i < 2; i++) {
        mm_at_serial_port_add_unsolicited_msg_handler (
            ports[i],
            ipdpact_regex,
            enable ? (MMAtSerialUnsolicitedMsgFn) ipdpact_received : NULL,
            enable ? self : NULL,
            NULL);
    }
    g_object_unref (modem);
    g_regex_unref (ipdpact_regex);
}

static void
finalize (GObject *object)
{
    MMBroadbandBearerSamsung *self = MM_BROADBAND_BEARER_SAMSUNG (object);

    set_unsolicited_result_codes (self, FALSE);

    G_OBJECT_CLASS (mm_broadband_bearer_samsung_parent_class)->finalize (object);
}

static void
mm_broadband_bearer_samsung_init (MMBroadbandBearerSamsung *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE ((self),
                                              MM_TYPE_BROADBAND_BEARER_SAMSUNG,
                                              MMBroadbandBearerSamsungPrivate);

    /* Set defaults */
    self->priv->cid = 0;
    self->priv->pending_dial = NULL;
    self->priv->pending_disconnect = NULL;
}

static void
mm_broadband_bearer_samsung_class_init (MMBroadbandBearerSamsungClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    MMBroadbandBearerClass *bearer_class = MM_BROADBAND_BEARER_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMBroadbandBearerSamsungPrivate));

    /* Virtual methods */
    bearer_class->dial_3gpp = dial_3gpp;
    bearer_class->dial_3gpp_finish = dial_3gpp_finish;

    bearer_class->disconnect_3gpp = disconnect_3gpp;
    bearer_class->disconnect_3gpp_finish = disconnect_3gpp_finish;

    object_class->finalize = finalize;
}

MMBearer *
mm_broadband_bearer_samsung_new_finish (GAsyncResult *res,
                                        GError **error)
{
    GObject *source;
    GObject *bearer;

    source = g_async_result_get_source_object (res);
    bearer = g_async_initable_new_finish (G_ASYNC_INITABLE (source), res, error);
    g_object_unref (source);

    if (!bearer)
        return NULL;

    set_unsolicited_result_codes (MM_BROADBAND_BEARER_SAMSUNG (bearer), TRUE);

    /* Only export valid bearers */
    mm_bearer_export (MM_BEARER (bearer));

    return MM_BEARER (bearer);
}

void mm_broadband_bearer_samsung_new (MMBroadbandModemSamsung *modem,
                                      MMBearerProperties *properties,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data)
{
    g_async_initable_new_async (
        MM_TYPE_BROADBAND_BEARER_SAMSUNG,
        G_PRIORITY_DEFAULT,
        cancellable,
        callback,
        user_data,
        MM_BEARER_MODEM, modem,
        NULL);
}
