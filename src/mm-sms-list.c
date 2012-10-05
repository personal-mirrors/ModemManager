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

#include "mm-iface-modem-messaging.h"
#include "mm-marshal.h"
#include "mm-sms-list.h"
#include "mm-sms.h"
#include "mm-log.h"

G_DEFINE_TYPE (MMSmsList, mm_sms_list, G_TYPE_OBJECT);

enum {
    PROP_0,
    PROP_MODEM,
    PROP_LAST
};
static GParamSpec *properties[PROP_LAST];

enum {
    SIGNAL_ADDED,
    SIGNAL_DELETED,
    SIGNAL_LAST
};
static guint signals[SIGNAL_LAST];

struct _MMSmsListPrivate {
    /* The owner modem */
    MMBaseModem *modem;
    /* List of sms objects */
    GList *list;
};

/*****************************************************************************/

gboolean
mm_sms_list_has_local_multipart_reference (MMSmsList *self,
                                           const gchar *number,
                                           guint8 reference)
{
    GList *l;

    /* No one should look for multipart reference 0, which isn't valid */
    g_assert (reference != 0);

    for (l = self->priv->list; l; l = g_list_next (l)) {
        MMSms *sms = MM_SMS (l->data);

        if (mm_sms_is_multipart (sms) &&
            mm_gdbus_sms_get_pdu_type (MM_GDBUS_SMS (sms)) == MM_SMS_PDU_TYPE_SUBMIT &&
            mm_sms_get_storage (sms) != MM_SMS_STORAGE_UNKNOWN &&
            mm_sms_get_multipart_reference (sms) == reference &&
            g_str_equal (mm_gdbus_sms_get_number (MM_GDBUS_SMS (sms)), number)) {
            /* Yes, the SMS list has an SMS with the same destination number
             * and multipart reference */
            return TRUE;
        }
    }

    return FALSE;
}

/*****************************************************************************/

guint
mm_sms_list_get_count (MMSmsList *self)
{
    return g_list_length (self->priv->list);
}

GStrv
mm_sms_list_get_paths (MMSmsList *self)
{
    GStrv path_list = NULL;
    GList *l;
    guint i;

    path_list = g_new0 (gchar *,
                        1 + g_list_length (self->priv->list));

    for (i = 0, l = self->priv->list; l; l = g_list_next (l)) {
        const gchar *path;

        /* Don't try to add NULL paths (not yet exported SMS objects) */
        path = mm_sms_get_path (MM_SMS (l->data));
        if (path)
            path_list[i++] = g_strdup (path);
    }

    return path_list;
}

/*****************************************************************************/

typedef struct {
    MMSmsList *self;
    GSimpleAsyncResult *result;
    gchar *path;
} DeleteSmsContext;

static void
delete_sms_context_complete_and_free (DeleteSmsContext *ctx)
{
    g_simple_async_result_complete (ctx->result);
    g_object_unref (ctx->result);
    g_object_unref (ctx->self);
    g_free (ctx->path);
    g_free (ctx);
}

gboolean
mm_sms_list_delete_sms_finish (MMSmsList *self,
                               GAsyncResult *res,
                               GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static guint
cmp_sms_by_path (MMSms *sms,
                 const gchar *path)
{
    return g_strcmp0 (mm_sms_get_path (sms), path);
}

static void
delete_ready (MMSms *sms,
              GAsyncResult *res,
              DeleteSmsContext *ctx)
{
    GError *error = NULL;
    GList *l;

    if (!mm_sms_delete_finish (sms, res, &error)) {
        /* We report the error */
        g_simple_async_result_take_error (ctx->result, error);
        delete_sms_context_complete_and_free (ctx);
        return;
    }

    /* The SMS was properly deleted, we now remove it from our list */
    l = g_list_find_custom (ctx->self->priv->list,
                            ctx->path,
                            (GCompareFunc)cmp_sms_by_path);
    if (l) {
        g_object_unref (MM_SMS (l->data));
        ctx->self->priv->list = g_list_delete_link (ctx->self->priv->list, l);
    }

    /* We don't need to unref the SMS any more, but we can use the
     * reference we got in the method, which is the one kept alive
     * during the async operation. */
    mm_sms_unexport (sms);

    g_signal_emit (ctx->self,
                   signals[SIGNAL_DELETED], 0,
                   ctx->path);

    g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
    delete_sms_context_complete_and_free (ctx);
}

void
mm_sms_list_delete_sms (MMSmsList *self,
                        const gchar *sms_path,
                        GAsyncReadyCallback callback,
                        gpointer user_data)
{
    DeleteSmsContext *ctx;
    GList *l;

    l = g_list_find_custom (self->priv->list,
                            (gpointer)sms_path,
                            (GCompareFunc)cmp_sms_by_path);
    if (!l) {
        g_simple_async_report_error_in_idle (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             MM_CORE_ERROR,
                                             MM_CORE_ERROR_NOT_FOUND,
                                             "No SMS found with path '%s'",
                                             sms_path);
        return;
    }

    /* Delete all SMS parts */
    ctx = g_new0 (DeleteSmsContext, 1);
    ctx->self = g_object_ref (self);
    ctx->path = g_strdup (sms_path);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             mm_sms_list_delete_sms);

    mm_sms_delete (MM_SMS (l->data),
                   (GAsyncReadyCallback)delete_ready,
                   ctx);
}

