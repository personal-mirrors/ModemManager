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
 * Copyright (C) 2021 Fibocom Wireless Inc.
 */

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-iface-modem.h"
#include "mm-iface-modem-rf.h"
#include "mm-log-object.h"

#define MM_LOG_NO_OBJECT

#define SUPPORT_CHECKED_TAG "rf-support-checked-tag"
#define SUPPORTED_TAG       "rf-supported-tag"

static GQuark support_checked_quark;
static GQuark supported_quark;

/*****************************************************************************/

void
mm_iface_modem_rf_bind_simple_status (MMIfaceModemRf *self,
                                      MMSimpleStatus  *status)
{
}

static GVariant *
get_rfim_info_build_result (GList *info_list)
{
    GList *l;
    GVariantBuilder builder;

    g_variant_builder_init (&builder, G_VARIANT_TYPE ("aa{sv}"));

    for (l = info_list; l; l = g_list_next (l)) {
        MMRfInfo *info = l->data;

        g_variant_builder_open (&builder, G_VARIANT_TYPE ("a{sv}"));

        g_variant_builder_add (&builder, "{sv}",
                               "serving-cell-info", g_variant_new_uint32 (info->serving_cell_info));
        g_variant_builder_add (&builder, "{sv}",
                               "center-frequency", g_variant_new_uint64 (info->center_frequency));
        g_variant_builder_add (&builder, "{sv}",
                               "bandwidth", g_variant_new_uint32 (info->bandwidth));
        g_variant_builder_add (&builder, "{sv}",
                               "rsrp", g_variant_new_uint32 (info->rsrp));
        g_variant_builder_add (&builder, "{sv}",
                               "rsrq", g_variant_new_uint32 (info->rsrq));
        g_variant_builder_add (&builder, "{sv}",
                               "sinr", g_variant_new_uint32 (info->sinr));
        g_variant_builder_add (&builder, "{sv}",
                               "rssi", g_variant_new_uint32 (info->rssi));
        g_variant_builder_add (&builder, "{sv}",
                               "connection-status", g_variant_new_uint32 (info->connection_status));
       g_variant_builder_close (&builder);
    }

    return g_variant_ref (g_variant_builder_end (&builder));
}

void
mm_iface_modem_rf_update_rf_info (MMIfaceModemRf *self,GList *info_list)
{
    MmGdbusModemRf       *skeleton = NULL;
    g_autoptr(GVariant)  dict_array = NULL;

    g_object_get (self,
                  MM_IFACE_MODEM_RF_DBUS_SKELETON, &skeleton,
                  NULL);

    if (!skeleton)
      return;


    /* Update the received RF data in the property*/
    dict_array = get_rfim_info_build_result (info_list);
    mm_gdbus_modem_rf_set_rf_inf (skeleton,dict_array);
    /* Flush right away */
    g_dbus_interface_skeleton_flush (G_DBUS_INTERFACE_SKELETON (skeleton));
    mm_rfim_info_list_free (info_list);
}

/*****************************************************************************/
/* Handle SetupRfInfo() */

typedef struct {
    MmGdbusModemRf *skeleton;
    GDBusMethodInvocation *invocation;
    MMIfaceModemRf *self;
    gboolean enable;
} HandleSetupRFInfoContext;

static void
handle_setup_rf_info_context_free (HandleSetupRFInfoContext *ctx)
{
    g_object_unref (ctx->skeleton);
    g_object_unref (ctx->invocation);
    g_object_unref (ctx->self);
    g_slice_free (HandleSetupRFInfoContext, ctx);
}

static void
handle_setup_rf_info_ready (MMIfaceModemRf          *self,
                            GAsyncResult             *res,
                            HandleSetupRFInfoContext *ctx)
{
    GError *error = NULL;

    if (!MM_IFACE_MODEM_RF_GET_INTERFACE (ctx->self)->setup_rf_info_finish (self, res, &error))
        g_dbus_method_invocation_take_error (ctx->invocation, error);
    else
        mm_gdbus_modem_rf_complete_setup_rf_info (ctx->skeleton, ctx->invocation);

    handle_setup_rf_info_context_free (ctx);
}

