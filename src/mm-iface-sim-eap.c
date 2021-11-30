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
 * Copyright (C) 2021 Google, Inc.
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

#include "mm-iface-sim-eap.h"

#include "mm-error-helpers.h"
#include "mm-iface-modem.h"
#include "mm-log-object.h"

/*****************************************************************************/
/* Helper methods related to building or converting the GVariant for/from DBus */

/* Builds a subvariant of the EAP-SIM GVariant result. Builds a (ut) */
static GVariant *
sim_auth_to_variant (MMIfaceSimEap *self,
                     guint32       sres,
                     guint64       kc)
{
    GVariantBuilder builder;

    /* Allow NULL */
    if (!self)
        return NULL;

    g_return_val_if_fail (MM_IS_IFACE_SIM_EAP (self), NULL);

    g_variant_builder_init (&builder, G_VARIANT_TYPE ("(ut)"));

    g_variant_builder_add (&builder, "u", sres);
    g_variant_builder_add (&builder, "t", kc);

    return g_variant_ref_sink (g_variant_builder_end (&builder));
}

/* Builds a subvariant of the EAP-AKA and EAP-AKAP GVariant result. Builds an ay */
static GVariant *
akas_auth_to_variant (MMIfaceSimEap *self,
                      const guint8  *val,
                      gsize         size)
{
    GVariantBuilder builder;
    gsize i;

    /* Allow NULL */
    if (!self)
        return NULL;

    g_return_val_if_fail (MM_IS_IFACE_SIM_EAP (self), NULL);

    g_variant_builder_init (&builder, G_VARIANT_TYPE ("ay"));

    for (i = 0; i < size; i++)
        g_variant_builder_add (&builder, "y", val[i]);

    return g_variant_ref_sink (g_variant_builder_end (&builder));
}

/* Converts the Sim-Eap GVariant input aay into a const guint8**
 *
 * Note:
 * The straightforward approach to using glib's g_variant_get_bytestring_array
 * is not an option in this scenario, as the aforementioned method requires that
 * input is null-terminated. However this is not to be the case for an array of
 * random bytes where a byte of value 0 would be incorrectly determined as a
 * null character and thus terminate the string */
static const guint8 **
convert_sim_auth_variant (GVariant *value,
                          gsize    *length) {
  const guint8 **strv;
  gsize n;
  gsize i;

    g_variant_get_data (value);
    n = g_variant_n_children (value);
    strv = g_new (const guint8 *, n + 1);

    for (i = 0; i < n; i++)
    {
        GVariant *string;
        gsize    sub_size;

        string = g_variant_get_child_value (value, i);
        strv[i] = (const guint8 *) g_variant_get_fixed_array (string, &sub_size, sizeof (guint8));
        g_variant_unref (string);
    }

    strv[n] = NULL;

    if (length)
        *length = n;

    return strv;
}

/*****************************************************************************/
/* Handle the "SimAuth" method from DBus */

typedef struct {
    MmGdbusSimEap *skeleton;
    GDBusMethodInvocation *invocation;
    MMIfaceSimEap *self;

    /* Expected input list of random number challenges */
    GVariant *randsv;

} HandleSimAuthContext;

static void
handle_sim_auth_context_free (HandleSimAuthContext *ctx)
{
    g_object_unref (ctx->skeleton);
    g_object_unref (ctx->invocation);
    g_object_unref (ctx->self);
    g_variant_unref (ctx->randsv);
    g_slice_free (HandleSimAuthContext , ctx);
}

