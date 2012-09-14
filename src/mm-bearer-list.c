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
 * Copyright (C) 2009 - 2011 Red Hat, Inc.
 * Copyright (C) 2011 Google, Inc.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#include <ModemManager.h>
#include <libmm-common.h>

#include "mm-bearer-list.h"
#include "mm-log.h"

G_DEFINE_TYPE (MMBearerList, mm_bearer_list, G_TYPE_OBJECT);

enum {
    PROP_0,
    PROP_MAX_BEARERS,
    PROP_MAX_ACTIVE_BEARERS,
    PROP_LAST
};

static GParamSpec *properties[PROP_LAST];

struct _MMBearerListPrivate {
    /* List of bearers */
    GList *bearers;
    /* Max number of bearers */
    guint max_bearers;
    /* Max number of active bearers */
    guint max_active_bearers;
};

/*****************************************************************************/

guint
mm_bearer_list_get_max (MMBearerList *self)
{
    return self->priv->max_bearers;
}

guint
mm_bearer_list_get_max_active (MMBearerList *self)
{
    return self->priv->max_active_bearers;
}

guint
mm_bearer_list_get_count (MMBearerList *self)
{
    return g_list_length (self->priv->bearers);
}

guint
mm_bearer_list_get_count_active (MMBearerList *self)
{
    return 0; /* TODO */
}

gboolean
mm_bearer_list_add_bearer (MMBearerList *self,
                           MMBearer *bearer,
                           GError **error)
{
    /* Just in case, ensure we don't go off limits */
    if (g_list_length (self->priv->bearers) == self->priv->max_bearers) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_TOO_MANY,
                     "Cannot add new bearer: already reached maximum (%u)",
                     self->priv->max_bearers);
        return FALSE;
    }

    /* Keep our own reference */
    self->priv->bearers = g_list_prepend (self->priv->bearers,
                                          g_object_ref (bearer));
    return TRUE;
}

gboolean
mm_bearer_list_delete_bearer (MMBearerList *self,
                              const gchar *path,
                              GError **error)
{
    GList *l;

    if (!g_str_has_prefix (path, MM_DBUS_BEARER_PREFIX)) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_INVALID_ARGS,
                     "Cannot delete bearer: invalid path '%s'",
                     path);
        return FALSE;
    }

    for (l = self->priv->bearers; l; l = g_list_next (l)) {
        if (g_str_equal (path, mm_bearer_get_path (MM_BEARER (l->data)))) {
            g_object_unref (l->data);
            self->priv->bearers =
                g_list_delete_link (self->priv->bearers, l);
            return TRUE;
        }
    }

    g_set_error (error,
                 MM_CORE_ERROR,
                 MM_CORE_ERROR_NOT_FOUND,
                 "Cannot delete bearer: path '%s' not found",
                 path);
    return FALSE;
}

void
mm_bearer_list_delete_all_bearers (MMBearerList *self)
{
    if (!self->priv->bearers)
        return;

    g_list_free_full (self->priv->bearers, (GDestroyNotify) g_object_unref);
    self->priv->bearers = NULL;
}

GStrv
mm_bearer_list_get_paths (MMBearerList *self)
{
    GStrv path_list = NULL;
    GList *l;
    guint i;

    path_list = g_new0 (gchar *,
                        1 + g_list_length (self->priv->bearers));

    for (i = 0, l = self->priv->bearers; l; l = g_list_next (l))
        path_list[i++] = g_strdup (mm_bearer_get_path (MM_BEARER (l->data)));

    return path_list;
}

void
mm_bearer_list_foreach (MMBearerList *self,
                        MMBearerListForeachFunc func,
                        gpointer user_data)
{
    g_list_foreach (self->priv->bearers, (GFunc)func, user_data);
}

MMBearer *
mm_bearer_list_find (MMBearerList *self,
                     MMBearerProperties *properties)
{
    GList *l;

    for (l = self->priv->bearers; l; l = g_list_next (l)) {
        if (mm_bearer_properties_cmp (mm_bearer_peek_config (MM_BEARER (l->data)), properties))
            return g_object_ref (l->data);
    }

    return NULL;
}

/*****************************************************************************/

typedef struct {
    GSimpleAsyncResult *result;
    GList *pending;
    MMBearer *current;
} DisconnectAllContext;

static void
disconnect_all_context_complete_and_free (DisconnectAllContext *ctx)
{
    g_simple_async_result_complete (ctx->result);
    g_object_unref (ctx->result);
    if (ctx->current)
        g_object_unref (ctx->current);
    g_list_free_full (ctx->pending, (GDestroyNotify) g_object_unref);
    g_free (ctx);
}

