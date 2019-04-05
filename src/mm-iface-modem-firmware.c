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

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-iface-modem.h"
#include "mm-iface-modem-firmware.h"
#include "mm-log.h"

/*****************************************************************************/

void
mm_iface_modem_firmware_bind_simple_status (MMIfaceModemFirmware *self,
                                             MMSimpleStatus *status)
{
}

/*****************************************************************************/
/* Handle the 'List' method from DBus */

typedef struct {
    MMIfaceModemFirmware *self;
    MmGdbusModemFirmware *skeleton;
    GDBusMethodInvocation *invocation;
    GList *list;
    MMFirmwareProperties *current;
} HandleListContext;

static void
handle_list_context_free (HandleListContext *ctx)
{
    if (ctx->list)
        g_list_free_full (ctx->list, g_object_unref);
    if (ctx->current)
        g_object_unref (ctx->current);
    g_object_unref (ctx->skeleton);
    g_object_unref (ctx->invocation);
    g_object_unref (ctx->self);
    g_slice_free (HandleListContext, ctx);
}

static void
load_current_ready (MMIfaceModemFirmware *self,
                    GAsyncResult *res,
                    HandleListContext *ctx)
{
    GVariantBuilder builder;
    GList *l;
    GError *error = NULL;

    ctx->current = MM_IFACE_MODEM_FIRMWARE_GET_INTERFACE (self)->load_current_finish (self, res, &error);
    if (!ctx->current) {
        /* Not found isn't fatal */
        if (!g_error_matches (error, MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND)) {
            g_dbus_method_invocation_take_error (ctx->invocation, error);
            handle_list_context_free (ctx);
            return;
        }
        mm_dbg ("Couldn't load current firmware image: %s", error->message);
        g_clear_error (&error);
    }

    /* Build array of dicts */
    g_variant_builder_init (&builder, G_VARIANT_TYPE ("aa{sv}"));
    for (l = ctx->list; l; l = g_list_next (l))
        g_variant_builder_add_value (
            &builder,
            mm_firmware_properties_get_dictionary (MM_FIRMWARE_PROPERTIES (l->data)));

    mm_gdbus_modem_firmware_complete_list (
        ctx->skeleton,
        ctx->invocation,
        (ctx->current ? mm_firmware_properties_get_unique_id (ctx->current) : ""),
        g_variant_builder_end (&builder));
    handle_list_context_free (ctx);
}

static void
load_list_ready (MMIfaceModemFirmware *self,
                 GAsyncResult *res,
                 HandleListContext *ctx)
{
    GError *error = NULL;

    ctx->list = MM_IFACE_MODEM_FIRMWARE_GET_INTERFACE (self)->load_list_finish (self, res, &error);
    if (!ctx->list) {
        /* Not found isn't fatal */
        if (!g_error_matches (error, MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND)) {
            g_dbus_method_invocation_take_error (ctx->invocation, error);
            handle_list_context_free (ctx);
            return;
        }
        mm_dbg ("Couldn't load firmware image list: %s", error->message);
        g_clear_error (&error);
    }

    MM_IFACE_MODEM_FIRMWARE_GET_INTERFACE (self)->load_current (MM_IFACE_MODEM_FIRMWARE (self),
                                                                (GAsyncReadyCallback)load_current_ready,
                                                                ctx);
}

static void
list_auth_ready (MMBaseModem *self,
                 GAsyncResult *res,
                 HandleListContext *ctx)
{
    GError *error = NULL;

    if (!mm_base_modem_authorize_finish (self, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_list_context_free (ctx);
        return;
    }

    if (!MM_IFACE_MODEM_FIRMWARE_GET_INTERFACE (self)->load_list ||
        !MM_IFACE_MODEM_FIRMWARE_GET_INTERFACE (self)->load_list_finish ||
        !MM_IFACE_MODEM_FIRMWARE_GET_INTERFACE (self)->load_current ||
        !MM_IFACE_MODEM_FIRMWARE_GET_INTERFACE (self)->load_current_finish) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_UNSUPPORTED,
                                               "Cannot list firmware: operation not supported");
        handle_list_context_free (ctx);
        return;
    }

    MM_IFACE_MODEM_FIRMWARE_GET_INTERFACE (self)->load_list (MM_IFACE_MODEM_FIRMWARE (self),
                                                             (GAsyncReadyCallback)load_list_ready,
                                                             ctx);
}

