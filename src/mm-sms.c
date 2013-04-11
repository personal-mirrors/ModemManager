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
 * Copyright (C) 2008 - 2009 Novell, Inc.
 * Copyright (C) 2009 - 2012 Red Hat, Inc.
 * Copyright (C) 2012 Google, Inc.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-broadband-modem.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-messaging.h"
#include "mm-sms.h"
#include "mm-base-modem-at.h"
#include "mm-base-modem.h"
#include "mm-log.h"
#include "mm-modem-helpers.h"

G_DEFINE_TYPE (MMSms, mm_sms, MM_GDBUS_TYPE_SMS_SKELETON);

enum {
    PROP_0,
    PROP_PATH,
    PROP_CONNECTION,
    PROP_MODEM,
    PROP_IS_MULTIPART,
    PROP_MAX_PARTS,
    PROP_MULTIPART_REFERENCE,
    PROP_LAST
};

static GParamSpec *properties[PROP_LAST];

struct _MMSmsPrivate {
    /* The connection to the system bus */
    GDBusConnection *connection;
    /* The modem which owns this SMS */
    MMBaseModem *modem;
    /* The path where the SMS object is exported */
    gchar *path;

    /* Multipart SMS specific stuff */
    gboolean is_multipart;
    guint multipart_reference;

    /* List of SMS parts */
    guint max_parts;
    GList *parts;

    /* Set to true when all needed parts were received,
     * parsed and assembled */
    gboolean is_assembled;
};

/*****************************************************************************/

static guint
get_validity_relative (GVariant *tuple)
{
    guint type;
    GVariant *value;
    guint value_integer = 0;

    if (!tuple)
        return 0;

    g_variant_get (tuple, "(ub)", &type, &value);

    if (type == MM_SMS_VALIDITY_TYPE_RELATIVE)
        value_integer = g_variant_get_uint32 (value);
    else
        /* Currently not supported other than relative */
        g_warn_if_reached ();

    g_variant_unref (value);

    return value_integer;
}

static gboolean
generate_submit_pdus (MMSms *self,
                      GError **error)
{
    guint i;
    guint n_parts;

    const gchar *text;
    GVariant *data_variant;
    const guint8 *data;
    gsize data_len = 0;

    MMSmsEncoding encoding;
    gchar **split_text = NULL;
    GByteArray **split_data = NULL;

    g_assert (self->priv->parts == NULL);

    text = mm_gdbus_sms_get_text (MM_GDBUS_SMS (self));
    data_variant = mm_gdbus_sms_get_data (MM_GDBUS_SMS (self));
    data = (data_variant ?
            g_variant_get_fixed_array (data_variant,
                                       &data_len,
                                       sizeof (guchar)) :
            NULL);

    g_assert (text != NULL || data != NULL);
    g_assert (!(text != NULL && data != NULL));

    if (text) {
        split_text = mm_sms_part_util_split_text (text, &encoding);
        if (!split_text) {
            g_set_error (error,
                         MM_CORE_ERROR,
                         MM_CORE_ERROR_INVALID_ARGS,
                         "Cannot generate PDUs: Error processing input text");
            return FALSE;
        }
        n_parts = g_strv_length (split_text);
    } else if (data) {
        encoding = MM_SMS_ENCODING_8BIT;
        split_data = mm_sms_part_util_split_data (data, data_len);
        g_assert (split_data != NULL);
        /* noop within the for */
        for (n_parts = 0; split_data[n_parts]; n_parts++);
    } else
        g_assert_not_reached ();

    g_assert (split_text != NULL || split_data != NULL);
    g_assert (!(split_text != NULL && split_data != NULL));

    if (n_parts > 255) {
        if (split_text)
            g_strfreev (split_text);
        else if (split_data) {
            guint i = 0;

            while (split_data[i])
                g_byte_array_unref (split_data[i++]);
            g_free (split_data);
        }

        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_TOO_MANY,
                     "Cannot generate PDUs: Text or Data too long");
        return FALSE;
    }

    /* Loop text/data chunks */
    i = 0;
    while (1) {
        MMSmsPart *part;
        gchar *part_text = NULL;
        GByteArray *part_data = NULL;

        if (split_text) {
            if (!split_text[i])
                break;
            part_text = split_text[i];
            mm_dbg ("  Processing chunk '%u' of text with '%u' bytes",
                    i, (guint) strlen (part_text));
        } else if (split_data) {
            if (!split_data[i])
                break;
            part_data = split_data[i];
            mm_dbg ("  Processing chunk '%u' of data with '%u' bytes",
                    i, part_data->len);

        } else
            g_assert_not_reached ();

        /* Create new part */
        part = mm_sms_part_new (SMS_PART_INVALID_INDEX, MM_SMS_PDU_TYPE_SUBMIT);
        mm_sms_part_take_text (part, part_text);
        mm_sms_part_take_data (part, part_data);
        mm_sms_part_set_encoding (part, encoding);
        mm_sms_part_set_number (part, mm_gdbus_sms_get_number (MM_GDBUS_SMS (self)));
        mm_sms_part_set_smsc (part, mm_gdbus_sms_get_smsc (MM_GDBUS_SMS (self)));
        mm_sms_part_set_validity_relative (part, get_validity_relative (mm_gdbus_sms_get_validity (MM_GDBUS_SMS (self))));
        mm_sms_part_set_class (part, mm_gdbus_sms_get_class (MM_GDBUS_SMS (self)));
        mm_sms_part_set_delivery_report_request (part, mm_gdbus_sms_get_delivery_report_request (MM_GDBUS_SMS (self)));

        if (n_parts > 1) {
            mm_sms_part_set_concat_reference (part, 0); /* We don't set a concat reference here */
            mm_sms_part_set_concat_sequence (part, i + 1);
            mm_sms_part_set_concat_max (part, n_parts);

            mm_dbg ("Created SMS part '%u' for multipart SMS ('%u' parts expected)",
                    i + 1, n_parts);
        } else {
            mm_dbg ("Created SMS part for singlepart SMS");
        }

        /* Add to the list of parts */
        self->priv->parts = g_list_append (self->priv->parts, part);

        i++;
    }

    /* Free array (not contents, which were taken for the part) */
    if (split_text)
        g_free (split_text);
    if (split_data)
        g_free (split_data);

    /* Set additional multipart specific properties */
    if (n_parts > 1) {
        self->priv->is_multipart = TRUE;
        self->priv->max_parts = n_parts;
    }

    /* No more parts are expected */
    self->priv->is_assembled = TRUE;

    return TRUE;
}

/*****************************************************************************/
/* Store SMS (DBus call handling) */

typedef struct {
    MMSms *self;
    MMBaseModem *modem;
    GDBusMethodInvocation *invocation;
    MMSmsStorage storage;
} HandleStoreContext;

static void
handle_store_context_free (HandleStoreContext *ctx)
{
    g_object_unref (ctx->invocation);
    g_object_unref (ctx->modem);
    g_object_unref (ctx->self);
    g_free (ctx);
}

