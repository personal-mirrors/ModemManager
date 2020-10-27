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
 * Copyright (C) 2018 Aleksander Morgado <aleksander@aleksander.es>
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "ModemManager.h"
#include "mm-log.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-location.h"
#include "mm-broadband-modem-xmm.h"
#include "mm-shared-xmm.h"
#include "mm-broadband-bearer-xmm-lte.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-base-modem-at.h"


static void iface_modem_init (MMIfaceModem *iface);
static void shared_xmm_init  (MMSharedXmm  *iface);
static void iface_modem_signal_init (MMIfaceModemSignal *iface);
static void iface_modem_location_init (MMIfaceModemLocation *iface);
static void iface_modem_3gpp_init (MMIfaceModem3gpp *iface);

static MMIfaceModemLocation *iface_modem_location_parent;

static MMIfaceModem3gpp *iface_modem_3gpp_parent;

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemXmm, mm_broadband_modem_xmm, MM_TYPE_BROADBAND_MODEM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_3GPP, iface_modem_3gpp_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_SIGNAL, iface_modem_signal_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_LOCATION, iface_modem_location_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_SHARED_XMM,  shared_xmm_init))

/*****************************************************************************/
/*****************************************************************************/
/* Register in network (3GPP interface) */

static gboolean
register_in_3gpp_network_finish (MMIfaceModem3gpp  *self,
                            GAsyncResult      *res,
                            GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
parent_registration_ready (MMIfaceModem3gpp *self,
                           GAsyncResult     *res,
                           GTask            *task)
{
    GError *error = NULL;

    if (!iface_modem_3gpp_parent->register_in_network_finish (self, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
run_parent_registration (GTask *task)
{
    MMBroadbandModemXmm*self;
    const gchar             *operator_id;

    self = g_task_get_source_object (task);
    operator_id = g_task_get_task_data (task);

    iface_modem_3gpp_parent->register_in_network (
        MM_IFACE_MODEM_3GPP (self),
        operator_id,
        g_task_get_cancellable (task),
        (GAsyncReadyCallback)parent_registration_ready,
        task);
}

static void
xdns_ready (MMBaseModem  *self,
            GAsyncResult *res,
            GTask        *task)
{
    GError      *error = NULL;

    if( !mm_base_modem_at_command_finish (self, res, &error))
    {
         g_task_return_error (task, error);
         g_object_unref (task);
         return;
    }

    run_parent_registration (task);
    return;
}

static void
register_in_3gpp_network (MMIfaceModem3gpp    *self,
                     const gchar         *operator_id,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, cancellable, callback, user_data);

    /* Store operator id as task data */
    g_task_set_task_data (task, g_strdup (operator_id), g_free);

    /* Before go to auto register, we should set the init PDP dynamic DNS. */
    if (operator_id == NULL || operator_id[0] == '\0') {
        /* Check which is the current operator selection status */
        mm_base_modem_at_command (MM_BASE_MODEM (self),
                                  "+XDNS=0,1",
                                  3,
                                  FALSE,
                                  (GAsyncReadyCallback)xdns_ready,
                                  task);
        return;
    }

    /* Otherwise, run parent's implementation right away */
    run_parent_registration (task);
}

/*****************************************************************************/
/* Create Bearer (Modem interface) */

static MMBaseBearer *
modem_create_bearer_finish (MMIfaceModem *self,
                            GAsyncResult *res,
                            GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
broadband_bearer_new_ready (GObject *source,
                            GAsyncResult *res,
                            GTask *task)
{
    MMBaseBearer *bearer = NULL;
    GError *error = NULL;

    bearer = mm_broadband_bearer_xmm_lte_new_finish (res, &error);
    if (!bearer)
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, bearer, g_object_unref);
    g_object_unref (task);
}

static void
modem_create_bearer (MMIfaceModem *self,
                     MMBearerProperties *properties,
                     GAsyncReadyCallback callback,
                     gpointer user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);
    /* We  create a xmm MMBroadbandBearer */
    mm_broadband_bearer_xmm_lte_new (MM_BROADBAND_MODEM_XMM(self),
                                        properties,
                                        NULL, /* cancellable */
                                        (GAsyncReadyCallback)broadband_bearer_new_ready,
                                        task);
}


/*****************************************************************************/

MMBroadbandModemXmm *
mm_broadband_modem_xmm_new (const gchar  *device,
                            const gchar **drivers,
                            const gchar  *plugin,
                            guint16       vendor_id,
                            guint16       product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_XMM,
                         MM_BASE_MODEM_DEVICE,     device,
                         MM_BASE_MODEM_DRIVERS,    drivers,
                         MM_BASE_MODEM_PLUGIN,     plugin,
                         MM_BASE_MODEM_VENDOR_ID,  vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         NULL);
}

static void
mm_broadband_modem_xmm_init (MMBroadbandModemXmm *self)
{
}

static void
iface_modem_init (MMIfaceModem *iface)
{
    iface->load_supported_modes        = mm_shared_xmm_load_supported_modes;
    iface->load_supported_modes_finish = mm_shared_xmm_load_supported_modes_finish;
    iface->load_current_modes          = mm_shared_xmm_load_current_modes;
    iface->load_current_modes_finish   = mm_shared_xmm_load_current_modes_finish;
    iface->set_current_modes           = mm_shared_xmm_set_current_modes;
    iface->set_current_modes_finish    = mm_shared_xmm_set_current_modes_finish;

    iface->load_supported_bands        = mm_shared_xmm_load_supported_bands;
    iface->load_supported_bands_finish = mm_shared_xmm_load_supported_bands_finish;
    iface->load_current_bands          = mm_shared_xmm_load_current_bands;
    iface->load_current_bands_finish   = mm_shared_xmm_load_current_bands_finish;
    iface->set_current_bands           = mm_shared_xmm_set_current_bands;
    iface->set_current_bands_finish    = mm_shared_xmm_set_current_bands_finish;

    iface->load_power_state        = mm_shared_xmm_load_power_state;
    iface->load_power_state_finish = mm_shared_xmm_load_power_state_finish;
    iface->modem_power_up          = mm_shared_xmm_power_up;
    iface->modem_power_up_finish   = mm_shared_xmm_power_up_finish;
    iface->modem_power_down        = mm_shared_xmm_power_down;
    iface->modem_power_down_finish = mm_shared_xmm_power_down_finish;
    iface->modem_power_off         = mm_shared_xmm_power_off;
    iface->modem_power_off_finish  = mm_shared_xmm_power_off_finish;
    iface->reset                   = mm_shared_xmm_reset;
    iface->reset_finish            = mm_shared_xmm_reset_finish;

    iface->create_bearer = modem_create_bearer;
    iface->create_bearer_finish = modem_create_bearer_finish;

}


static void
iface_modem_location_init (MMIfaceModemLocation *iface)
{
    iface_modem_location_parent = g_type_interface_peek_parent (iface);

    iface->load_capabilities                 = mm_shared_xmm_location_load_capabilities;
    iface->load_capabilities_finish          = mm_shared_xmm_location_load_capabilities_finish;
    iface->enable_location_gathering         = mm_shared_xmm_enable_location_gathering;
    iface->enable_location_gathering_finish  = mm_shared_xmm_enable_location_gathering_finish;
    iface->disable_location_gathering        = mm_shared_xmm_disable_location_gathering;
    iface->disable_location_gathering_finish = mm_shared_xmm_disable_location_gathering_finish;
    iface->load_supl_server                  = mm_shared_xmm_location_load_supl_server;
    iface->load_supl_server_finish           = mm_shared_xmm_location_load_supl_server_finish;
    iface->set_supl_server                   = mm_shared_xmm_location_set_supl_server;
    iface->set_supl_server_finish            = mm_shared_xmm_location_set_supl_server_finish;
}

static MMBroadbandModemClass *
peek_parent_broadband_modem_class (MMSharedXmm *self)
{
    return MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_xmm_parent_class);
}