static gboolean
handle_list (MmGdbusModemFirmware *skeleton,
             GDBusMethodInvocation *invocation,
             MMIfaceModemFirmware *self)
{
    HandleListContext *ctx;

    ctx = g_slice_new (HandleListContext);
    ctx->skeleton = g_object_ref (skeleton);
    ctx->invocation = g_object_ref (invocation);
    ctx->self = g_object_ref (self);

    mm_base_modem_authorize (MM_BASE_MODEM (self),
                             invocation,
                             MM_AUTHORIZATION_DEVICE_CONTROL,
                             (GAsyncReadyCallback)list_auth_ready,
                             ctx);

    return TRUE;
}

/*****************************************************************************/
/* Handle the 'Select' method from DBus */

typedef struct {
    MMIfaceModemFirmware *self;
    MmGdbusModemFirmware *skeleton;
    GDBusMethodInvocation *invocation;
    gchar *name;
} HandleSelectContext;

static void
handle_select_context_free (HandleSelectContext *ctx)
{
    g_free (ctx->name);
    g_object_unref (ctx->skeleton);
    g_object_unref (ctx->invocation);
    g_object_unref (ctx->self);
    g_slice_free (HandleSelectContext, ctx);
}

static void
change_current_ready (MMIfaceModemFirmware *self,
                      GAsyncResult *res,
                      HandleSelectContext *ctx)
{
    GError *error = NULL;

    if (!MM_IFACE_MODEM_FIRMWARE_GET_INTERFACE (self)->change_current_finish (self, res, &error))
        g_dbus_method_invocation_take_error (ctx->invocation, error);
    else
        mm_gdbus_modem_firmware_complete_select (ctx->skeleton, ctx->invocation);
    handle_select_context_free (ctx);
}

static void
select_auth_ready (MMBaseModem *self,
                   GAsyncResult *res,
                   HandleSelectContext *ctx)
{
    GError *error = NULL;

    if (!mm_base_modem_authorize_finish (self, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_select_context_free (ctx);
        return;
    }


    if (!MM_IFACE_MODEM_FIRMWARE_GET_INTERFACE (self)->change_current ||
        !MM_IFACE_MODEM_FIRMWARE_GET_INTERFACE (self)->change_current_finish) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_UNSUPPORTED,
                                               "Cannot select firmware: operation not supported");
        handle_select_context_free (ctx);
        return;
    }

    MM_IFACE_MODEM_FIRMWARE_GET_INTERFACE (self)->change_current (MM_IFACE_MODEM_FIRMWARE (self),
                                                                  ctx->name,
                                                                  (GAsyncReadyCallback)change_current_ready,
                                                                  ctx);
}

static gboolean
handle_select (MmGdbusModemFirmware *skeleton,
               GDBusMethodInvocation *invocation,
               const gchar *name,
               MMIfaceModemFirmware *self)
{
    HandleSelectContext *ctx;

    ctx = g_slice_new (HandleSelectContext);
    ctx->skeleton = g_object_ref (skeleton);
    ctx->invocation = g_object_ref (invocation);
    ctx->self = g_object_ref (self);
    ctx->name = g_strdup (name);

    mm_base_modem_authorize (MM_BASE_MODEM (self),
                             invocation,
                             MM_AUTHORIZATION_DEVICE_CONTROL,
                             (GAsyncReadyCallback)select_auth_ready,
                             ctx);

    return TRUE;
}

/*****************************************************************************/

typedef struct _InitializationContext InitializationContext;
static void interface_initialization_step (GTask *task);

typedef enum {
    INITIALIZATION_STEP_FIRST,
    INITIALIZATION_STEP_UPDATE_SETTINGS,
    INITIALIZATION_STEP_LAST
} InitializationStep;

struct _InitializationContext {
    MmGdbusModemFirmware *skeleton;
    InitializationStep    step;
};

static void
initialization_context_free (InitializationContext *ctx)
{
    g_object_unref (ctx->skeleton);
    g_free (ctx);
}