gboolean
mm_bearer_list_disconnect_all_bearers_finish (MMBearerList *self,
                                              GAsyncResult *res,
                                              GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void disconnect_next_bearer (DisconnectAllContext *ctx);

static void
disconnect_ready (MMBearer *bearer,
                  GAsyncResult *res,
                  DisconnectAllContext *ctx)
{
    GError *error = NULL;

    if (!mm_bearer_disconnect_finish (bearer, res, &error)) {
        g_simple_async_result_take_error (ctx->result, error);
        disconnect_all_context_complete_and_free (ctx);
        return;
    }

    disconnect_next_bearer (ctx);
}

static void
disconnect_next_bearer (DisconnectAllContext *ctx)
{
    if (ctx->current)
        g_clear_object (&ctx->current);

    /* No more bearers? all done! */
    if (!ctx->pending) {
        g_simple_async_result_set_op_res_gboolean (ctx->result, TRUE);
        disconnect_all_context_complete_and_free (ctx);
        return;
    }

    ctx->current = MM_BEARER (ctx->pending->data);
    ctx->pending = g_list_delete_link (ctx->pending, ctx->pending);

    mm_bearer_disconnect (ctx->current,
                          (GAsyncReadyCallback)disconnect_ready,
                          ctx);
}

void
mm_bearer_list_disconnect_all_bearers (MMBearerList *self,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
    DisconnectAllContext *ctx;

    ctx = g_new0 (DisconnectAllContext, 1);
    ctx->result = g_simple_async_result_new (G_OBJECT (self),
                                             callback,
                                             user_data,
                                             mm_bearer_list_disconnect_all_bearers);
    /* Get a copy of the list */
    ctx->pending = g_list_copy (self->priv->bearers);
    g_list_foreach (ctx->pending, (GFunc) g_object_ref, NULL);

    disconnect_next_bearer (ctx);
}

/*****************************************************************************/

MMBearerList *
mm_bearer_list_new (guint max_bearers,
                    guint max_active_bearers)
{
    mm_dbg ("Creating bearer list (max: %u, max active: %u)",
            max_bearers,
            max_active_bearers);

    /* Create the object */
    return g_object_new  (MM_TYPE_BEARER_LIST,
                          MM_BEARER_LIST_MAX_BEARERS, max_bearers,
                          MM_BEARER_LIST_MAX_ACTIVE_BEARERS, max_active_bearers,
                          NULL);
}

static void
set_property (GObject *object,
              guint prop_id,
              const GValue *value,
              GParamSpec *pspec)
{
    MMBearerList *self = MM_BEARER_LIST (object);

    switch (prop_id) {
    case PROP_MAX_BEARERS:
        self->priv->max_bearers = g_value_get_uint (value);
        break;
    case PROP_MAX_ACTIVE_BEARERS:
        self->priv->max_active_bearers = g_value_get_uint (value);
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
    MMBearerList *self = MM_BEARER_LIST (object);

    switch (prop_id) {
    case PROP_MAX_BEARERS:
        g_value_set_uint (value, self->priv->max_bearers);
        break;
    case PROP_MAX_ACTIVE_BEARERS:
        g_value_set_uint (value, self->priv->max_active_bearers);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
mm_bearer_list_init (MMBearerList *self)
{
    /* Initialize private data */
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE ((self),
                                              MM_TYPE_BEARER_LIST,
                                              MMBearerListPrivate);
}

static void
dispose (GObject *object)
{
    MMBearerList *self = MM_BEARER_LIST (object);

    mm_bearer_list_delete_all_bearers (self);

    G_OBJECT_CLASS (mm_bearer_list_parent_class)->dispose (object);
}

static void
mm_bearer_list_class_init (MMBearerListClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMBearerListPrivate));

    /* Virtual methods */
    object_class->get_property = get_property;
    object_class->set_property = set_property;
    object_class->dispose = dispose;

    properties[PROP_MAX_BEARERS] =
        g_param_spec_uint (MM_BEARER_LIST_MAX_BEARERS,
                           "Max bearers",
                           "Maximum number of bearers the list can handle",
                           1,
                           G_MAXUINT,
                           1,
                           G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);
    g_object_class_install_property (object_class, PROP_MAX_BEARERS, properties[PROP_MAX_BEARERS]);

    properties[PROP_MAX_ACTIVE_BEARERS] =
        g_param_spec_uint (MM_BEARER_LIST_MAX_ACTIVE_BEARERS,
                           "Max active bearers",
                           "Maximum number of active bearers the list can handle",
                           1,
                           G_MAXUINT,
                           1,
                           G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);
    g_object_class_install_property (object_class, PROP_MAX_ACTIVE_BEARERS, properties[PROP_MAX_ACTIVE_BEARERS]);

}