static void
sim_auth_ready (MMDevice             *device,
                GAsyncResult         *res,
                HandleSimAuthContext *ctx)
{
    GVariantBuilder builder;
    GVariant        *variant;
    GError *error = NULL;
    SimAuthResponse resp;

    if (!MM_IFACE_SIM_EAP_GET_INTERFACE(ctx->self)->sim_auth_finish(device,
                                                                        &resp,
                                                                        res,
                                                                        &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_sim_auth_context_free(ctx);
        return;
    }

    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ut)"));

    g_variant_builder_add_value (&builder,
                                sim_auth_to_variant(ctx->self,
                                                    resp.out_sres1,
                                                    resp.out_kc1));
    g_variant_builder_add_value (&builder,
                                sim_auth_to_variant(ctx->self,
                                                    resp.out_sres2,
                                                    resp.out_kc2));
    /* There are either 2 or 3 tuples, add the remain sres, kc accordingly */
    if (resp.out_n == 3) {
        g_variant_builder_add_value (&builder,
                                     sim_auth_to_variant(ctx->self,
                                                         resp.out_sres3,
                                                         resp.out_kc3));
    }

    variant = g_variant_ref_sink (g_variant_builder_end (&builder));
    g_variant_print (variant, TRUE);
    mm_gdbus_sim_eap_complete_sim_auth (ctx->skeleton, ctx->invocation, variant);
    g_variant_unref (variant);
    handle_sim_auth_context_free (ctx);
}

static void
handle_sim_auth_auth_ready (MMBaseModem *modem,
                            GAsyncResult *res,
                            HandleSimAuthContext *ctx)
{
    gsize                   rands_size;
    GError                  *error = NULL;
    const guint8 **rands = NULL;

    if (!mm_base_modem_authorize_finish (modem, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_sim_auth_context_free (ctx);
        return;
    }

    if (!ctx->self->sim_auth || !ctx->self->sim_auth_finish) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_UNSUPPORTED,
                                               "Cannot authenticate via EAP-SIM: "
                                               "operation not supported");
        handle_sim_auth_context_free (ctx);
        return;
    }

    if (!mm_gdbus_sim_get_active (MM_GDBUS_SIM (ctx->self))) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_UNSUPPORTED,
                                               "authenticate via EAP-SIM: "
                                               "SIM not currently active");
        handle_sim_auth_context_free (ctx);
        return;
    }

    rands = convert_sim_auth_variant (ctx->randsv, &rands_size);

    MM_IFACE_SIM_EAP_GET_INTERFACE (ctx->self)->sim_auth (ctx->self,
                                                          rands,
                                                          rands_size,
                                                          (GAsyncReadyCallback) sim_auth_ready,
                                                          ctx);
}

static gboolean
handle_sim_auth (MmGdbusSimEap         *skeleton,
                 GDBusMethodInvocation *invocation,
                 GVariant              *rands,
                 MMIfaceSimEap         *self)
{
    HandleSimAuthContext *ctx;
    MMBaseModem *modem = NULL;

    ctx = g_slice_new (HandleSimAuthContext);
    ctx->skeleton = g_object_ref (skeleton);
    ctx->invocation = g_object_ref (invocation);
    ctx->self = g_object_ref (self);
    ctx->randsv = g_variant_ref (rands);

    g_object_get (MM_BASE_SIM(self), MM_BASE_SIM_MODEM, &modem, NULL);

    mm_base_modem_authorize (modem,
                             invocation,
                             MM_AUTHORIZATION_DEVICE_CONTROL,
                             (GAsyncReadyCallback)handle_sim_auth_auth_ready,
                             ctx);

    return TRUE;
}

/*****************************************************************************/
/* Handle the "AkaAuth" method from DBus */

typedef struct {
    MmGdbusSimEap *skeleton;
    GDBusMethodInvocation *invocation;
    MMIfaceSimEap *self;

    /* Expected input the random number challenge and auth value */
    GVariant *rand;
    GVariant *autn;

} HandleAkaAuthContext;

static void
handle_aka_auth_context_free (HandleAkaAuthContext *ctx)
{
  g_object_unref (ctx->skeleton);
  g_object_unref (ctx->invocation);
  g_object_unref (ctx->self);
  g_variant_unref (ctx->rand);
  g_variant_unref (ctx->autn);
  g_slice_free (HandleAkaAuthContext, ctx);
}