gboolean
mm_iface_modem_firmware_initialize_finish (MMIfaceModemFirmware  *self,
                                           GAsyncResult          *res,
                                           GError               **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static gboolean
add_generic_version (MMBaseModem               *self,
                     MMFirmwareUpdateSettings  *update_settings,
                     GError                   **error)
{
    const gchar *firmware_revision;
    const gchar *carrier_revision;
    gchar       *combined;

    firmware_revision = mm_iface_modem_get_revision (MM_IFACE_MODEM (self));
    if (!firmware_revision) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "Unknown revision");
        return FALSE;
    }

    mm_iface_modem_get_carrier_config (MM_IFACE_MODEM (self), NULL, &carrier_revision);

    if (!carrier_revision) {
        mm_firmware_update_settings_set_version (update_settings, firmware_revision);
        return TRUE;
    }

    combined = g_strdup_printf ("%s - %s", firmware_revision, carrier_revision);
    mm_firmware_update_settings_set_version (update_settings, combined);
    g_free (combined);
    return TRUE;
}

static gboolean
add_generic_device_ids (MMBaseModem               *self,
                        MMFirmwareUpdateSettings  *update_settings,
                        GError                   **error)
{
    guint16      vid;
    guint16      pid;
    guint16      rid;
    GPtrArray   *ids;
    MMPort      *primary = NULL;
    const gchar *subsystem;
    const gchar *aux;

    vid = mm_base_modem_get_vendor_id (self);
    pid = mm_base_modem_get_product_id (self);

#if defined WITH_QMI
    primary = MM_PORT (mm_base_modem_peek_port_qmi (self));
#endif
#if defined WITH_MBIM
    if (!primary)
        primary = MM_PORT (mm_base_modem_peek_port_mbim (self));
#endif
    if (!primary)
        primary = MM_PORT (mm_base_modem_peek_port_primary (self));
    g_assert (primary != NULL);
    rid = mm_kernel_device_get_physdev_revision (mm_port_peek_kernel_device (primary));

    subsystem = mm_kernel_device_get_physdev_subsystem (mm_port_peek_kernel_device (primary));
    if (g_strcmp0 (subsystem, "usb")) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "Unsupported subsystem: %s", subsystem);
        return FALSE;
    }

    mm_iface_modem_get_carrier_config (MM_IFACE_MODEM (self), &aux, NULL);

    ids = g_ptr_array_new_with_free_func ((GDestroyNotify)g_free);
    if (aux) {
        gchar *carrier;

        carrier = g_ascii_strup (aux, -1);
        g_ptr_array_add (ids, g_strdup_printf ("USB\\VID_%04X&PID_%04X&REV_%04X&CARRIER_%s", vid, pid, rid, carrier));
        g_free (carrier);
    }
    g_ptr_array_add (ids, g_strdup_printf ("USB\\VID_%04X&PID_%04X&REV_%04X", vid, pid, rid));
    g_ptr_array_add (ids, g_strdup_printf ("USB\\VID_%04X&PID_%04X", vid, pid));
    g_ptr_array_add (ids, g_strdup_printf ("USB\\VID_%04X", vid));
    g_ptr_array_add (ids, NULL);

    mm_firmware_update_settings_set_device_ids (update_settings, (const gchar **)ids->pdata);
    g_ptr_array_unref (ids);
    return TRUE;
}

static void
load_update_settings_ready (MMIfaceModemFirmware *self,
                            GAsyncResult         *res,
                            GTask                *task)
{
    InitializationContext    *ctx;
    MMFirmwareUpdateSettings *update_settings;
    GError                   *error = NULL;
    GVariant                 *variant = NULL;

    ctx = g_task_get_task_data (task);

    update_settings = MM_IFACE_MODEM_FIRMWARE_GET_INTERFACE (self)->load_update_settings_finish (self, res, &error);
    if (!update_settings) {
        mm_dbg ("Couldn't load update settings: '%s'", error->message);
        g_error_free (error);
        goto out;
    }

    /* If the plugin didn't specify custom device ids, add the default ones ourselves */
    if (!mm_firmware_update_settings_get_device_ids (update_settings) &&
        !add_generic_device_ids (MM_BASE_MODEM (self), update_settings, &error)) {
        mm_warn ("Couldn't build device ids: '%s'", error->message);
        g_error_free (error);
        g_clear_object (&update_settings);
        goto out;
    }

    /* If the plugin didn't specify custom version, add the default one ourselves */
    if (!mm_firmware_update_settings_get_version (update_settings) &&
        !add_generic_version (MM_BASE_MODEM (self), update_settings, &error)) {
        mm_warn ("Couldn't set version: '%s'", error->message);
        g_error_free (error);
        g_clear_object (&update_settings);
        goto out;
    }

out:
    if (update_settings) {
        variant = mm_firmware_update_settings_get_variant (update_settings);
        g_object_unref (update_settings);
    }
    mm_gdbus_modem_firmware_set_update_settings (ctx->skeleton, variant);
    if (variant)
        g_variant_unref (variant);

    /* Go on to next step */
    ctx->step++;
    interface_initialization_step (task);
}

