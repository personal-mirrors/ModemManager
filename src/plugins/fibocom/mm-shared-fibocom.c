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
 * Copyright (C) 2022 Fibocom Wireless Inc.
 */

#include <config.h>
#include <arpa/inet.h>

#include <glib-object.h>
#include <gio/gio.h>

#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-log-object.h"
#include "mm-broadband-modem.h"
#include "mm-broadband-modem-mbim.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-modem-helpers-mbim.h"
#include "mm-port-mbim.h"
#include "mm-shared-fibocom.h"

/*****************************************************************************/
/* Private data context */

#define PRIVATE_TAG "shared-intel-private-tag"
static GQuark private_quark;

typedef struct {
    /* 3GPP interface support */
    MMIfaceModem3gpp *iface_modem_3gpp_parent;
} Private;

static void
private_free (Private *priv)
{
    g_slice_free (Private, priv);
}

static Private *
get_private (MMSharedFibocom *self)
{
    Private *priv;

    if (G_UNLIKELY (!private_quark))
        private_quark = g_quark_from_static_string (PRIVATE_TAG);

    priv = g_object_get_qdata (G_OBJECT (self), private_quark);
    if (!priv) {
        priv = g_slice_new0 (Private);

        /* Setup parent class' MMIfaceModem3gpp */
        g_assert (MM_SHARED_FIBOCOM_GET_INTERFACE (self)->peek_parent_3gpp_interface);
        priv->iface_modem_3gpp_parent = MM_SHARED_FIBOCOM_GET_INTERFACE (self)->peek_parent_3gpp_interface (self);

        g_object_set_qdata_full (G_OBJECT (self), private_quark, priv, (GDestroyNotify)private_free);
    }

    return priv;
}

/*****************************************************************************/

typedef struct {
    MMBearerProperties *config;
    gboolean            initial_eps_off_on;
} SetInitialEpsBearerSettingsContext;

static void
set_initial_eps_bearer_settings_context_free (SetInitialEpsBearerSettingsContext *ctx)
{
    g_clear_object (&ctx->config);
    g_slice_free (SetInitialEpsBearerSettingsContext, ctx);
}