static void
aka_auth_ready (MMDevice             *device,
                GAsyncResult         *res,
                HandleAkaAuthContext *ctx)
{
    GVariantBuilder builder;
    GVariant *variant;
    AkasAuthResponse resp;
    GError *error = NULL;

    if (!MM_IFACE_SIM_EAP_GET_INTERFACE (ctx->self)->aka_auth_finish (device,
                                                                      &resp,
                                                                      res,
                                                                      &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_aka_auth_context_free (ctx);
        return;
    }

    g_variant_builder_init (&builder, G_VARIANT_TYPE ("(uaay)"));

    g_variant_builder_add (&builder, "u", resp.out_res_len);

    g_variant_builder_open (&builder, G_VARIANT_TYPE ("aay"));
    g_variant_builder_add_value (&builder,
                                 akas_auth_to_variant(ctx->self,
                                                      resp.out_res,
                                                      16));
    g_variant_builder_add_value (&builder,
                                 akas_auth_to_variant(ctx->self,
                                                      resp.out_integrating_key,
                                                      16));
    g_variant_builder_add_value (&builder,
                                 akas_auth_to_variant(ctx->self,
                                                      resp.out_ciphering_key,
                                                      16));
    g_variant_builder_add_value (&builder,
                                 akas_auth_to_variant(ctx->self,
                                                      resp.out_auts,
                                                      14));
    g_variant_builder_close (&builder);

    variant = g_variant_ref_sink (g_variant_builder_end (&builder));
    mm_gdbus_sim_eap_complete_aka_auth (ctx->skeleton, ctx->invocation, variant);
    g_variant_unref (variant);
    handle_aka_auth_context_free (ctx);
}

static void
handle_aka_auth_auth_ready (MMBaseModem *modem,
                            GAsyncResult *res,
                            HandleAkaAuthContext *ctx)
{
    GError       *error = NULL;
    const guint8 *rand;
    const guint8 *autn;
    gsize         rand_size;
    gsize         autn_size;

    if (!mm_base_modem_authorize_finish (modem, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_aka_auth_context_free (ctx);
        return;
    }

    if (!ctx->self->aka_auth || !ctx->self->aka_auth_finish) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_UNSUPPORTED,
                                               "Cannot authenticate via EAP-AKA: "
                                               "operation not supported");
        handle_aka_auth_context_free (ctx);
        return;
    }

    if (!mm_gdbus_sim_get_active (MM_GDBUS_SIM (ctx->self))) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_UNSUPPORTED,
                                               "authenticate via EAP-AKA: "
                                               "SIM not currently active");
        handle_aka_auth_context_free (ctx);
        return;
    }

    rand = (const guint8 *) g_variant_get_fixed_array (ctx->rand, &rand_size, sizeof (guint8));
    autn = (const guint8 *) g_variant_get_fixed_array (ctx->autn, &autn_size, sizeof (guint8));

    MM_IFACE_SIM_EAP_GET_INTERFACE (ctx->self)->aka_auth (ctx->self,
                                                          rand,
                                                          autn,
                                                          (GAsyncReadyCallback) aka_auth_ready,
                                                          ctx);

}

static gboolean
handle_aka_auth (MmGdbusSimEap         *skeleton,
                 GDBusMethodInvocation *invocation,
                 GVariant              *rand,
                 GVariant              *autn,
                 MMIfaceSimEap         *self)
{
    MMBaseModem          *modem = NULL;
    HandleAkaAuthContext *ctx;

    ctx = g_slice_new (HandleAkaAuthContext);
    ctx->skeleton = g_object_ref (skeleton);
    ctx->invocation = g_object_ref (invocation);
    ctx->self = g_object_ref (self);
    ctx->rand = g_variant_ref (rand);
    ctx->autn = g_variant_ref (autn);

    g_object_get (MM_BASE_SIM(self), MM_BASE_SIM_MODEM, &modem, NULL);

    mm_base_modem_authorize (modem,
                             invocation,
                             MM_AUTHORIZATION_DEVICE_CONTROL,
                             (GAsyncReadyCallback)handle_aka_auth_auth_ready,
                             ctx);

    return TRUE;
}

/*****************************************************************************/
/* Handle the "AkapAuth" method from DBus */