static void
interface_initialization_step (GTask *task)
{
    MMIfaceModemFirmware *self;
    InitializationContext *ctx;

    /* Don't run new steps if we're cancelled */
    if (g_task_return_error_if_cancelled (task)) {
        g_object_unref (task);
        return;
    }

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    switch (ctx->step) {
    case INITIALIZATION_STEP_FIRST:
        /* Fall down to next step */
        ctx->step++;

    case INITIALIZATION_STEP_UPDATE_SETTINGS:
        if (MM_IFACE_MODEM_FIRMWARE_GET_INTERFACE (self)->load_update_settings &&
            MM_IFACE_MODEM_FIRMWARE_GET_INTERFACE (self)->load_update_settings_finish) {
            MM_IFACE_MODEM_FIRMWARE_GET_INTERFACE (self)->load_update_settings (
                self,
                (GAsyncReadyCallback)load_update_settings_ready,
                task);
            return;
        }
        /* Fall down to next step */
        ctx->step++;

    case INITIALIZATION_STEP_LAST:
        /* We are done without errors! */

        /* Handle method invocations */
        g_object_connect (ctx->skeleton,
                          "signal::handle-list",   G_CALLBACK (handle_list),   self,
                          "signal::handle-select", G_CALLBACK (handle_select), self,
                          NULL);

        /* Finally, export the new interface */
        mm_gdbus_object_skeleton_set_modem_firmware (MM_GDBUS_OBJECT_SKELETON (self),
                                                     MM_GDBUS_MODEM_FIRMWARE (ctx->skeleton));

        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    g_assert_not_reached ();
}

void
mm_iface_modem_firmware_initialize (MMIfaceModemFirmware *self,
                                    GCancellable         *cancellable,
                                    GAsyncReadyCallback   callback,
                                    gpointer              user_data)
{
    InitializationContext *ctx;
    MmGdbusModemFirmware  *skeleton = NULL;
    GTask                 *task;

    /* Did we already create it? */
    g_object_get (self,
                  MM_IFACE_MODEM_FIRMWARE_DBUS_SKELETON, &skeleton,
                  NULL);
    if (!skeleton) {
        skeleton = mm_gdbus_modem_firmware_skeleton_new ();
        g_object_set (self,
                      MM_IFACE_MODEM_FIRMWARE_DBUS_SKELETON, skeleton,
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
mm_iface_modem_firmware_shutdown (MMIfaceModemFirmware *self)
{
    g_return_if_fail (MM_IS_IFACE_MODEM_FIRMWARE (self));

    /* Unexport DBus interface and remove the skeleton */
    mm_gdbus_object_skeleton_set_modem_firmware (MM_GDBUS_OBJECT_SKELETON (self), NULL);
    g_object_set (self,
                  MM_IFACE_MODEM_FIRMWARE_DBUS_SKELETON, NULL,
                  NULL);
}

/*****************************************************************************/

static void
iface_modem_firmware_init (gpointer g_iface)
{
    static gboolean initialized = FALSE;

    if (initialized)
        return;

    /* Properties */
    g_object_interface_install_property
        (g_iface,
         g_param_spec_object (MM_IFACE_MODEM_FIRMWARE_DBUS_SKELETON,
                              "Firmware DBus skeleton",
                              "DBus skeleton for the Firmware interface",
                              MM_GDBUS_TYPE_MODEM_FIRMWARE_SKELETON,
                              G_PARAM_READWRITE));

    initialized = TRUE;
}

GType
mm_iface_modem_firmware_get_type (void)
{
    static GType iface_modem_firmware_type = 0;

    if (!G_UNLIKELY (iface_modem_firmware_type)) {
        static const GTypeInfo info = {
            sizeof (MMIfaceModemFirmware), /* class_size */
            iface_modem_firmware_init,     /* base_init */
            NULL,                           /* base_finalize */
        };

        iface_modem_firmware_type = g_type_register_static (G_TYPE_INTERFACE,
                                                            "MMIfaceModemFirmware",
                                                            &info,
                                                            0);

        g_type_interface_add_prerequisite (iface_modem_firmware_type, MM_TYPE_IFACE_MODEM);
    }

    return iface_modem_firmware_type;
}