/*****************************************************************************/

void
mm_sms_list_add_sms (MMSmsList *self,
                     MMSms *sms)
{
    self->priv->list = g_list_prepend (self->priv->list, g_object_ref (sms));
}

/*****************************************************************************/

static guint
cmp_sms_by_concat_reference (MMSms *sms,
                             gpointer user_data)
{
    if (!mm_sms_is_multipart (sms))
        return -1;

    return (GPOINTER_TO_UINT (user_data) - mm_sms_get_multipart_reference (sms));
}

typedef struct {
    guint part_index;
    MMSmsStorage storage;
} PartIndexAndStorage;

static guint
cmp_sms_by_part_index_and_storage (MMSms *sms,
                                   PartIndexAndStorage *ctx)
{
    return !(mm_sms_get_storage (sms) == ctx->storage &&
             mm_sms_has_part_index (sms, ctx->part_index));
}

static gboolean
take_singlepart (MMSmsList *self,
                 MMSmsPart *part,
                 MMSmsState state,
                 MMSmsStorage storage,
                 GError **error)
{
    MMSms *sms;

    sms = mm_sms_singlepart_new (self->priv->modem,
                                 state,
                                 storage,
                                 part,
                                 error);
    if (!sms)
        return FALSE;

    self->priv->list = g_list_prepend (self->priv->list, sms);
    g_signal_emit (self, signals[SIGNAL_ADDED], 0,
                   mm_sms_get_path (sms),
                   state == MM_SMS_STATE_RECEIVED);
    return TRUE;
}

static gboolean
take_multipart (MMSmsList *self,
                MMSmsPart *part,
                MMSmsState state,
                MMSmsStorage storage,
                GError **error)
{
    GList *l;
    MMSms *sms;
    guint concat_reference;

    concat_reference = mm_sms_part_get_concat_reference (part);
    l = g_list_find_custom (self->priv->list,
                            GUINT_TO_POINTER (concat_reference),
                            (GCompareFunc)cmp_sms_by_concat_reference);
    if (l)
        /* Try to take the part */
        return mm_sms_multipart_take_part (MM_SMS (l->data), part, error);

    /* Create new Multipart */
    sms = mm_sms_multipart_new (self->priv->modem,
                                state,
                                storage,
                                concat_reference,
                                mm_sms_part_get_concat_max (part),
                                part,
                                error);
    if (!sms)
        return FALSE;

    self->priv->list = g_list_prepend (self->priv->list, sms);
    g_signal_emit (self, signals[SIGNAL_ADDED], 0,
                   mm_sms_get_path (sms),
                   (state == MM_SMS_STATE_RECEIVED ||
                    state == MM_SMS_STATE_RECEIVING));

    return TRUE;
}

gboolean
mm_sms_list_has_part (MMSmsList *self,
                      MMSmsStorage storage,
                      guint index)
{
    PartIndexAndStorage ctx;

    if (storage == MM_SMS_STORAGE_UNKNOWN ||
        index == SMS_PART_INVALID_INDEX)
        return FALSE;

    ctx.part_index = index;
    ctx.storage = storage;

    return !!g_list_find_custom (self->priv->list,
                                 &ctx,
                                 (GCompareFunc)cmp_sms_by_part_index_and_storage);
}