static MMIfaceModemLocation *
peek_parent_location_interface (MMSharedXmm *self)
{
    return iface_modem_location_parent;
}

static void
iface_modem_signal_init (MMIfaceModemSignal *iface)
{
    iface->check_support        = mm_shared_xmm_signal_check_support;
    iface->check_support_finish = mm_shared_xmm_signal_check_support_finish;
    iface->load_values          = mm_shared_xmm_signal_load_values;
    iface->load_values_finish   = mm_shared_xmm_signal_load_values_finish;
}

static void
shared_xmm_init (MMSharedXmm *iface)
{
    iface->peek_parent_broadband_modem_class = peek_parent_broadband_modem_class;
    iface->peek_parent_location_interface    = peek_parent_location_interface;
}

static void
iface_modem_3gpp_init (MMIfaceModem3gpp *iface)
{
    iface_modem_3gpp_parent = g_type_interface_peek_parent (iface);

    iface->register_in_network = register_in_3gpp_network;
    iface->register_in_network_finish = register_in_3gpp_network_finish;
}


static void
mm_broadband_modem_xmm_class_init (MMBroadbandModemXmmClass *klass)
{
    MMBroadbandModemClass *broadband_modem_class = MM_BROADBAND_MODEM_CLASS (klass);

    broadband_modem_class->setup_ports = mm_shared_xmm_setup_ports;
}