static void
handle_store_ready (MMSms *self,
                    GAsyncResult *res,
                    HandleStoreContext *ctx)
{
    GError *error = NULL;

    if (!MM_SMS_GET_CLASS (self)->store_finish (self, res, &error))
        g_dbus_method_invocation_take_error (ctx->invocation, error);
    else {
        mm_gdbus_sms_set_storage (MM_GDBUS_SMS (ctx->self), ctx->storage);

        /* Transition from Unknown->Stored for SMS which were created by the user */
        if (mm_gdbus_sms_get_state (MM_GDBUS_SMS (ctx->self)) == MM_SMS_STATE_UNKNOWN)
            mm_gdbus_sms_set_state (MM_GDBUS_SMS (ctx->self), MM_SMS_STATE_STORED);

        mm_gdbus_sms_complete_store (MM_GDBUS_SMS (ctx->self), ctx->invocation);
    }

    handle_store_context_free (ctx);
}

static gboolean
prepare_sms_to_be_stored (MMSms *self,
                          GError **error)
{
    GList *l;
    guint8 reference;

    g_assert (self->priv->parts == NULL);

    /* Look for a valid multipart reference to use. When storing, we need to
     * check whether we have already stored multipart SMS with the same
     * reference and destination number */
    reference = (mm_iface_modem_messaging_get_local_multipart_reference (
                     MM_IFACE_MODEM_MESSAGING (self->priv->modem),
                     mm_gdbus_sms_get_number (MM_GDBUS_SMS (self)),
                     error));
    if (!reference ||
        !generate_submit_pdus (self, error)) {
        g_prefix_error (error, "Cannot prepare SMS to be stored: ");
        return FALSE;
    }

    /* If the message is a multipart message, we need to set a proper
     * multipart reference. When sending a message which wasn't stored
     * yet, we can just get a random multipart reference. */
    self->priv->multipart_reference = reference;
    for (l = self->priv->parts; l; l = g_list_next (l)) {
        mm_sms_part_set_concat_reference ((MMSmsPart *)l->data,
                                          self->priv->multipart_reference);
    }

    return TRUE;
}

static void
handle_store_auth_ready (MMBaseModem *modem,
                         GAsyncResult *res,
                         HandleStoreContext *ctx)
{
    GError *error = NULL;

    if (!mm_base_modem_authorize_finish (modem, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_store_context_free (ctx);
        return;
    }

    /* First of all, check if we already have the SMS stored. */
    if (mm_sms_get_storage (ctx->self) != MM_SMS_STORAGE_UNKNOWN) {
        /* Check if SMS stored in some other storage */
        if (mm_sms_get_storage (ctx->self) == ctx->storage)
            /* Good, same storage */
            mm_gdbus_sms_complete_store (MM_GDBUS_SMS (ctx->self), ctx->invocation);
        else
            g_dbus_method_invocation_return_error (
                ctx->invocation,
                MM_CORE_ERROR,
                MM_CORE_ERROR_FAILED,
                "SMS is already stored in storage '%s', cannot store it in storage '%s'",
                mm_sms_storage_get_string (mm_sms_get_storage (ctx->self)),
                mm_sms_storage_get_string (ctx->storage));
        handle_store_context_free (ctx);
        return;
    }

    /* Check if the requested storage is allowed for storing */
    if (!mm_iface_modem_messaging_is_storage_supported_for_storing (MM_IFACE_MODEM_MESSAGING (ctx->modem),
                                                                    ctx->storage,
                                                                    &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_store_context_free (ctx);
        return;
    }

    /* Prepare the SMS to be stored, creating the PDU list if required */
    if (!prepare_sms_to_be_stored (ctx->self, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_store_context_free (ctx);
        return;
    }

    /* If not stored, check if we do support doing it */
    if (!MM_SMS_GET_CLASS (ctx->self)->store ||
        !MM_SMS_GET_CLASS (ctx->self)->store_finish) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_UNSUPPORTED,
                                               "Storing SMS is not supported by this modem");
        handle_store_context_free (ctx);
        return;
    }

    MM_SMS_GET_CLASS (ctx->self)->store (ctx->self,
                                         ctx->storage,
                                         (GAsyncReadyCallback)handle_store_ready,
                                         ctx);
}

static gboolean
handle_store (MMSms *self,
              GDBusMethodInvocation *invocation,
              guint32 storage)
{
    HandleStoreContext *ctx;

    ctx = g_new0 (HandleStoreContext, 1);
    ctx->self = g_object_ref (self);
    ctx->invocation = g_object_ref (invocation);
    g_object_get (self,
                  MM_SMS_MODEM, &ctx->modem,
                  NULL);
    ctx->storage = (MMSmsStorage)storage;

    if (ctx->storage == MM_SMS_STORAGE_UNKNOWN) {
        /* We'll set now the proper storage, taken from the default mem2 one */
        g_object_get (self->priv->modem,
                      MM_IFACE_MODEM_MESSAGING_SMS_DEFAULT_STORAGE, &ctx->storage,
                      NULL);
        g_assert (ctx->storage != MM_SMS_STORAGE_UNKNOWN);
    }

    mm_base_modem_authorize (ctx->modem,
                             invocation,
                             MM_AUTHORIZATION_MESSAGING,
                             (GAsyncReadyCallback)handle_store_auth_ready,
                             ctx);
    return TRUE;
}

/*****************************************************************************/
/* Send SMS (DBus call handling) */

typedef struct {
    MMSms *self;
    MMBaseModem *modem;
    GDBusMethodInvocation *invocation;
} HandleSendContext;

static void
handle_send_context_free (HandleSendContext *ctx)
{
    g_object_unref (ctx->invocation);
    g_object_unref (ctx->modem);
    g_object_unref (ctx->self);
    g_free (ctx);
}

static void
handle_send_ready (MMSms *self,
                   GAsyncResult *res,
                   HandleSendContext *ctx)
{
    GError *error = NULL;

    if (!MM_SMS_GET_CLASS (self)->send_finish (self, res, &error))
        g_dbus_method_invocation_take_error (ctx->invocation, error);
    else {
        /* Transition from Unknown->Sent or Stored->Sent */
        if (mm_gdbus_sms_get_state (MM_GDBUS_SMS (ctx->self)) == MM_SMS_STATE_UNKNOWN ||
            mm_gdbus_sms_get_state (MM_GDBUS_SMS (ctx->self)) == MM_SMS_STATE_STORED) {
            GList *l;

            /* Update state */
            mm_gdbus_sms_set_state (MM_GDBUS_SMS (ctx->self), MM_SMS_STATE_SENT);
            /* Grab last message reference */
            l = g_list_last (mm_sms_get_parts (ctx->self));
            mm_gdbus_sms_set_message_reference (MM_GDBUS_SMS (ctx->self),
                                                mm_sms_part_get_message_reference ((MMSmsPart *)l->data));
        }
        mm_gdbus_sms_complete_send (MM_GDBUS_SMS (ctx->self), ctx->invocation);
    }

    handle_send_context_free (ctx);
}

static gboolean
prepare_sms_to_be_sent (MMSms *self,
                        GError **error)
{
    GList *l;

    if (self->priv->parts)
        return TRUE;

    if (!generate_submit_pdus (self, error)) {
        g_prefix_error (error, "Cannot prepare SMS to be sent: ");
        return FALSE;
    }

    /* If the message is a multipart message, we need to set a proper
     * multipart reference. When sending a message which wasn't stored
     * yet, we can just get a random multipart reference. */
    self->priv->multipart_reference = g_random_int_range (1,255);
    for (l = self->priv->parts; l; l = g_list_next (l)) {
        mm_sms_part_set_concat_reference ((MMSmsPart *)l->data,
                                          self->priv->multipart_reference);
    }

    return TRUE;
}

static void
handle_send_auth_ready (MMBaseModem *modem,
                        GAsyncResult *res,
                        HandleSendContext *ctx)
{
    MMSmsState state;
    GError *error = NULL;

    if (!mm_base_modem_authorize_finish (modem, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_send_context_free (ctx);
        return;
    }

    /* We can only send SMS created by the user */
    state = mm_gdbus_sms_get_state (MM_GDBUS_SMS (ctx->self));
    if (state == MM_SMS_STATE_RECEIVED ||
        state == MM_SMS_STATE_RECEIVING) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_FAILED,
                                               "This SMS was received, cannot send it");
        handle_send_context_free (ctx);
        return;
    }

    /* Don't allow sending the same SMS multiple times, we would lose the message reference */
    if (state == MM_SMS_STATE_SENT) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_FAILED,
                                               "This SMS was already sent, cannot send it again");
        handle_send_context_free (ctx);
        return;
    }

    /* Prepare the SMS to be sent, creating the PDU list if required */
    if (!prepare_sms_to_be_sent (ctx->self, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_send_context_free (ctx);
        return;
    }

    /* Check if we do support doing it */
    if (!MM_SMS_GET_CLASS (ctx->self)->send ||
        !MM_SMS_GET_CLASS (ctx->self)->send_finish) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_UNSUPPORTED,
                                               "Sending SMS is not supported by this modem");
        handle_send_context_free (ctx);
        return;
    }

    MM_SMS_GET_CLASS (ctx->self)->send (ctx->self,
                                        (GAsyncReadyCallback)handle_send_ready,
                                        ctx);
}