typedef struct {
    MmGdbusSimEap *skeleton;
    GDBusMethodInvocation *invocation;
    MMIfaceSimEap *self;

    /* Expected input the random number challenge and auth value */
    GVariant *rand;
    GVariant *autn;
    gchar    *network_name;

} HandleAkapAuthContext;

static void
handle_akap_auth_context_free(HandleAkapAuthContext *ctx)
{
  g_object_unref (ctx->skeleton);
  g_object_unref (ctx->invocation);
  g_object_unref (ctx->self);
  g_variant_unref (ctx->rand);
  g_variant_unref (ctx->autn);
  g_free (ctx->network_name);
  g_slice_free (HandleAkapAuthContext , ctx);
}

static void
akap_auth_ready (MMDevice              *device,
                 GAsyncResult          *res,
                 HandleAkapAuthContext *ctx)
{
    GError          *error = NULL;
    GVariantBuilder  builder;
    GVariant        *variant;
    AkasAuthResponse resp;

    if (!MM_IFACE_SIM_EAP_GET_INTERFACE (ctx->self)->akap_auth_finish (device,
                                                                       &resp,
                                                                       res,
                                                                       &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_akap_auth_context_free (ctx);
        return;
    }

    g_variant_builder_init (&builder, G_VARIANT_TYPE ("(uaay)"));

    g_variant_builder_add (&builder, "u", resp.out_res_len);

    g_variant_builder_open (&builder, G_VARIANT_TYPE ("aay"));
    g_variant_builder_add_value (&builder,
                                 akas_auth_to_variant(ctx->self,
                                                      resp.out_res,
                                                      16));
    g_variant_builder_add_value (&builder,
                                 akas_auth_to_variant(ctx->self,
                                                      resp.out_integrating_key,
                                                      16));
    g_variant_builder_add_value (&builder,
                                 akas_auth_to_variant(ctx->self,
                                                      resp.out_ciphering_key,
                                                      16));
    g_variant_builder_add_value (&builder,
                                 akas_auth_to_variant(ctx->self,
                                                      resp.out_auts,
                                                      14));
    g_variant_builder_close (&builder);

    variant = g_variant_ref_sink (g_variant_builder_end (&builder));
    mm_gdbus_sim_eap_complete_akap_auth (ctx->skeleton, ctx->invocation, variant);
    g_variant_unref (variant);
    handle_akap_auth_context_free (ctx);
}

static void
handle_akap_auth_auth_ready (MMBaseModem *modem,
                             GAsyncResult *res,
                             HandleAkapAuthContext *ctx)
{
    GError *error = NULL;
    const guint8 *rand;
    const guint8 *autn;
    gsize rand_size;
    gsize autn_size;

    if (!mm_base_modem_authorize_finish (modem, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_akap_auth_context_free (ctx);
        return;
    }

    if (!ctx->self->akap_auth || !ctx->self->akap_auth_finish) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_UNSUPPORTED,
                                               "Cannot authenticate via EAP-AKAP: "
                                               "operation not supported");
        handle_akap_auth_context_free (ctx);
        return;
    }

    if (!mm_gdbus_sim_get_active (MM_GDBUS_SIM (ctx->self))) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_UNSUPPORTED,
                                               "authenticate via EAP-AKAP: "
                                               "SIM not currently active");
        handle_akap_auth_context_free (ctx);
        return;
    }

    rand = (const guint8 *) g_variant_get_fixed_array (ctx->rand, &rand_size, sizeof (guint8));
    autn = (const guint8 *) g_variant_get_fixed_array (ctx->autn, &autn_size, sizeof (guint8));

    MM_IFACE_SIM_EAP_GET_INTERFACE (ctx->self)->akap_auth (ctx->self,
                                                           rand,
                                                           autn,
                                                           ctx->network_name,
                                                           (GAsyncReadyCallback) akap_auth_ready,
                                                           ctx);

}