static void
handle_setup_rf_info_auth_ready (MMBaseModem              *self,
                                 GAsyncResult             *res,
                                 HandleSetupRFInfoContext *ctx)
{
    GError *error = NULL;

    if (!mm_base_modem_authorize_finish (self, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_setup_rf_info_context_free (ctx);
        return;
    }

    if (!MM_IFACE_MODEM_RF_GET_INTERFACE (ctx->self)->setup_rf_info ||
        !MM_IFACE_MODEM_RF_GET_INTERFACE (ctx->self)->setup_rf_info_finish) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_UNSUPPORTED,
                                               "Cannot setup RF: "
                                               "operation not supported");
        handle_setup_rf_info_context_free (ctx);
        return;
    }

    mm_obj_dbg (self, "%s RF...", ctx->enable ? "Enabling" : "Disabling");

    MM_IFACE_MODEM_RF_GET_INTERFACE (ctx->self)->setup_rf_info (ctx->self,
                                                                ctx->enable,
                                                                (GAsyncReadyCallback)handle_setup_rf_info_ready,
                                                                ctx);
}

static gboolean
handle_setup_rf_info (MmGdbusModemRf        *skeleton,
                      GDBusMethodInvocation *invocation,
                      gboolean               enable,
                      MMIfaceModemRf        *self)
{
    HandleSetupRFInfoContext *ctx;

    ctx = g_slice_new0 (HandleSetupRFInfoContext);
    ctx->skeleton = g_object_ref (skeleton);
    ctx->invocation = g_object_ref (invocation);
    ctx->self = g_object_ref (self);
    ctx->enable = enable;

    mm_base_modem_authorize (MM_BASE_MODEM (self),
                             invocation,
                             MM_AUTHORIZATION_DEVICE_CONTROL,
                             (GAsyncReadyCallback)handle_setup_rf_info_auth_ready,
                             ctx);
    return TRUE;
}

/*****************************************************************************/
/* Handle GetRfInfo() */

typedef struct {
    MmGdbusModemRf *skeleton;
    GDBusMethodInvocation *invocation;
    MMIfaceModemRf *self;
} HandleGetRfIinfoContext;

static void
handle_get_rf_info_context_free (HandleGetRfIinfoContext *ctx)
{
    g_object_unref (ctx->skeleton);
    g_object_unref (ctx->invocation);
    g_object_unref (ctx->self);
    g_slice_free (HandleGetRfIinfoContext, ctx);
}

static void
handle_get_rf_info_ready (MMIfaceModemRf     *self,
                          GAsyncResult        *res,
                          HandleGetRfIinfoContext *ctx)
{
    GError *error = NULL;
    GList *info_list;

    info_list = MM_IFACE_MODEM_RF_GET_INTERFACE (ctx->self)->get_rf_info_finish (self, res, &error);

    if (error)
        g_dbus_method_invocation_take_error (ctx->invocation, error);
    else {
        /* Update the received RF data in the property*/
        g_autoptr(GVariant) dict_array;

        dict_array = get_rfim_info_build_result (info_list);
        mm_gdbus_modem_rf_set_rf_inf (ctx->skeleton,dict_array);
        mm_gdbus_modem_rf_complete_get_rf_info (ctx->skeleton, ctx->invocation);
    }
    handle_get_rf_info_context_free (ctx);
}