static gboolean
handle_send (MMSms *self,
             GDBusMethodInvocation *invocation)
{
    HandleSendContext *ctx;

    ctx = g_new0 (HandleSendContext, 1);
    ctx->self = g_object_ref (self);
    ctx->invocation = g_object_ref (invocation);
    g_object_get (self,
                  MM_SMS_MODEM, &ctx->modem,
                  NULL);

    mm_base_modem_authorize (ctx->modem,
                             invocation,
                             MM_AUTHORIZATION_MESSAGING,
                             (GAsyncReadyCallback)handle_send_auth_ready,
                             ctx);
    return TRUE;
}

/*****************************************************************************/

void
mm_sms_export (MMSms *self)
{
    static guint id = 0;
    gchar *path;

    path = g_strdup_printf (MM_DBUS_SMS_PREFIX "/%d", id++);
    g_object_set (self,
                  MM_SMS_PATH, path,
                  NULL);
    g_free (path);
}

void
mm_sms_unexport (MMSms *self)
{
    g_object_set (self,
                  MM_SMS_PATH, NULL,
                  NULL);
}

/*****************************************************************************/

static void
mm_sms_dbus_export (MMSms *self)
{
    GError *error = NULL;

    /* Handle method invocations */
    g_signal_connect (self,
                      "handle-store",
                      G_CALLBACK (handle_store),
                      NULL);
    g_signal_connect (self,
                      "handle-send",
                      G_CALLBACK (handle_send),
                      NULL);

    if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self),
                                           self->priv->connection,
                                           self->priv->path,
                                           &error)) {
        mm_warn ("couldn't export SMS at '%s': '%s'",
                 self->priv->path,
                 error->message);
        g_error_free (error);
    }
}

static void
mm_sms_dbus_unexport (MMSms *self)
{
    /* Only unexport if currently exported */
    if (g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON (self)))
        g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self));
}

/*****************************************************************************/

const gchar *
mm_sms_get_path (MMSms *self)
{
    return self->priv->path;
}

MMSmsStorage
mm_sms_get_storage (MMSms *self)
{
    return mm_gdbus_sms_get_storage (MM_GDBUS_SMS (self));
}

gboolean
mm_sms_is_multipart (MMSms *self)
{
    return self->priv->is_multipart;
}

guint
mm_sms_get_multipart_reference (MMSms *self)
{
    g_return_val_if_fail (self->priv->is_multipart, 0);

    return self->priv->multipart_reference;
}

gboolean
mm_sms_multipart_is_complete (MMSms *self)
{
    return (g_list_length (self->priv->parts) == self->priv->max_parts);
}

gboolean
mm_sms_multipart_is_assembled (MMSms *self)
{
    return self->priv->is_assembled;
}

/*****************************************************************************/

static guint
cmp_sms_part_index (MMSmsPart *part,
                    gpointer user_data)
{
    return (GPOINTER_TO_UINT (user_data) - mm_sms_part_get_index (part));
}

gboolean
mm_sms_has_part_index (MMSms *self,
                       guint index)
{
    return !!g_list_find_custom (self->priv->parts,
                                 GUINT_TO_POINTER (index),
                                 (GCompareFunc)cmp_sms_part_index);
}

GList *
mm_sms_get_parts (MMSms *self)
{
    return self->priv->parts;
}

/*****************************************************************************/

static gboolean
sms_get_store_or_send_command (MMSmsPart *part,
                               gboolean text_or_pdu,   /* TRUE for PDU */
                               gboolean store_or_send, /* TRUE for send */
                               gchar **out_cmd,
                               gchar **out_msg_data,
                               GError **error)
{
    g_assert (out_cmd != NULL);
    g_assert (out_msg_data != NULL);

    if (!text_or_pdu) {
        /* Text mode */
        *out_cmd = g_strdup_printf ("+CMG%c=\"%s\"",
                                    store_or_send ? 'S' : 'W',
                                    mm_sms_part_get_number (part));
        *out_msg_data = g_strdup_printf ("%s\x1a", mm_sms_part_get_text (part));
    } else {
        guint8 *pdu;
        guint pdulen = 0;
        guint msgstart = 0;
        gchar *hex;

        /* AT+CMGW=<length>[, <stat>]<CR> PDU can be entered. <CTRL-Z>/<ESC> */

        pdu = mm_sms_part_get_submit_pdu (part, &pdulen, &msgstart, error);
        if (!pdu)
            /* 'error' should already be set */
            return FALSE;

        /* Convert PDU to hex */
        hex = mm_utils_bin2hexstr (pdu, pdulen);
        g_free (pdu);

        if (!hex) {
            g_set_error (error,
                         MM_CORE_ERROR,
                         MM_CORE_ERROR_FAILED,
                         "Not enough memory to send SMS PDU");
            return FALSE;
        }

        /* CMGW/S length is the size of the PDU without SMSC information */
        *out_cmd = g_strdup_printf ("+CMG%c=%d",
                                    store_or_send ? 'S' : 'W',
                                    pdulen - msgstart);
        *out_msg_data = g_strdup_printf ("%s\x1a", hex);
        g_free (hex);
    }

    return TRUE;
}

/*****************************************************************************/
/* Store the SMS */

typedef struct {
    MMSms *self;
    MMBaseModem *modem;
    GSimpleAsyncResult *result;
    MMSmsStorage storage;
    gboolean need_unlock;
    gboolean use_pdu_mode;
    GList *current;
    gchar *msg_data;
} SmsStoreContext;