static gboolean
handle_akap_auth (MmGdbusSimEap        *skeleton,
                  GDBusMethodInvocation *invocation,
                  GVariant              *rand,
                  GVariant              *autn,
                  const gchar           *network_name,
                  MMIfaceSimEap         *self)
{
    MMBaseModem           *modem = NULL;
    HandleAkapAuthContext *ctx;

    ctx = g_slice_new (HandleAkapAuthContext);
    ctx->skeleton = g_object_ref (skeleton);
    ctx->invocation = g_object_ref (invocation);
    ctx->self = g_object_ref (self);
    ctx->rand = g_variant_ref (rand);
    ctx->autn = g_variant_ref (autn);
    ctx->network_name = g_strdup (network_name);

    g_object_get (MM_BASE_SIM (self), MM_BASE_SIM_MODEM, &modem, NULL);

    mm_base_modem_authorize (modem,
                             invocation,
                             MM_AUTHORIZATION_DEVICE_CONTROL,
                             (GAsyncReadyCallback)handle_akap_auth_auth_ready,
                             ctx);

    return TRUE;
}

/*****************************************************************************/
/* Initialize and Disable methods */

void
mm_iface_sim_eap_initialize (MMIfaceSimEap *self)
{
    MmGdbusSimEap   *skeleton = NULL;
    GError          *error = NULL;
    GDBusConnection *connection;
    gchar           *path;

    /* Did we already create it? */
    g_object_get (self,
                  MM_IFACE_SIM_EAP_DBUS_SKELETON, &skeleton,
                  NULL);
    if (!skeleton) {
        skeleton = mm_gdbus_sim_eap_skeleton_new ();
        g_object_set (self,
                      MM_IFACE_SIM_EAP_DBUS_SKELETON, skeleton,
                      NULL);
    }

    /* Handle method invocations */
    g_signal_connect (skeleton,
                      "handle-sim-auth",
                      G_CALLBACK (handle_sim_auth),
                      self);
    g_signal_connect (skeleton,
                      "handle-aka-auth",
                      G_CALLBACK (handle_aka_auth),
                      self);
    g_signal_connect (skeleton,
                      "handle-akap-auth",
                      G_CALLBACK (handle_akap_auth),
                      self);

    g_object_get (MM_BASE_SIM (self),
                  MM_BASE_SIM_CONNECTION, &connection,
                  MM_BASE_SIM_PATH,       &path,
                  NULL);

    /* Finally, export the new interface */
    if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (skeleton),
                                           connection,
                                           path,
                                           &error)) {
        mm_obj_warn (self, "couldn't export the SIM-EAP interface to bus: %s, %d",
                     error->message, error->code);
        g_error_free (error);
    }
}

void
mm_iface_sim_eap_disable (MMIfaceSimEap *self)
{
    MmGdbusSimEap *skeleton = NULL;

    g_object_get (self, MM_IFACE_SIM_EAP_DBUS_SKELETON, &skeleton, NULL);
    /* Only unexport if currently exported */
    if (skeleton && g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON (skeleton)))
        g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (skeleton));
}

/*****************************************************************************/

static void
iface_sim_eap_init (gpointer g_iface)
{
    static gboolean initialized = FALSE;

    if (initialized)
        return;

    /* Properties */
    g_object_interface_install_property (
        g_iface,
        g_param_spec_object (MM_IFACE_SIM_EAP_DBUS_SKELETON,
                             "Sim Eap DBus skeleton",
                             "DBus skeleton for the Sim Eap interface",
                             MM_GDBUS_TYPE_SIM_EAP_SKELETON,
                             G_PARAM_READWRITE));

    initialized = TRUE;
}

GType
mm_iface_sim_eap_get_type (void)
{
    static GType iface_sim_eap_type = 0;

    if (!G_UNLIKELY (iface_sim_eap_type)) {
        static const GTypeInfo info = {
            sizeof (MMIfaceSimEap), /* class_size */
            iface_sim_eap_init,     /* base_init */
            NULL,                   /* base_finalize */
        };

        iface_sim_eap_type = g_type_register_static (G_TYPE_INTERFACE,
                                                     "MMIfaceSimEap",
                                                     &info,
                                                     0);
    }

    return iface_sim_eap_type;
}