static void
handle_get_rf_info_auth_ready (MMBaseModem             *self,
                               GAsyncResult            *res,
                               HandleGetRfIinfoContext *ctx)
{
    GError *error = NULL;

    if (!mm_base_modem_authorize_finish (self, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_get_rf_info_context_free (ctx);
        return;
    }

    if (!MM_IFACE_MODEM_RF_GET_INTERFACE (ctx->self)->get_rf_info ||
        !MM_IFACE_MODEM_RF_GET_INTERFACE (ctx->self)->get_rf_info_finish) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_UNSUPPORTED,
                                               "Cannot get RF info: "
                                               "operation not supported");
        handle_get_rf_info_context_free (ctx);
        return;
    }

    mm_obj_dbg (self, "Requesting RF info");

    MM_IFACE_MODEM_RF_GET_INTERFACE (ctx->self)->get_rf_info (ctx->self,
                                                              (GAsyncReadyCallback)handle_get_rf_info_ready,
                                                              ctx);
}

static gboolean
handle_get_rf_info (MmGdbusModemRf        *skeleton,
                    GDBusMethodInvocation *invocation,
                    MMIfaceModemRf        *self)
{
    HandleGetRfIinfoContext *ctx;

    ctx = g_slice_new0 (HandleGetRfIinfoContext);
    ctx->skeleton = g_object_ref (skeleton);
    ctx->invocation = g_object_ref (invocation);
    ctx->self = g_object_ref (self);

    mm_base_modem_authorize (MM_BASE_MODEM (self),
                             invocation,
                             MM_AUTHORIZATION_DEVICE_CONTROL,
                             (GAsyncReadyCallback)handle_get_rf_info_auth_ready,
                             ctx);
    return TRUE;
}

/*****************************************************************************/

typedef struct _InitializationContext InitializationContext;
static void interface_initialization_step (GTask *task);

typedef enum {
    INITIALIZATION_STEP_FIRST,
    INITIALIZATION_STEP_CHECK_SUPPORT,
    INITIALIZATION_STEP_LAST
} InitializationStep;

struct _InitializationContext {
    MmGdbusModemRf     *skeleton;
    InitializationStep  step;
};

static void
initialization_context_free (InitializationContext *ctx)
{
    g_object_unref (ctx->skeleton);
    g_free (ctx);
}