static void
sms_store_context_complete_and_free (SmsStoreContext *ctx)
{
    g_simple_async_result_complete (ctx->result);
    g_object_unref (ctx->result);
    /* Unlock mem2 storage if we had the lock */
    if (ctx->need_unlock)
        mm_broadband_modem_unlock_sms_storages (MM_BROADBAND_MODEM (ctx->modem), FALSE, TRUE);
    g_object_unref (ctx->modem);
    g_object_unref (ctx->self);
    g_free (ctx->msg_data);
    g_free (ctx);
}

static gboolean
sms_store_finish (MMSms *self,
                  GAsyncResult *res,
                  GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void sms_store_next_part (SmsStoreContext *ctx);

static void
store_msg_data_ready (MMBaseModem *modem,
                      GAsyncResult *res,
                      SmsStoreContext *ctx)
{
    const gchar *response;
    GError *error = NULL;
    gint rv;
    gint idx;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (modem), res, &error);
    if (error) {
        g_simple_async_result_take_error (ctx->result, error);
        sms_store_context_complete_and_free (ctx);
        return;
    }

    /* Read the new part index from the reply */
    rv = sscanf (response, "+CMGW: %d", &idx);
    if (rv != 1 || idx < 0) {
        g_simple_async_result_set_error (ctx->result,
                                         MM_CORE_ERROR,
                                         MM_CORE_ERROR_FAILED,
                                         "Couldn't read index of already stored part: "
                                         "%d fields parsed",
                                         rv);
        sms_store_context_complete_and_free (ctx);
        return;
    }

    /* Set the index in the part we hold */
    mm_sms_part_set_index ((MMSmsPart *)ctx->current->data, (guint)idx);

    ctx->current = g_list_next (ctx->current);
    sms_store_next_part (ctx);
}

static void
store_ready (MMBaseModem *modem,
             GAsyncResult *res,
             SmsStoreContext *ctx)
{
    const gchar *response;
    GError *error = NULL;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (modem), res, &error);
    if (error) {
        g_simple_async_result_take_error (ctx->result, error);
        sms_store_context_complete_and_free (ctx);
        return;
    }

    /* Send the actual message data */
    mm_base_modem_at_command_raw (ctx->modem,
                                  ctx->msg_data,
                                  10,
                                  FALSE,
                                  (GAsyncReadyCallback)store_msg_data_ready,
                                  ctx);
}

static void
sms_store_next_part (SmsStoreContext *ctx)
{
    gchar *cmd;
    GError *error = NULL;

    if (!ctx->current) {
        /* Done we are */
        g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
        sms_store_context_complete_and_free (ctx);
        return;
    }

    if (ctx->msg_data) {
        g_free (ctx->msg_data);
        ctx->msg_data = NULL;
    }

    if (!sms_get_store_or_send_command ((MMSmsPart *)ctx->current->data,
                                        ctx->use_pdu_mode,
                                        FALSE,
                                        &cmd,
                                        &ctx->msg_data,
                                        &error)) {
        g_simple_async_result_take_error (ctx->result, error);
        sms_store_context_complete_and_free (ctx);
        return;
    }

    g_assert (cmd != NULL);
    g_assert (ctx->msg_data != NULL);

    mm_base_modem_at_command (ctx->modem,
                              cmd,
                              10,
                              FALSE,
                              (GAsyncReadyCallback)store_ready,
                              ctx);
    g_free (cmd);
}

static void
store_lock_sms_storages_ready (MMBroadbandModem *modem,
                               GAsyncResult *res,
                               SmsStoreContext *ctx)
{
    GError *error = NULL;

    if (!mm_broadband_modem_lock_sms_storages_finish (modem, res, &error)) {
        g_simple_async_result_take_error (ctx->result, error);
        sms_store_context_complete_and_free (ctx);
        return;
    }

    /* We are now locked. Whatever result we have here, we need to make sure
     * we unlock the storages before finishing. */
    ctx->need_unlock = TRUE;

    /* Go on to store the parts */
    ctx->current = ctx->self->priv->parts;
    sms_store_next_part (ctx);
}

static void
sms_store (MMSms *self,
           MMSmsStorage storage,
           GAsyncReadyCallback callback,
           gpointer user_data)
{
    SmsStoreContext *ctx;

    /* Setup the context */
    ctx = g_new0 (SmsStoreContext, 1);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             sms_store);
    ctx->self = g_object_ref (self);
    ctx->modem = g_object_ref (self->priv->modem);
    ctx->storage = storage;

    /* Different ways to do it if on PDU or text mode */
    g_object_get (self->priv->modem,
                  MM_IFACE_MODEM_MESSAGING_SMS_PDU_MODE, &ctx->use_pdu_mode,
                  NULL);

    /* First, lock storage to use */
    g_assert (MM_IS_BROADBAND_MODEM (self->priv->modem));
    mm_broadband_modem_lock_sms_storages (
        MM_BROADBAND_MODEM (self->priv->modem),
        MM_SMS_STORAGE_UNKNOWN, /* none required for mem1 */
        ctx->storage,
        (GAsyncReadyCallback)store_lock_sms_storages_ready,
        ctx);
}

/*****************************************************************************/
/* Send the SMS */

typedef struct {
    MMSms *self;
    MMBaseModem *modem;
    GSimpleAsyncResult *result;
    gboolean need_unlock;
    gboolean from_storage;
    gboolean use_pdu_mode;
    GList *current;
    gchar *msg_data;
} SmsSendContext;

static void
sms_send_context_complete_and_free (SmsSendContext *ctx)
{
    g_simple_async_result_complete_in_idle (ctx->result);
    g_object_unref (ctx->result);
    /* Unlock mem2 storage if we had the lock */
    if (ctx->need_unlock)
        mm_broadband_modem_unlock_sms_storages (MM_BROADBAND_MODEM (ctx->modem), FALSE, TRUE);
    g_object_unref (ctx->modem);
    g_object_unref (ctx->self);
    g_free (ctx->msg_data);
    g_free (ctx);
}

static gboolean
sms_send_finish (MMSms *self,
                 GAsyncResult *res,
                 GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void sms_send_next_part (SmsSendContext *ctx);

static gint
read_message_reference_from_reply (const gchar *response,
                                   GError **error)
{
    gint rv = 0;
    gint idx = -1;

    if (strstr (response, "+CMGS"))
        rv = sscanf (strstr (response, "+CMGS"), "+CMGS: %d", &idx);
    else if (strstr (response, "+CMSS"))
        rv = sscanf (strstr (response, "+CMSS"), "+CMSS: %d", &idx);

    if (rv != 1 || idx < 0) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Couldn't read message reference: "
                     "%d fields parsed from response '%s'",
                     rv, response);
        return -1;
    }

    return idx;
}

static void
send_generic_msg_data_ready (MMBaseModem *modem,
                             GAsyncResult *res,
                             SmsSendContext *ctx)
{
    GError *error = NULL;
    const gchar *response;
    gint message_reference;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (modem), res, &error);
    if (error) {
        g_simple_async_result_take_error (ctx->result, error);
        sms_send_context_complete_and_free (ctx);
        return;
    }

    message_reference = read_message_reference_from_reply (response, &error);
    if (error) {
        g_simple_async_result_take_error (ctx->result, error);
        sms_send_context_complete_and_free (ctx);
        return;
    }

    mm_sms_part_set_message_reference ((MMSmsPart *)ctx->current->data,
                                       (guint)message_reference);

    ctx->current = g_list_next (ctx->current);
    sms_send_next_part (ctx);
}

