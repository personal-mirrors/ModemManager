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
 * Singleton support imported from NetworkManager.
 *     (C) Copyright 2014 Red Hat, Inc.
 */

#ifndef MM_UTILS_H
#define MM_UTILS_H

#include <glib.h>
#include <glib-object.h>

#include "mm-log.h"

/*****************************************************************************/

#define MM_DEFINE_SINGLETON_INSTANCE(TYPE)      \
    static TYPE *singleton_instance

#define MM_DEFINE_SINGLETON_WEAK_REF(TYPE)                              \
    MM_DEFINE_SINGLETON_INSTANCE (TYPE);                                \
    static void                                                         \
    _singleton_instance_weak_ref_cb (gpointer data,                     \
                                     GObject *where_the_object_was)     \
    {                                                                   \
        mm_dbg ("disposing %s singleton (%p)", G_STRINGIFY (TYPE), singleton_instance); \
        singleton_instance = NULL;                                      \
    }                                                                   \
    static inline void                                                  \
    mm_singleton_instance_weak_ref_register (void)                      \
    {                                                                   \
        g_object_weak_ref (G_OBJECT (singleton_instance), _singleton_instance_weak_ref_cb, NULL); \
    }

#define MM_DEFINE_SINGLETON_DESTRUCTOR(TYPE)                            \
    MM_DEFINE_SINGLETON_INSTANCE (TYPE);                                \
    static void __attribute__((destructor))                             \
    _singleton_destructor (void)                                        \
    {                                                                   \
        if (singleton_instance) {                                       \
            if (G_OBJECT (singleton_instance)->ref_count > 1)           \
                mm_dbg ("disown %s singleton (%p)", G_STRINGIFY (TYPE), singleton_instance); \
            g_object_unref (singleton_instance);                        \
        }                                                               \
    }

/* By default, the getter will assert that the singleton will be created only once. You can
 * change this by redefining MM_DEFINE_SINGLETON_ALLOW_MULTIPLE. */
#ifndef MM_DEFINE_SINGLETON_ALLOW_MULTIPLE
#define MM_DEFINE_SINGLETON_ALLOW_MULTIPLE     FALSE
#endif

#define MM_DEFINE_SINGLETON_GETTER(TYPE, GETTER, GTYPE, ...)            \
    MM_DEFINE_SINGLETON_INSTANCE (TYPE);                                \
    MM_DEFINE_SINGLETON_WEAK_REF (TYPE);                                \
    TYPE *                                                              \
    GETTER (void)                                                       \
    {                                                                   \
        if (G_UNLIKELY (!singleton_instance)) {                         \
            static char _already_created = FALSE;                       \
                                                                        \
            g_assert (!_already_created || (MM_DEFINE_SINGLETON_ALLOW_MULTIPLE));        \
            _already_created = TRUE;                                                     \
            singleton_instance = (g_object_new (GTYPE, ##__VA_ARGS__, NULL));            \
            g_assert (singleton_instance);                                               \
            mm_singleton_instance_weak_ref_register ();                                  \
            mm_dbg ("create %s singleton (%p)", G_STRINGIFY (TYPE), singleton_instance); \
        }                                                               \
        return singleton_instance;                                      \
    }                                                                   \
    MM_DEFINE_SINGLETON_DESTRUCTOR(TYPE)

#endif /* MM_UTILS_H */
