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

#include <string.h>
#include <errno.h>
#include <stdlib.h>

#include "mm-enums-types.h"
#include "mm-unlock-retries.h"

G_DEFINE_TYPE (MMUnlockRetries, mm_unlock_retries, G_TYPE_OBJECT);

struct _MMUnlockRetriesPrivate {
    GHashTable *ht;
};

/*****************************************************************************/

void
mm_unlock_retries_set (MMUnlockRetries *self,
                       MMModemLock lock,
                       guint retries)
{
    g_hash_table_replace (self->priv->ht,
                          GUINT_TO_POINTER (lock),
                          GUINT_TO_POINTER (retries));
}

void
mm_unlock_retries_unset (MMUnlockRetries *self,
                         MMModemLock lock)
{
    g_hash_table_remove (self->priv->ht,
                         GUINT_TO_POINTER (lock));
}

guint
mm_unlock_retries_get (MMUnlockRetries *self,
                       MMModemLock lock)
{
    gpointer value = NULL;

    return (g_hash_table_lookup_extended (self->priv->ht,
                                          GUINT_TO_POINTER (lock),
                                          NULL, /* original key not needed */
                                          &value) ?
            GPOINTER_TO_UINT (value) :
            MM_UNLOCK_RETRIES_UNKNOWN);
}

/*****************************************************************************/

gboolean
mm_unlock_retries_cmp (MMUnlockRetries *a,
                       MMUnlockRetries *b)
{
    GHashTableIter iter;
    gpointer key, value;

    if (g_hash_table_size (a->priv->ht) != g_hash_table_size (b->priv->ht))
        return FALSE;

    g_hash_table_iter_init (&iter, a->priv->ht);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
        g_assert (GPOINTER_TO_UINT (value) != MM_UNLOCK_RETRIES_UNKNOWN);

        if (GPOINTER_TO_UINT (value) != mm_unlock_retries_get (b, GPOINTER_TO_UINT (key)))
            return FALSE;
    }

    /* All equal! */
    return TRUE;
}

/*****************************************************************************/

void
mm_unlock_retries_foreach (MMUnlockRetries *self,
                           MMUnlockRetriesForeachCb callback,
                           gpointer user_data)
{
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init (&iter, self->priv->ht);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
        callback (GPOINTER_TO_UINT (key),
                  GPOINTER_TO_UINT (value),
                  user_data);
    }
}

/*****************************************************************************/

GVariant *
mm_unlock_retries_get_dictionary (MMUnlockRetries *self)
{
    GVariantBuilder builder;
    GHashTableIter iter;
    gpointer key, value;

    /* We do allow NULL */
    if (!self)
        return NULL;

    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{uu}"));

    g_hash_table_iter_init (&iter, self->priv->ht);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
        g_variant_builder_add (&builder,
                               "{uu}",
                               GPOINTER_TO_UINT (key),
                               GPOINTER_TO_UINT (value));
    }

    return g_variant_ref_sink (g_variant_builder_end (&builder));
}

/*****************************************************************************/

MMUnlockRetries *
mm_unlock_retries_new_from_dictionary (GVariant *dictionary)
{
    GVariantIter iter;
    guint key, value;
    MMUnlockRetries *self;

    self = mm_unlock_retries_new ();
    if (!dictionary)
        return self;

    g_variant_iter_init (&iter, dictionary);
    while (g_variant_iter_next (&iter, "{uu}", &key, &value)) {
        mm_unlock_retries_set (self,
                               (MMModemLock)key,
                               value);
    }

    return self;
}

/*****************************************************************************/

gchar *
mm_unlock_retries_build_string (MMUnlockRetries *self)
{
    GString *str = NULL;
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init (&iter, self->priv->ht);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
        const gchar *lock_name;
        guint retries;

        lock_name = mm_modem_lock_get_string ((MMModemLock)GPOINTER_TO_UINT (key));
        retries = GPOINTER_TO_UINT (value);

        if (!str) {
            str = g_string_new ("");
            g_string_append_printf (str, "%s (%u)", lock_name, retries);
        } else
            g_string_append_printf (str, ", %s (%u)", lock_name, retries);
    }

    return (str ? g_string_free (str, FALSE) : NULL);
}

/*****************************************************************************/

MMUnlockRetries *
mm_unlock_retries_new (void)
{
    return (MM_UNLOCK_RETRIES (
                g_object_new (MM_TYPE_UNLOCK_RETRIES, NULL)));
}

static void
mm_unlock_retries_init (MMUnlockRetries *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE ((self),
                                              MM_TYPE_UNLOCK_RETRIES,
                                              MMUnlockRetriesPrivate);
    self->priv->ht = g_hash_table_new (g_direct_hash,
                                       g_direct_equal);
}

static void
finalize (GObject *object)
{
    MMUnlockRetries *self = MM_UNLOCK_RETRIES (object);

    g_hash_table_destroy (self->priv->ht);

    G_OBJECT_CLASS (mm_unlock_retries_parent_class)->finalize (object);
}

static void
mm_unlock_retries_class_init (MMUnlockRetriesClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMUnlockRetriesPrivate));

    object_class->finalize = finalize;
}