static void
send_generic_ready (MMBaseModem *modem,
                    GAsyncResult *res,
                    SmsSendContext *ctx)
{
    GError *error = NULL;

    mm_base_modem_at_command_finish (MM_BASE_MODEM (modem), res, &error);
    if (error) {
        g_simple_async_result_take_error (ctx->result, error);
        sms_send_context_complete_and_free (ctx);
        return;
    }

    /* Send the actual message data */
    mm_base_modem_at_command_raw (ctx->modem,
                                  ctx->msg_data,
                                  10,
                                  FALSE,
                                  (GAsyncReadyCallback)send_generic_msg_data_ready,
                                  ctx);
}

static void
send_from_storage_ready (MMBaseModem *modem,
                         GAsyncResult *res,
                         SmsSendContext *ctx)
{
    GError *error = NULL;
    const gchar *response;
    gint message_reference;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (modem), res, &error);
    if (error) {
        if (g_error_matches (error, MM_SERIAL_ERROR, MM_SERIAL_ERROR_RESPONSE_TIMEOUT)) {
            g_simple_async_result_take_error (ctx->result, error);
            sms_send_context_complete_and_free (ctx);
            return;
        }

        mm_dbg ("Couldn't send SMS from storage: '%s'; trying generic send...",
                error->message);
        g_error_free (error);

        ctx->from_storage = FALSE;
        sms_send_next_part (ctx);
        return;
    }

    message_reference = read_message_reference_from_reply (response, &error);
    if (error) {
        g_simple_async_result_take_error (ctx->result, error);
        sms_send_context_complete_and_free (ctx);
        return;
    }

    mm_sms_part_set_message_reference ((MMSmsPart *)ctx->current->data,
                                       (guint)message_reference);

    ctx->current = g_list_next (ctx->current);
    sms_send_next_part (ctx);
}

static void
sms_send_next_part (SmsSendContext *ctx)
{
    GError *error = NULL;
    gchar *cmd;

    if (!ctx->current) {
        /* Done we are */
        g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
        sms_send_context_complete_and_free (ctx);
        return;
    }

    /* Send from storage */
    if (ctx->from_storage) {
        cmd = g_strdup_printf ("+CMSS=%d",
                               mm_sms_part_get_index ((MMSmsPart *)ctx->current->data));
        mm_base_modem_at_command (ctx->modem,
                                  cmd,
                                  30,
                                  FALSE,
                                  (GAsyncReadyCallback)send_from_storage_ready,
                                  ctx);
        g_free (cmd);
        return;
    }

    /* Generic send */

    if (ctx->msg_data) {
        g_free (ctx->msg_data);
        ctx->msg_data = NULL;
    }

    if (!sms_get_store_or_send_command ((MMSmsPart *)ctx->current->data,
                                        ctx->use_pdu_mode,
                                        TRUE,
                                        &cmd,
                                        &ctx->msg_data,
                                        &error)) {
        g_simple_async_result_take_error (ctx->result, error);
        sms_send_context_complete_and_free (ctx);
        return;
    }

    g_assert (cmd != NULL);
    g_assert (ctx->msg_data != NULL);
    mm_base_modem_at_command (ctx->modem,
                              cmd,
                              30,
                              FALSE,
                              (GAsyncReadyCallback)send_generic_ready,
                              ctx);
    g_free (cmd);
}

static void
send_lock_sms_storages_ready (MMBroadbandModem *modem,
                              GAsyncResult *res,
                              SmsSendContext *ctx)
{
    GError *error = NULL;

    if (!mm_broadband_modem_lock_sms_storages_finish (modem, res, &error)) {
        g_simple_async_result_take_error (ctx->result, error);
        sms_send_context_complete_and_free (ctx);
        return;
    }

    /* We are now locked. Whatever result we have here, we need to make sure
     * we unlock the storages before finishing. */
    ctx->need_unlock = TRUE;

    /* Go on to send the parts */
    ctx->current = ctx->self->priv->parts;
    sms_send_next_part (ctx);
}

static void
sms_send (MMSms *self,
          GAsyncReadyCallback callback,
          gpointer user_data)
{
    SmsSendContext *ctx;

    /* Setup the context */
    ctx = g_new0 (SmsSendContext, 1);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             sms_send);
    ctx->self = g_object_ref (self);
    ctx->modem = g_object_ref (self->priv->modem);

    /* If the SMS is STORED, try to send from storage */
    ctx->from_storage = (mm_sms_get_storage (self) != MM_SMS_STORAGE_UNKNOWN);
    if (ctx->from_storage) {
        /* When sending from storage, first lock storage to use */
        g_assert (MM_IS_BROADBAND_MODEM (self->priv->modem));
        mm_broadband_modem_lock_sms_storages (
            MM_BROADBAND_MODEM (self->priv->modem),
            MM_SMS_STORAGE_UNKNOWN, /* none required for mem1 */
            mm_sms_get_storage (self),
            (GAsyncReadyCallback)send_lock_sms_storages_ready,
            ctx);
        return;
    }

    /* Different ways to do it if on PDU or text mode */
    g_object_get (self->priv->modem,
                  MM_IFACE_MODEM_MESSAGING_SMS_PDU_MODE, &ctx->use_pdu_mode,
                  NULL);
    ctx->current = self->priv->parts;
    sms_send_next_part (ctx);
}

/*****************************************************************************/

typedef struct {
    MMSms *self;
    MMBaseModem *modem;
    GSimpleAsyncResult *result;
    gboolean need_unlock;
    GList *current;
    guint n_failed;
} SmsDeletePartsContext;

static void
sms_delete_parts_context_complete_and_free (SmsDeletePartsContext *ctx)
{
    g_simple_async_result_complete_in_idle (ctx->result);
    g_object_unref (ctx->result);
    /* Unlock mem1 storage if we had the lock */
    if (ctx->need_unlock)
        mm_broadband_modem_unlock_sms_storages (MM_BROADBAND_MODEM (ctx->modem), TRUE, FALSE);
    g_object_unref (ctx->modem);
    g_object_unref (ctx->self);
    g_free (ctx);
}

static gboolean
sms_delete_finish (MMSms *self,
                   GAsyncResult *res,
                   GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void delete_next_part (SmsDeletePartsContext *ctx);

static void
delete_part_ready (MMBaseModem *modem,
                   GAsyncResult *res,
                   SmsDeletePartsContext *ctx)
{
    GError *error = NULL;

    mm_base_modem_at_command_finish (MM_BASE_MODEM (modem), res, &error);
    if (error) {
        ctx->n_failed++;
        mm_dbg ("Couldn't delete SMS part with index %u: '%s'",
                mm_sms_part_get_index ((MMSmsPart *)ctx->current->data),
                error->message);
        g_error_free (error);
    }

    /* We reset the index, as there is no longer that part */
    mm_sms_part_set_index ((MMSmsPart *)ctx->current->data, SMS_PART_INVALID_INDEX);

    ctx->current = g_list_next (ctx->current);
    delete_next_part (ctx);
}

static void
delete_next_part (SmsDeletePartsContext *ctx)
{
    gchar *cmd;

    /* Skip non-stored parts */
    while (ctx->current &&
           mm_sms_part_get_index ((MMSmsPart *)ctx->current->data) == SMS_PART_INVALID_INDEX)
        ctx->current = g_list_next (ctx->current);

    /* If all removed, we're done */
    if (!ctx->current) {
        if (ctx->n_failed > 0)
            g_simple_async_result_set_error (ctx->result,
                                             MM_CORE_ERROR,
                                             MM_CORE_ERROR_FAILED,
                                             "Couldn't delete %u parts from this SMS",
                                             ctx->n_failed);
        else
            g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);

        sms_delete_parts_context_complete_and_free (ctx);
        return;
    }

    cmd = g_strdup_printf ("+CMGD=%d",
                           mm_sms_part_get_index ((MMSmsPart *)ctx->current->data));
    mm_base_modem_at_command (ctx->modem,
                              cmd,
                              10,
                              FALSE,
                              (GAsyncReadyCallback)delete_part_ready,
                              ctx);
    g_free (cmd);
}