gboolean
mm_iface_modem_rf_initialize_finish (MMIfaceModemRf  *self,
                                     GAsyncResult    *res,
                                     GError         **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
check_support_ready (MMIfaceModemRf *self,
                     GAsyncResult    *res,
                     GTask           *task)
{
    InitializationContext  *ctx;
    GError                 *error = NULL;

    if (!MM_IFACE_MODEM_RF_GET_INTERFACE (self)->check_support_finish (self, res, &error)) {
        if (error) {
            /* This error shouldn't be treated as critical */
            mm_obj_dbg (self, "RF support check failed: %s", error->message);
            g_error_free (error);
        }
    } else {
        /* RF is supported! */
        g_object_set_qdata (G_OBJECT (self),
                            supported_quark,
                            GUINT_TO_POINTER (TRUE));
    }

    /* Go on to next step */
    ctx = g_task_get_task_data (task);
    ctx->step++;
    interface_initialization_step (task);
}

static void
interface_initialization_step (GTask *task)
{
    MMIfaceModemRf         *self;
    InitializationContext  *ctx;

    /* Don't run new steps if we're cancelled */
    if (g_task_return_error_if_cancelled (task)) {
        g_object_unref (task);
        return;
    }

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    switch (ctx->step) {

    case INITIALIZATION_STEP_FIRST:
        /* Setup quarks if we didn't do it before */
        if (G_UNLIKELY (!support_checked_quark))
            support_checked_quark = (g_quark_from_static_string (
                                         SUPPORT_CHECKED_TAG));
        if (G_UNLIKELY (!supported_quark))
            supported_quark = (g_quark_from_static_string (
                                   SUPPORTED_TAG));
        ctx->step++;
        /* fall through */

    case INITIALIZATION_STEP_CHECK_SUPPORT:
        if (!GPOINTER_TO_UINT (g_object_get_qdata (G_OBJECT (self),
                                                   support_checked_quark))) {
            /* Set the checked flag so that we don't run it again */
            g_object_set_qdata (G_OBJECT (self),
                                support_checked_quark,
                                GUINT_TO_POINTER (TRUE));
            /* Initially, assume we don't support it */
            g_object_set_qdata (G_OBJECT (self),
                                supported_quark,
                                GUINT_TO_POINTER (FALSE));

            if (MM_IFACE_MODEM_RF_GET_INTERFACE (self)->check_support &&
                MM_IFACE_MODEM_RF_GET_INTERFACE (self)->check_support_finish) {
                MM_IFACE_MODEM_RF_GET_INTERFACE (self)->check_support (
                    self,
                    (GAsyncReadyCallback)check_support_ready,
                    task);
                return;
            }

            /* If there is no implementation to check support, assume we DON'T
             * support it. */
        }
        ctx->step++;
        /* fall through */

    case INITIALIZATION_STEP_LAST:
        /* We are done without errors! */

        /* Handle method invocations */
        g_signal_connect (ctx->skeleton,
                          "handle-setup-rf-info",
                          G_CALLBACK (handle_setup_rf_info),
                          self);
        g_signal_connect (ctx->skeleton,
                          "handle-get-rf-info",
                          G_CALLBACK (handle_get_rf_info),
                          self);

        /* Finally, export the new interface */
        mm_gdbus_object_skeleton_set_modem_rf (MM_GDBUS_OBJECT_SKELETON (self),
                                               MM_GDBUS_MODEM_RF (ctx->skeleton));
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;

    default:
        break;
    }

    g_assert_not_reached ();
}

void
mm_iface_modem_rf_initialize (MMIfaceModemRf       *self,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
    InitializationContext  *ctx;
    MmGdbusModemRf         *skeleton = NULL;
    GTask                  *task;

    /* Did we already create it? */
    g_object_get (self,
                  MM_IFACE_MODEM_RF_DBUS_SKELETON, &skeleton,
                  NULL);

    if (!skeleton) {
        skeleton = mm_gdbus_modem_rf_skeleton_new ();
        g_object_set (self,
                      MM_IFACE_MODEM_RF_DBUS_SKELETON, skeleton,
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

/*****************************************************************************/

void
mm_iface_modem_rf_shutdown (MMIfaceModemRf *self)
{
    /* Unexport DBus interface and remove the skeleton */
    mm_gdbus_object_skeleton_set_modem_rf (MM_GDBUS_OBJECT_SKELETON (self), NULL);
    g_object_set (self,
                  MM_IFACE_MODEM_RF_DBUS_SKELETON, NULL,
                  NULL);
}

/*****************************************************************************/

static void
iface_modem_rf_init (gpointer g_iface)
{
    static gboolean initialized = FALSE;

    if (initialized)
        return;

    /* Properties */
    g_object_interface_install_property
        (g_iface,
         g_param_spec_object (MM_IFACE_MODEM_RF_DBUS_SKELETON,
                              "RF DBus skeleton",
                              "DBus skeleton for the RF interface",
                              MM_GDBUS_TYPE_MODEM_RF_SKELETON,
                              G_PARAM_READWRITE));
    initialized = TRUE;
}

GType
mm_iface_modem_rf_get_type (void)
{
    static GType iface_modem_rf_type = 0;

    if (!G_UNLIKELY (iface_modem_rf_type)) {
        static const GTypeInfo info = {
            sizeof (MMIfaceModemRf), /* class_size */
            iface_modem_rf_init,     /* base_init */
            NULL,                     /* base_finalize */
        };

        iface_modem_rf_type = g_type_register_static (G_TYPE_INTERFACE,
                                                      "MMIfaceModemRf",
                                                      &info,
                                                      0);

        g_type_interface_add_prerequisite (iface_modem_rf_type, MM_TYPE_IFACE_MODEM);
    }

    return iface_modem_rf_type;
}

