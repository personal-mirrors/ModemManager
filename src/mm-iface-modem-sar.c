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
 * Copyright TODO
 */

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-iface-modem.h"
#include "mm-iface-modem-sar.h"
#include "mm-log-object.h"

/*****************************************************************************/

typedef struct _InitializationContext InitializationContext;
static void interface_initialization_step (GTask *task);

typedef enum {
    INITIALIZATION_STEP_FIRST,
    INITIALIZATION_STEP_LAST
} InitializationStep;

struct _InitializationContext {
    MmGdbusModemSar *skeleton;
    InitializationStep    step;
};

static void
initialization_context_free (InitializationContext *ctx)
{
    g_object_unref (ctx->skeleton);
    g_free (ctx);
}

gboolean
mm_iface_modem_sar_initialize_finish (MMIfaceModemSar  *self,
                                      GAsyncResult     *res,
                                      GError          **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}


static void
interface_initialization_step (GTask *task)
{
    //TODO
}

void
mm_iface_modem_sar_initialize (MMIfaceModemSar      *self,
                               GCancellable         *cancellable,
                               GAsyncReadyCallback   callback,
                               gpointer              user_data)
{
    InitializationContext *ctx;
    MmGdbusModemSar  *skeleton = NULL;
    GTask                 *task;

    /* Did we already create it? */
    g_object_get (self,
                  MM_IFACE_MODEM_SAR_DBUS_SKELETON, &skeleton,
                  NULL);
    if (!skeleton) {
        skeleton = mm_gdbus_modem_sar_skeleton_new ();
        g_object_set (self,
                      MM_IFACE_MODEM_SAR_DBUS_SKELETON, skeleton,
                      NULL);
    }

    /* Perform async initialization here */
    ctx = g_new0 (InitializationContext, 1);
    ctx->step = INITIALIZATION_STEP_FIRST;
    ctx->skeleton = skeleton;

    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify)initialization_context_free);

    interface_initialization_step (task);
}

void
mm_iface_modem_sar_shutdown (MMIfaceModemSar *self)
{
    g_return_if_fail (MM_IS_IFACE_MODEM_SAR (self));

    /* Unexport DBus interface and remove the skeleton */
    mm_gdbus_object_skeleton_set_modem_sar (MM_GDBUS_OBJECT_SKELETON (self), NULL);
    g_object_set (self,
                  MM_IFACE_MODEM_SAR_DBUS_SKELETON, NULL,
                  NULL);
}

/*****************************************************************************/

static void
iface_modem_sar_init (gpointer g_iface)
{
    static gboolean initialized = FALSE;

    if (initialized)
        return;

    /* Properties */
    g_object_interface_install_property
        (g_iface,
         g_param_spec_object (MM_IFACE_MODEM_SAR_DBUS_SKELETON,
                              "Sar DBus skeleton",
                              "DBus skeleton for the Sar interface",
                              MM_GDBUS_TYPE_MODEM_SAR_SKELETON,
                              G_PARAM_READWRITE));
    initialized = TRUE;
}

GType
mm_iface_modem_sar_get_type (void)
{
    static GType iface_modem_sar_type = 0;

    if (!G_UNLIKELY (iface_modem_sar_type)) {
        static const GTypeInfo info = {
            sizeof (MMIfaceModemSar), /* class_size */
            iface_modem_sar_init,     /* base_init */
            NULL,                           /* base_finalize */
        };

        iface_modem_sar_type = g_type_register_static (G_TYPE_INTERFACE,
                                                            "MMIfaceModemSar",
                                                            &info,
                                                            0);

        g_type_interface_add_prerequisite (iface_modem_sar_type, MM_TYPE_IFACE_MODEM);
    }

    return iface_modem_sar_type;
}