static void
delete_lock_sms_storages_ready (MMBroadbandModem *modem,
                                GAsyncResult *res,
                                SmsDeletePartsContext *ctx)
{
    GError *error = NULL;

    if (!mm_broadband_modem_lock_sms_storages_finish (modem, res, &error)) {
        g_simple_async_result_take_error (ctx->result, error);
        sms_delete_parts_context_complete_and_free (ctx);
        return;
    }

    /* We are now locked. Whatever result we have here, we need to make sure
     * we unlock the storages before finishing. */
    ctx->need_unlock = TRUE;

    /* Go on deleting parts */
    ctx->current = ctx->self->priv->parts;
    delete_next_part (ctx);
}

static void
sms_delete (MMSms *self,
            GAsyncReadyCallback callback,
            gpointer user_data)
{
    SmsDeletePartsContext *ctx;

    ctx = g_new0 (SmsDeletePartsContext, 1);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             sms_delete);
    ctx->self = g_object_ref (self);
    ctx->modem = g_object_ref (self->priv->modem);

    if (mm_sms_get_storage (self) == MM_SMS_STORAGE_UNKNOWN) {
        mm_dbg ("Not removing parts from non-stored SMS");
        g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
        sms_delete_parts_context_complete_and_free (ctx);
        return;
    }

    /* Select specific storage to delete from */
    mm_broadband_modem_lock_sms_storages (
        MM_BROADBAND_MODEM (self->priv->modem),
        mm_sms_get_storage (self),
        MM_SMS_STORAGE_UNKNOWN, /* none required for mem2 */
        (GAsyncReadyCallback)delete_lock_sms_storages_ready,
        ctx);
}

/*****************************************************************************/

gboolean
mm_sms_delete_finish (MMSms *self,
                      GAsyncResult *res,
                      GError **error)
{
    if (MM_SMS_GET_CLASS (self)->delete_finish) {
        gboolean deleted;

        deleted = MM_SMS_GET_CLASS (self)->delete_finish (self, res, error);
        if (deleted)
            /* We do change the state of this SMS back to UNKNOWN, as it is no
             * longer stored in the device */
            mm_gdbus_sms_set_state (MM_GDBUS_SMS (self), MM_SMS_STATE_UNKNOWN);

        return deleted;
    }

    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

void
mm_sms_delete (MMSms *self,
               GAsyncReadyCallback callback,
               gpointer user_data)
{
    if (MM_SMS_GET_CLASS (self)->delete &&
        MM_SMS_GET_CLASS (self)->delete_finish) {
        MM_SMS_GET_CLASS (self)->delete (self, callback, user_data);
        return;
    }

    g_simple_async_report_error_in_idle (G_OBJECT (self),
                                         callback,
                                         user_data,
                                         MM_CORE_ERROR,
                                         MM_CORE_ERROR_UNSUPPORTED,
                                         "Deleting SMS is not supported by this modem");
}

/*****************************************************************************/

static gboolean
assemble_sms (MMSms *self,
              GError **error)
{
    GList *l;
    guint idx;
    MMSmsPart **sorted_parts;
    GString *fulltext;
    GByteArray *fulldata;
    guint validity_relative;

    sorted_parts = g_new0 (MMSmsPart *, self->priv->max_parts);

    /* Note that sequence in multipart messages start with '1', while singlepart
     * messages have '0' as sequence. */

    if (self->priv->max_parts == 1) {
        if (g_list_length (self->priv->parts) != 1) {
            g_set_error (error,
                         MM_CORE_ERROR,
                         MM_CORE_ERROR_FAILED,
                         "Single part message with multiple parts (%u) found",
                         g_list_length (self->priv->parts));
            g_free (sorted_parts);
            return FALSE;
        }

        sorted_parts[0] = (MMSmsPart *)self->priv->parts->data;
    } else {
        /* Check if we have duplicate parts */
        for (l = self->priv->parts; l; l = g_list_next (l)) {
            idx = mm_sms_part_get_concat_sequence ((MMSmsPart *)l->data);

            if (idx < 1 || idx > self->priv->max_parts) {
                mm_warn ("Invalid part index (%u) found, ignoring", idx);
                continue;
            }

            if (sorted_parts[idx - 1]) {
                mm_warn ("Duplicate part index (%u) found, ignoring", idx);
                continue;
            }

            /* Add the part to the proper index */
            sorted_parts[idx - 1] = (MMSmsPart *)l->data;
        }
    }

    fulltext = g_string_new ("");
    fulldata = g_byte_array_sized_new (160 * self->priv->max_parts);

    /* Assemble text and data from all parts. Now 'idx' is the index of the
     * array, so for multipart messages the real index of the part is 'idx + 1'
     */
    for (idx = 0; idx < self->priv->max_parts; idx++) {
        const gchar *parttext;
        const GByteArray *partdata;

        if (!sorted_parts[idx]) {
            g_set_error (error,
                         MM_CORE_ERROR,
                         MM_CORE_ERROR_FAILED,
                         "Cannot assemble SMS, missing part at index (%u)",
                         self->priv->max_parts == 1 ? idx : idx + 1);
            g_string_free (fulltext, TRUE);
            g_byte_array_free (fulldata, TRUE);
            g_free (sorted_parts);
            return FALSE;
        }

        /* When the user creates the SMS, it will have either 'text' or 'data',
         * not both. Also status report PDUs may not have neither text nor data. */
        parttext = mm_sms_part_get_text (sorted_parts[idx]);
        partdata = mm_sms_part_get_data (sorted_parts[idx]);

        if (!parttext && !partdata &&
            mm_sms_part_get_pdu_type (sorted_parts[idx]) != MM_SMS_PDU_TYPE_STATUS_REPORT) {
            g_set_error (error,
                         MM_CORE_ERROR,
                         MM_CORE_ERROR_FAILED,
                         "Cannot assemble SMS, part at index (%u) has neither text nor data",
                         self->priv->max_parts == 1 ? idx : idx + 1);
            g_string_free (fulltext, TRUE);
            g_byte_array_free (fulldata, TRUE);
            g_free (sorted_parts);
            return FALSE;
        }

        if (parttext)
            g_string_append (fulltext, parttext);
        if (partdata)
            g_byte_array_append (fulldata, partdata->data, partdata->len);
    }

    /* If we got all parts, we also have the first one always */
    g_assert (sorted_parts[0] != NULL);

    /* Prepare for validity tuple */
    validity_relative = mm_sms_part_get_validity_relative (sorted_parts[0]);

    /* If we got everything, assemble the text! */
    g_object_set (self,
                  "pdu-type",  mm_sms_part_get_pdu_type (sorted_parts[0]),
                  "text",      fulltext->str,
                  "data",      g_variant_new_from_data (G_VARIANT_TYPE ("ay"),
                                                        fulldata->data,
                                                        fulldata->len * sizeof (guint8),
                                                        TRUE,
                                                        (GDestroyNotify) g_byte_array_unref,
                                                        g_byte_array_ref (fulldata)),
                  "smsc",      mm_sms_part_get_smsc (sorted_parts[0]),
                  "class",     mm_sms_part_get_class (sorted_parts[0]),
                  "number",    mm_sms_part_get_number (sorted_parts[0]),
                  "validity",                (validity_relative ?
                                              g_variant_new ("(uv)", MM_SMS_VALIDITY_TYPE_RELATIVE, g_variant_new_uint32 (validity_relative)) :
                                              g_variant_new ("(uv)", MM_SMS_VALIDITY_TYPE_UNKNOWN, g_variant_new_boolean (FALSE))),
                  "timestamp",               mm_sms_part_get_timestamp (sorted_parts[0]),
                  "discharge-timestamp",     mm_sms_part_get_discharge_timestamp (sorted_parts[0]),
                  "delivery-state",          mm_sms_part_get_delivery_state (sorted_parts[0]),
                  /* delivery report request and message reference taken always from the last part */
                  "message-reference",       mm_sms_part_get_message_reference (sorted_parts[self->priv->max_parts - 1]),
                  "delivery-report-request", mm_sms_part_get_delivery_report_request (sorted_parts[self->priv->max_parts - 1]),
                  NULL);

    g_string_free (fulltext, TRUE);
    g_byte_array_unref (fulldata);
    g_free (sorted_parts);

    self->priv->is_assembled = TRUE;

    return TRUE;
}

/*****************************************************************************/

static guint
cmp_sms_part_sequence (MMSmsPart *a,
                       MMSmsPart *b)
{
    return (mm_sms_part_get_concat_sequence (a) - mm_sms_part_get_concat_sequence (b));
}

gboolean
mm_sms_multipart_take_part (MMSms *self,
                            MMSmsPart *part,
                            GError **error)
{
    if (!self->priv->is_multipart) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "This SMS is not a multipart message");
        return FALSE;
    }

    if (g_list_length (self->priv->parts) >= self->priv->max_parts) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Already took %u parts, cannot take more",
                     g_list_length (self->priv->parts));
        return FALSE;
    }

    if (g_list_find_custom (self->priv->parts,
                            part,
                            (GCompareFunc)cmp_sms_part_sequence)) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Cannot take part, sequence %u already taken",
                     mm_sms_part_get_concat_sequence (part));
        return FALSE;
    }

    if (mm_sms_part_get_concat_sequence (part) > self->priv->max_parts) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Cannot take part with sequence %u, maximum is %u",
                     mm_sms_part_get_concat_sequence (part),
                     self->priv->max_parts);
        return FALSE;
    }

    /* Insert sorted by concat sequence */
    self->priv->parts = g_list_insert_sorted (self->priv->parts,
                                              part,
                                              (GCompareFunc)cmp_sms_part_sequence);

    /* We only populate contents when the multipart SMS is complete */
    if (mm_sms_multipart_is_complete (self)) {
        GError *inner_error = NULL;

        if (!assemble_sms (self, &inner_error)) {
            /* We DO NOT propagate the error. The part was properly taken
             * so ownership passed to the MMSms object. */
            mm_warn ("Couldn't assemble SMS: '%s'",
                     inner_error->message);
            g_error_free (inner_error);
        } else {
            /* Completed AND assembled
             * Change state RECEIVING->RECEIVED, and signal completeness */
            if (mm_gdbus_sms_get_state (MM_GDBUS_SMS (self)) == MM_SMS_STATE_RECEIVING)
                mm_gdbus_sms_set_state (MM_GDBUS_SMS (self), MM_SMS_STATE_RECEIVED);
        }
    }

    return TRUE;
}