gboolean
mm_sms_list_take_part (MMSmsList *self,
                       MMSmsPart *part,
                       MMSmsState state,
                       MMSmsStorage storage,
                       GError **error)
{
    PartIndexAndStorage ctx;

    ctx.part_index = mm_sms_part_get_index (part);
    ctx.storage = storage;

    /* Ensure we don't have already taken a part with the same index */
    if (mm_sms_list_has_part (self,
                              storage,
                              mm_sms_part_get_index (part))) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "A part with index %u was already taken",
                     mm_sms_part_get_index (part));
        return FALSE;
    }

    /* Did we just get a part of a multi-part SMS? */
    if (mm_sms_part_should_concat (part)) {
        if (mm_sms_part_get_index (part) != SMS_PART_INVALID_INDEX)
            mm_dbg ("SMS part at '%s/%u' is from a multipart SMS (reference: '%u', sequence: '%u')",
                    mm_sms_storage_get_string (storage),
                    mm_sms_part_get_index (part),
                    mm_sms_part_get_concat_reference (part),
                    mm_sms_part_get_concat_sequence (part));
        else
            mm_dbg ("SMS part (not stored) is from a multipart SMS (reference: '%u', sequence: '%u')",
                    mm_sms_part_get_concat_reference (part),
                    mm_sms_part_get_concat_sequence (part));

        return take_multipart (self, part, state, storage, error);
    }

    /* Otherwise, we build a whole new single-part MMSms just from this part */
    if (mm_sms_part_get_index (part) != SMS_PART_INVALID_INDEX)
        mm_dbg ("SMS part at '%s/%u' is from a singlepart SMS",
                mm_sms_storage_get_string (storage),
                mm_sms_part_get_index (part));
    else
        mm_dbg ("SMS part (not stored) is from a singlepart SMS");
    return take_singlepart (self, part, state, storage, error);
}

/*****************************************************************************/

MMSmsList *
mm_sms_list_new (MMBaseModem *modem)
{
    /* Create the object */
    return g_object_new  (MM_TYPE_SMS_LIST,
                          MM_SMS_LIST_MODEM, modem,
                          NULL);
}

static void
set_property (GObject *object,
              guint prop_id,
              const GValue *value,
              GParamSpec *pspec)
{
    MMSmsList *self = MM_SMS_LIST (object);

    switch (prop_id) {
    case PROP_MODEM:
        g_clear_object (&self->priv->modem);
        self->priv->modem = g_value_dup_object (value);
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
    MMSmsList *self = MM_SMS_LIST (object);

    switch (prop_id) {
    case PROP_MODEM:
        g_value_set_object (value, self->priv->modem);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
mm_sms_list_init (MMSmsList *self)
{
    /* Initialize private data */
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE ((self),
                                              MM_TYPE_SMS_LIST,
                                              MMSmsListPrivate);
}

static void
dispose (GObject *object)
{
    MMSmsList *self = MM_SMS_LIST (object);

    g_clear_object (&self->priv->modem);
    g_list_free_full (self->priv->list, (GDestroyNotify)g_object_unref);

    G_OBJECT_CLASS (mm_sms_list_parent_class)->dispose (object);
}

static void
mm_sms_list_class_init (MMSmsListClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMSmsListPrivate));

    /* Virtual methods */
    object_class->get_property = get_property;
    object_class->set_property = set_property;
    object_class->dispose = dispose;

    /* Properties */
    properties[PROP_MODEM] =
        g_param_spec_object (MM_SMS_LIST_MODEM,
                             "Modem",
                             "The Modem which owns this SMS list",
                             MM_TYPE_BASE_MODEM,
                             G_PARAM_READWRITE);
    g_object_class_install_property (object_class, PROP_MODEM, properties[PROP_MODEM]);

    /* Signals */
    signals[SIGNAL_ADDED] =
        g_signal_new (MM_SMS_ADDED,
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (MMSmsListClass, sms_added),
                      NULL, NULL,
                      mm_marshal_VOID__STRING_BOOLEAN,
                      G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);

    signals[SIGNAL_DELETED] =
        g_signal_new (MM_SMS_DELETED,
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (MMSmsListClass, sms_deleted),
                      NULL, NULL,
                      mm_marshal_VOID__STRING,
                      G_TYPE_NONE, 1, G_TYPE_STRING);
}