gboolean
mm_shared_fibocom_set_initial_eps_bearer_settings_finish (MMIfaceModem3gpp  *self,
                                                          GAsyncResult      *res,
                                                          GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
after_attach_apn_modem_power_up_ready (MMIfaceModem *self,
                                       GAsyncResult *res,
                                       GTask        *task)
{
    GError *error = NULL;

    if (!mm_iface_modem_set_power_state_finish (self, res, &error)) {
        mm_obj_warn (self, "failed to power up modem after attach APN settings update: %s", error->message);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    mm_obj_dbg (self, "success toggling modem power up after attach APN");
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}



static gboolean
peek_device (gpointer              self,
             MbimDevice          **o_device,
             GAsyncReadyCallback   callback,
             gpointer              user_data)
{
    MMPortMbim *port;

    port = mm_broadband_modem_mbim_peek_port_mbim (MM_BROADBAND_MODEM_MBIM (self));
    if (!port) {
        g_task_report_new_error (self, callback, user_data, peek_device,
                                 MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "Couldn't peek MBIM port");
        return FALSE;
    }

    *o_device = mm_port_mbim_peek_device (port);
    return TRUE;
}


static void
parent_att_hack_set_lte_attach_configuration_set_ready (MbimDevice   *device,
                                                        GAsyncResult *res,
                                                        GTask        *task)
{
    MbimMessage          *response;
    GError               *error = NULL;

    response = mbim_device_command_finish (device, res, &error);
    if (!response || !mbim_message_response_get_result (response, MBIM_MESSAGE_TYPE_COMMAND_DONE, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);

    if (response)
        mbim_message_unref (response);
}

/* This function is almost identical to before_set_lte_attach_configuration_query_ready in mm-broadband-modem-mbim.c
 * The only difference is the code related to ptr_home/ptr_partner/ptr_non_partner and the flow that is executed after this function. */
static void
parent_att_hack_before_set_lte_attach_configuration_query_ready (MbimDevice   *device,
                                                                 GAsyncResult *res,
                                                                 GTask        *task)
{
    MMBroadbandModemMbim                       *self;
    MbimMessage                                *request;
    MbimMessage                                *response;
    GError                                     *error = NULL;
    MMBearerProperties                         *config;
    guint32                                     n_configurations = 0;
    MbimLteAttachConfiguration                **configurations = NULL;
    gint                                        i;
    MbimLteAttachConfiguration *ptr_home = NULL, *ptr_partner = NULL, *ptr_non_partner = NULL;

    self   = g_task_get_source_object (task);
    config = g_task_get_task_data (task);

    response = mbim_device_command_finish (device, res, &error);
    if (!response ||
        !mbim_message_response_get_result (response, MBIM_MESSAGE_TYPE_COMMAND_DONE, &error) ||
        !mbim_message_ms_basic_connect_extensions_lte_attach_configuration_response_parse (
            response,
            &n_configurations,
            &configurations,
            &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        goto out;
    }

    /* We should always receive 3 configurations but the MBIM API doesn't force
     * that so we'll just assume we don't get always the same fixed number */
    for (i = 0; i < n_configurations; i++) {
        MMBearerIpFamily ip_family;
        MMBearerAllowedAuth auth;

        /* We only support configuring the HOME settings */
        if (configurations[i]->roaming != MBIM_LTE_ATTACH_CONTEXT_ROAMING_CONTROL_HOME)
            continue;

        ip_family = mm_bearer_properties_get_ip_type (config);
        if (ip_family == MM_BEARER_IP_FAMILY_NONE || ip_family == MM_BEARER_IP_FAMILY_ANY)
            configurations[i]->ip_type = MBIM_CONTEXT_IP_TYPE_DEFAULT;
        else {
            configurations[i]->ip_type = mm_bearer_ip_family_to_mbim_context_ip_type (ip_family, &error);
            if (error) {
                configurations[i]->ip_type = MBIM_CONTEXT_IP_TYPE_DEFAULT;
                mm_obj_warn (self, "unexpected IP type settings requested: %s", error->message);
                g_clear_error (&error);
            }
        }

        g_clear_pointer (&(configurations[i]->access_string), g_free);
        configurations[i]->access_string = g_strdup (mm_bearer_properties_get_apn (config));

        g_clear_pointer (&(configurations[i]->user_name), g_free);
        configurations[i]->user_name = g_strdup (mm_bearer_properties_get_user (config));

        g_clear_pointer (&(configurations[i]->password), g_free);
        configurations[i]->password = g_strdup (mm_bearer_properties_get_password (config));

        auth = mm_bearer_properties_get_allowed_auth (config);
        if ((auth != MM_BEARER_ALLOWED_AUTH_UNKNOWN) || configurations[i]->user_name || configurations[i]->password) {
            configurations[i]->auth_protocol = mm_bearer_allowed_auth_to_mbim_auth_protocol (auth, self, &error);
            if (error) {
                configurations[i]->auth_protocol = MBIM_AUTH_PROTOCOL_NONE;
                mm_obj_warn (self, "unexpected auth settings requested: %s", error->message);
                g_clear_error (&error);
            }
        } else {
            configurations[i]->auth_protocol = MBIM_AUTH_PROTOCOL_NONE;
        }

        configurations[i]->source = MBIM_CONTEXT_SOURCE_USER;
        configurations[i]->compression = MBIM_COMPRESSION_NONE;
        break;
    }

    /* Code customization start b/224986971 */
    for (i = 0; i < n_configurations; i++) {
        if (configurations[i]->roaming == MBIM_LTE_ATTACH_CONTEXT_ROAMING_CONTROL_HOME)
            ptr_home = configurations[i];
        else if (configurations[i]->roaming == MBIM_LTE_ATTACH_CONTEXT_ROAMING_CONTROL_NON_PARTNER)
            ptr_non_partner = configurations[i];
        else
            ptr_partner = configurations[i];
    }
    /* Sort the profiles in the order home, non_partner and partner, so we can easily remove the last one(partner). */
    if (n_configurations == 3 && ptr_home && ptr_partner && ptr_non_partner) {
        mm_obj_info (self, "removing partner profile");
        configurations[0] = ptr_home;
        configurations[1] = ptr_non_partner;
        configurations[2] = ptr_partner;
        n_configurations = 2;
    }
    /* Code customization end b/224986971 */

    request = mbim_message_ms_basic_connect_extensions_lte_attach_configuration_set_new (
                  MBIM_LTE_ATTACH_CONTEXT_OPERATION_DEFAULT,
                  n_configurations,
                  (const MbimLteAttachConfiguration *const *)configurations,
                  &error);
    if (!request) {
        g_task_return_error (task, error);
        g_object_unref (task);
        goto out;
    }
    mbim_device_command (device,
                         request,
                         10,
                         NULL,
                         (GAsyncReadyCallback)parent_att_hack_set_lte_attach_configuration_set_ready,
                         task);
    mbim_message_unref (request);

 out:
    if (configurations)
        mbim_lte_attach_configuration_array_free (configurations);

    if (response)
        mbim_message_unref (response);
}

/* This function is functionally identical to set_initial_eps_bearer_settings in mm-broadband-modem-mbim.c */
static void
parent_att_hack_set_initial_eps_bearer_settings (MMIfaceModem3gpp    *_self,
                                                 MMBearerProperties  *config,
                                                 GAsyncReadyCallback  callback,
                                                 gpointer             user_data)
{
    MMSharedFibocom *self = MM_SHARED_FIBOCOM (_self);
    GTask                *task;
    MbimDevice           *device;
    MbimMessage          *message;

    if (!peek_device (MM_BROADBAND_MODEM_MBIM (self), &device, callback, user_data))
        return;

    task = g_task_new (self, NULL, callback, user_data);

    if (!mm_broadband_modem_mbim_get_is_lte_attach_info_supported(MM_BROADBAND_MODEM_MBIM (self))) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                                 "LTE attach configuration is unsupported");
        g_object_unref (task);
        return;
    }

    g_task_set_task_data (task, g_object_ref (config), g_object_unref);

    message = mbim_message_ms_basic_connect_extensions_lte_attach_configuration_query_new (NULL);
    mbim_device_command (device,
                         message,
                         10,
                         NULL,
                         (GAsyncReadyCallback)parent_att_hack_before_set_lte_attach_configuration_query_ready,
                         task);
    mbim_message_unref (message);
}

static void
parent_set_initial_eps_bearer_settings_ready (MMIfaceModem3gpp *self,
                                              GAsyncResult     *res,
                                              GTask            *task)
{
    SetInitialEpsBearerSettingsContext *ctx;
    Private                            *priv;
    GError                             *error = NULL;

    ctx = g_task_get_task_data (task);
    priv = get_private (MM_SHARED_FIBOCOM (self));

    if (!priv->iface_modem_3gpp_parent->set_initial_eps_bearer_settings_finish (self, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (ctx->initial_eps_off_on) {
        mm_obj_dbg (self, "toggle modem power up after attach APN");
        mm_iface_modem_set_power_state (MM_IFACE_MODEM (self),
                                        MM_MODEM_POWER_STATE_ON,
                                        (GAsyncReadyCallback) after_attach_apn_modem_power_up_ready,
                                        task);
        return;
    }

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
parent_set_initial_eps_bearer_settings (GTask *task)
{
    MMSharedFibocom                    *self;
    SetInitialEpsBearerSettingsContext *ctx;
    Private                            *priv;
    g_autoptr(MMBaseSim)                modem_sim = NULL;
    gchar                              *apn;
    gchar                              *operator_identifier = NULL;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);
    priv = get_private (self);

    g_assert (priv->iface_modem_3gpp_parent);
    g_assert (priv->iface_modem_3gpp_parent->set_initial_eps_bearer_settings);
    g_assert (priv->iface_modem_3gpp_parent->set_initial_eps_bearer_settings_finish);

    g_object_get (self,
                  MM_IFACE_MODEM_SIM, &modem_sim,
                  NULL);
    if (modem_sim)
        operator_identifier = mm_gdbus_sim_get_operator_identifier(modem_sim);
    apn = mm_bearer_properties_get_apn (ctx->config);
    mm_obj_info (self, "operator_identifier: '%s' apn='%s'", operator_identifier, apn);
    if (mm_base_modem_get_vendor_id (MM_BASE_MODEM (self)) == 0x2cb7 &&
        mm_base_modem_get_product_id (MM_BASE_MODEM (self)) == 0x0007 &&
        operator_identifier && g_strcmp0(operator_identifier, "310280") == 0) {
        mm_obj_info (self, "executing custom attach logic for AT&T 310280");
        parent_att_hack_set_initial_eps_bearer_settings (MM_IFACE_MODEM_3GPP (self),
                                                         ctx->config,
                                                         (GAsyncReadyCallback)parent_set_initial_eps_bearer_settings_ready,
                                                         task);
        return;
    }

    priv->iface_modem_3gpp_parent->set_initial_eps_bearer_settings (MM_IFACE_MODEM_3GPP (self),
                                                                    ctx->config,
                                                                    (GAsyncReadyCallback)parent_set_initial_eps_bearer_settings_ready,
                                                                    task);
}

static void
before_attach_apn_modem_power_down_ready (MMIfaceModem *self,
                                          GAsyncResult *res,
                                          GTask        *task)
{
    GError *error = NULL;

    if (!mm_iface_modem_set_power_state_finish (self, res, &error)) {
        mm_obj_warn (self, "failed to power down modem before attach APN settings update: %s", error->message);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_obj_dbg (self, "success toggling modem power down before attach APN");

    parent_set_initial_eps_bearer_settings (task);
}

void
mm_shared_fibocom_set_initial_eps_bearer_settings (MMIfaceModem3gpp    *self,
                                                   MMBearerProperties  *config,
                                                   GAsyncReadyCallback  callback,
                                                   gpointer             user_data)
{
    SetInitialEpsBearerSettingsContext *ctx;
    GTask                              *task;
    MMPortMbim                         *port;

    task = g_task_new (self, NULL, callback, user_data);

    /* This shared logic is only expected in MBIM capable devices */
    g_assert (MM_IS_BROADBAND_MODEM_MBIM (self));
    port = mm_broadband_modem_mbim_peek_port_mbim (MM_BROADBAND_MODEM_MBIM (self));
    if (!port) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "No valid MBIM port found");
        g_object_unref (task);
        return;
    }

    ctx = g_slice_new0 (SetInitialEpsBearerSettingsContext);
    ctx->config = g_object_ref (config);
    ctx->initial_eps_off_on = mm_kernel_device_get_property_as_boolean (mm_port_peek_kernel_device (MM_PORT (port)), "ID_MM_FIBOCOM_INITIAL_EPS_OFF_ON");
    g_task_set_task_data (task, ctx, (GDestroyNotify)set_initial_eps_bearer_settings_context_free);

    if (ctx->initial_eps_off_on) {
        mm_obj_dbg (self, "toggle modem power down before attach APN");
        mm_iface_modem_set_power_state (MM_IFACE_MODEM (self),
                                        MM_MODEM_POWER_STATE_LOW,
                                        (GAsyncReadyCallback) before_attach_apn_modem_power_down_ready,
                                        task);
        return;
    }

    parent_set_initial_eps_bearer_settings (task);
}

/*****************************************************************************/

static void
shared_fibocom_init (gpointer g_iface)
{
}

GType
mm_shared_fibocom_get_type (void)
{
    static GType shared_fibocom_type = 0;

    if (!G_UNLIKELY (shared_fibocom_type)) {
        static const GTypeInfo info = {
            sizeof (MMSharedFibocom),  /* class_size */
            shared_fibocom_init,       /* base_init */
            NULL,                      /* base_finalize */
        };

        shared_fibocom_type = g_type_register_static (G_TYPE_INTERFACE, "MMSharedFibocom", &info, 0);
        g_type_interface_add_prerequisite (shared_fibocom_type, MM_TYPE_IFACE_MODEM);
        g_type_interface_add_prerequisite (shared_fibocom_type, MM_TYPE_IFACE_MODEM_3GPP);
    }

    return shared_fibocom_type;
}