MMSms *
mm_sms_new (MMBaseModem *modem)
{
    return MM_SMS (g_object_new (MM_TYPE_SMS,
                                 MM_SMS_MODEM, modem,
                                 NULL));
}

MMSms *
mm_sms_singlepart_new (MMBaseModem *modem,
                       MMSmsState state,
                       MMSmsStorage storage,
                       MMSmsPart *part,
                       GError **error)
{
    MMSms *self;

    g_assert (MM_IS_IFACE_MODEM_MESSAGING (modem));

    /* Create an SMS object as defined by the interface */
    self = mm_iface_modem_messaging_create_sms (MM_IFACE_MODEM_MESSAGING (modem));
    g_object_set (self,
                  "state",   state,
                  "storage", storage,
                  NULL);

    /* Keep the single part in the list */
    self->priv->parts = g_list_prepend (self->priv->parts, part);

    if (!assemble_sms (self, error)) {
        /* Note: we need to remove the part from the list, as we really didn't
         * take it, and therefore the caller is responsible for freeing it. */
        self->priv->parts = g_list_remove (self->priv->parts, part);
        g_clear_object (&self);
    } else
        /* Only export once properly created */
        mm_sms_export (self);

    return self;
}

MMSms *
mm_sms_multipart_new (MMBaseModem *modem,
                      MMSmsState state,
                      MMSmsStorage storage,
                      guint reference,
                      guint max_parts,
                      MMSmsPart *first_part,
                      GError **error)
{
    MMSms *self;

    g_assert (MM_IS_IFACE_MODEM_MESSAGING (modem));

    /* If this is the first part of a RECEIVED SMS, we overwrite the state
     * as RECEIVING, to indicate that it is not completed yet. */
    if (state == MM_SMS_STATE_RECEIVED)
        state = MM_SMS_STATE_RECEIVING;

    /* Create an SMS object as defined by the interface */
    self = mm_iface_modem_messaging_create_sms (MM_IFACE_MODEM_MESSAGING (modem));
    g_object_set (self,
                  MM_SMS_IS_MULTIPART,        TRUE,
                  MM_SMS_MAX_PARTS,           max_parts,
                  MM_SMS_MULTIPART_REFERENCE, reference,
                  "state",                    state,
                  "storage",                  storage,
                  NULL);

    if (!mm_sms_multipart_take_part (self, first_part, error))
        g_clear_object (&self);

    /* We do export uncomplete multipart messages, in order to be able to
     *  request removal of all parts of those multipart SMS that will never
     *  get completed.
     * Only the STATE of the SMS object will be valid in the exported DBus
     *  interface.*/
    if (self)
        mm_sms_export (self);

    return self;
}

MMSms *
mm_sms_new_from_properties (MMBaseModem *modem,
                            MMSmsProperties *properties,
                            GError **error)
{
    MMSms *self;
    const gchar *text;
    GByteArray *data;

    g_assert (MM_IS_IFACE_MODEM_MESSAGING (modem));

    text = mm_sms_properties_get_text (properties);
    data = mm_sms_properties_peek_data_bytearray (properties);

    /* Don't create SMS from properties if either (text|data) or number is missing */
    if (!mm_sms_properties_get_number (properties) ||
        (!text && !data)) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_INVALID_ARGS,
                     "Cannot create SMS: mandatory parameter '%s' is missing",
                     (!mm_sms_properties_get_number (properties)?
                      "number" : "text' or 'data"));
        return NULL;
    }

    /* Don't create SMS from properties if both text and data are given */
    if (text && data) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_INVALID_ARGS,
                     "Cannot create SMS: both 'text' and 'data' given");
        return NULL;
    }

    /* Create an SMS object as defined by the interface */
    self = mm_iface_modem_messaging_create_sms (MM_IFACE_MODEM_MESSAGING (modem));
    g_object_set (self,
                  "state",    MM_SMS_STATE_UNKNOWN,
                  "storage",  MM_SMS_STORAGE_UNKNOWN,
                  "number",   mm_sms_properties_get_number (properties),
                  "pdu-type", MM_SMS_PDU_TYPE_SUBMIT,
                  "text",     text,
                  "data",     (data ?
                               g_variant_new_from_data (G_VARIANT_TYPE ("ay"),
                                                        data->data,
                                                        data->len * sizeof (guint8),
                                                        TRUE,
                                                        (GDestroyNotify) g_byte_array_unref,
                                                        g_byte_array_ref (data)) :
                               NULL),
                  "smsc",     mm_sms_properties_get_smsc (properties),
                  "class",    mm_sms_properties_get_class (properties),
                  "delivery-report-request", mm_sms_properties_get_delivery_report_request (properties),
                  NULL);

    /* Only export once properly created */
    mm_sms_export (self);

    return self;
}

/*****************************************************************************/

static void
set_property (GObject *object,
              guint prop_id,
              const GValue *value,
              GParamSpec *pspec)
{
    MMSms *self = MM_SMS (object);

    switch (prop_id) {
    case PROP_PATH:
        g_free (self->priv->path);
        self->priv->path = g_value_dup_string (value);

        /* Export when we get a DBus connection AND we have a path */
        if (!self->priv->path)
            mm_sms_dbus_unexport (self);
        else if (self->priv->connection)
            mm_sms_dbus_export (self);
        break;
    case PROP_CONNECTION:
        g_clear_object (&self->priv->connection);
        self->priv->connection = g_value_dup_object (value);

        /* Export when we get a DBus connection AND we have a path */
        if (!self->priv->connection)
            mm_sms_dbus_unexport (self);
        else if (self->priv->path)
            mm_sms_dbus_export (self);
        break;
    case PROP_MODEM:
        g_clear_object (&self->priv->modem);
        self->priv->modem = g_value_dup_object (value);
        if (self->priv->modem) {
            /* Bind the modem's connection (which is set when it is exported,
             * and unset when unexported) to the SMS's connection */
            g_object_bind_property (self->priv->modem, MM_BASE_MODEM_CONNECTION,
                                    self, MM_SMS_CONNECTION,
                                    G_BINDING_DEFAULT | G_BINDING_SYNC_CREATE);
        }
        break;
    case PROP_IS_MULTIPART:
        self->priv->is_multipart = g_value_get_boolean (value);
        break;
    case PROP_MAX_PARTS:
        self->priv->max_parts = g_value_get_uint (value);
        break;
    case PROP_MULTIPART_REFERENCE:
        self->priv->multipart_reference = g_value_get_uint (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
get_property (GObject *object,
              guint prop_id,
              GValue *value,
              GParamSpec *pspec)
{
    MMSms *self = MM_SMS (object);

    switch (prop_id) {
    case PROP_PATH:
        g_value_set_string (value, self->priv->path);
        break;
    case PROP_CONNECTION:
        g_value_set_object (value, self->priv->connection);
        break;
    case PROP_MODEM:
        g_value_set_object (value, self->priv->modem);
        break;
    case PROP_IS_MULTIPART:
        g_value_set_boolean (value, self->priv->is_multipart);
        break;
    case PROP_MAX_PARTS:
        g_value_set_uint (value, self->priv->max_parts);
        break;
    case PROP_MULTIPART_REFERENCE:
        g_value_set_uint (value, self->priv->multipart_reference);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
mm_sms_init (MMSms *self)
{
    /* Initialize private data */
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE ((self),
                                              MM_TYPE_SMS,
                                              MMSmsPrivate);
    /* Defaults */
    self->priv->max_parts = 1;
}

static void
finalize (GObject *object)
{
    MMSms *self = MM_SMS (object);

    g_list_free_full (self->priv->parts, (GDestroyNotify)mm_sms_part_free);
    g_free (self->priv->path);

    G_OBJECT_CLASS (mm_sms_parent_class)->finalize (object);
}

static void
dispose (GObject *object)
{
    MMSms *self = MM_SMS (object);

    if (self->priv->connection) {
        /* If we arrived here with a valid connection, make sure we unexport
         * the object */
        mm_sms_dbus_unexport (self);
        g_clear_object (&self->priv->connection);
    }

    g_clear_object (&self->priv->modem);

    G_OBJECT_CLASS (mm_sms_parent_class)->dispose (object);
}

static void
mm_sms_class_init (MMSmsClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMSmsPrivate));

    /* Virtual methods */
    object_class->get_property = get_property;
    object_class->set_property = set_property;
    object_class->finalize = finalize;
    object_class->dispose = dispose;

    klass->store = sms_store;
    klass->store_finish = sms_store_finish;
    klass->send = sms_send;
    klass->send_finish = sms_send_finish;
    klass->delete = sms_delete;
    klass->delete_finish = sms_delete_finish;

    properties[PROP_CONNECTION] =
        g_param_spec_object (MM_SMS_CONNECTION,
                             "Connection",
                             "GDBus connection to the system bus.",
                             G_TYPE_DBUS_CONNECTION,
                             G_PARAM_READWRITE);
    g_object_class_install_property (object_class, PROP_CONNECTION, properties[PROP_CONNECTION]);

    properties[PROP_PATH] =
        g_param_spec_string (MM_SMS_PATH,
                             "Path",
                             "DBus path of the SMS",
                             NULL,
                             G_PARAM_READWRITE);
    g_object_class_install_property (object_class, PROP_PATH, properties[PROP_PATH]);

    properties[PROP_MODEM] =
        g_param_spec_object (MM_SMS_MODEM,
                             "Modem",
                             "The Modem which owns this SMS",
                             MM_TYPE_BASE_MODEM,
                             G_PARAM_READWRITE);
    g_object_class_install_property (object_class, PROP_MODEM, properties[PROP_MODEM]);

    properties[PROP_IS_MULTIPART] =
        g_param_spec_boolean (MM_SMS_IS_MULTIPART,
                              "Is multipart",
                              "Flag specifying if the SMS is multipart",
                              FALSE,
                              G_PARAM_READWRITE);
    g_object_class_install_property (object_class, PROP_IS_MULTIPART, properties[PROP_IS_MULTIPART]);

    properties[PROP_MAX_PARTS] =
        g_param_spec_uint (MM_SMS_MAX_PARTS,
                           "Max parts",
                           "Maximum number of parts composing this SMS",
                           1,255, 1,
                           G_PARAM_READWRITE);
    g_object_class_install_property (object_class, PROP_MAX_PARTS, properties[PROP_MAX_PARTS]);

    properties[PROP_MULTIPART_REFERENCE] =
        g_param_spec_uint (MM_SMS_MULTIPART_REFERENCE,
                           "Multipart reference",
                           "Common reference for all parts in the multipart SMS",
                           0, G_MAXUINT, 0,
                           G_PARAM_READWRITE);
    g_object_class_install_property (object_class, PROP_MULTIPART_REFERENCE, properties[PROP_MULTIPART_REFERENCE]);
}
