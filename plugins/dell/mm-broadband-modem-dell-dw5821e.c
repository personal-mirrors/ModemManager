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
#include <time.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-log.h"
#include "mm-errors-types.h"
#include "mm-modem-helpers.h"
#include "mm-base-modem-at.h"
#include "mm-iface-modem-location.h"
#include "mm-broadband-modem-dell-dw5821e.h"

static void iface_modem_location_init (MMIfaceModemLocation *iface);

static MMIfaceModemLocation *iface_modem_location_parent;

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemDellDw5821e, mm_broadband_modem_dell_dw5821e, MM_TYPE_BROADBAND_MODEM_MBIM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_LOCATION, iface_modem_location_init))

typedef enum {
    FEATURE_SUPPORT_UNKNOWN,
    FEATURE_NOT_SUPPORTED,
    FEATURE_SUPPORTED
} FeatureSupport;

struct _MMBroadbandModemDellDw5821ePrivate {
    FeatureSupport unmanaged_gps_support;
};

/*****************************************************************************/
/* Location capabilities loading (Location interface) */

static MMModemLocationSource
location_load_capabilities_finish (MMIfaceModemLocation  *self,
                                   GAsyncResult          *res,
                                   GError              **error)
{
    GError *inner_error = NULL;
    gssize value;

    value = g_task_propagate_int (G_TASK (res), &inner_error);
    if (inner_error) {
        g_propagate_error (error, inner_error);
        return MM_MODEM_LOCATION_SOURCE_NONE;
    }
    return (MMModemLocationSource)value;
}

static void
parent_load_capabilities_ready (MMIfaceModemLocation *self,
                                GAsyncResult         *res,
                                GTask                *task)
{
    MMModemLocationSource  sources;
    GError                *error = NULL;

    sources = iface_modem_location_parent->load_capabilities_finish (self, res, &error);
    if (error) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* If we have a GPS port and an AT port, enable unmanaged GPS support */
    if (mm_base_modem_peek_port_primary (MM_BASE_MODEM (self)) &&
        mm_base_modem_peek_port_gps (MM_BASE_MODEM (self))) {
        MM_BROADBAND_MODEM_DELL_DW5821E (self)->priv->unmanaged_gps_support = FEATURE_SUPPORTED;
        sources |= MM_MODEM_LOCATION_SOURCE_GPS_UNMANAGED;
    }

    /* So we're done, complete */
    g_task_return_int (task, sources);
    g_object_unref (task);
}

static void
location_load_capabilities (MMIfaceModemLocation *self,
                            GAsyncReadyCallback   callback,
                            gpointer              user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* Chain up parent's setup */
    iface_modem_location_parent->load_capabilities (self,
                                                    (GAsyncReadyCallback)parent_load_capabilities_ready,
                                                    task);
}

/*****************************************************************************/
/* Disable location gathering (Location interface) */

static gboolean
disable_location_gathering_finish (MMIfaceModemLocation  *self,
                                   GAsyncResult          *res,
                                   GError               **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
parent_disable_location_gathering_ready (MMIfaceModemLocation *self,
                                         GAsyncResult         *res,
                                         GTask                *task)
{
    GError *error = NULL;

    if (!iface_modem_location_parent->disable_location_gathering_finish (self, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
parent_disable_location_gathering (GTask *task)
{
    MMIfaceModemLocation  *self;
    MMModemLocationSource  source;

    self = MM_IFACE_MODEM_LOCATION (g_task_get_source_object (task));
    source = GPOINTER_TO_UINT (g_task_get_task_data (task));

    iface_modem_location_parent->disable_location_gathering (self,
                                                            source,
                                                            (GAsyncReadyCallback)parent_disable_location_gathering_ready,
                                                            task);
}

static void
unmanaged_gps_disabled_ready (MMBaseModem  *self,
                              GAsyncResult *res,
                              GTask        *task)
{
    GError *error = NULL;

    if (!mm_base_modem_at_command_finish (self, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    parent_disable_location_gathering (task);
}

static void
disable_location_gathering (MMIfaceModemLocation  *_self,
                            MMModemLocationSource  source,
                            GAsyncReadyCallback    callback,
                            gpointer               user_data)
{
    MMBroadbandModemDellDw5821e *self = MM_BROADBAND_MODEM_DELL_DW5821E (_self);
    GTask                       *task;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, GUINT_TO_POINTER (source), NULL);

    /* We only support Unmanaged GPS at this level */
    if ((self->priv->unmanaged_gps_support != FEATURE_SUPPORTED) ||
        (source != MM_MODEM_LOCATION_SOURCE_GPS_UNMANAGED)) {
        parent_disable_location_gathering (task);
        return;
    }

    mm_base_modem_at_command (MM_BASE_MODEM (_self),
                              "^NV=30007,01,\"00\"",
                              3,
                              FALSE,
                              (GAsyncReadyCallback)unmanaged_gps_disabled_ready,
                              task);
}

/*****************************************************************************/
/* Enable location gathering (Location interface) */

static gboolean
enable_location_gathering_finish (MMIfaceModemLocation  *self,
                                  GAsyncResult          *res,
                                  GError               **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
unmanaged_gps_enabled_ready (MMBaseModem  *self,
                             GAsyncResult *res,
                             GTask        *task)
{
    GError *error = NULL;

    if (!mm_base_modem_at_command_finish (self, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
parent_enable_location_gathering_ready (MMIfaceModemLocation *_self,
                                        GAsyncResult         *res,
                                        GTask                *task)
{
    MMBroadbandModemDellDw5821e *self = MM_BROADBAND_MODEM_DELL_DW5821E (_self);
    GError                      *error = NULL;
    MMModemLocationSource        source;

    if (!iface_modem_location_parent->enable_location_gathering_finish (_self, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* We only support Unmanaged GPS at this level */
    source = GPOINTER_TO_UINT (g_task_get_task_data (task));
    if ((self->priv->unmanaged_gps_support != FEATURE_SUPPORTED) ||
        (source != MM_MODEM_LOCATION_SOURCE_GPS_UNMANAGED)) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    mm_base_modem_at_command (MM_BASE_MODEM (_self),
                              "^NV=30007,01,\"01\"",
                              3,
                              FALSE,
                              (GAsyncReadyCallback)unmanaged_gps_enabled_ready,
                              task);
}

static void
enable_location_gathering (MMIfaceModemLocation  *self,
                           MMModemLocationSource  source,
                           GAsyncReadyCallback    callback,
                           gpointer               user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, GUINT_TO_POINTER (source), NULL);

    /* Chain up parent's gathering enable */
    iface_modem_location_parent->enable_location_gathering (self,
                                                            source,
                                                            (GAsyncReadyCallback)parent_enable_location_gathering_ready,
                                                            task);
}

/*****************************************************************************/

MMBroadbandModemDellDw5821e *
mm_broadband_modem_dell_dw5821e_new (const gchar  *device,
                                     const gchar **drivers,
                                     const gchar  *plugin,
                                     guint16       vendor_id,
                                     guint16       product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_DELL_DW5821E,
                         MM_BASE_MODEM_DEVICE,     device,
                         MM_BASE_MODEM_DRIVERS,    drivers,
                         MM_BASE_MODEM_PLUGIN,     plugin,
                         MM_BASE_MODEM_VENDOR_ID,  vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         NULL);
}

static void
mm_broadband_modem_dell_dw5821e_init (MMBroadbandModemDellDw5821e *self)
{
    /* Initialize private data */
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, MM_TYPE_BROADBAND_MODEM_DELL_DW5821E, MMBroadbandModemDellDw5821ePrivate);
    self->priv->unmanaged_gps_support = FEATURE_SUPPORT_UNKNOWN;
}

static void
iface_modem_location_init (MMIfaceModemLocation *iface)
{
    iface_modem_location_parent = g_type_interface_peek_parent (iface);

    iface->load_capabilities = location_load_capabilities;
    iface->load_capabilities_finish = location_load_capabilities_finish;
    iface->enable_location_gathering = enable_location_gathering;
    iface->enable_location_gathering_finish = enable_location_gathering_finish;
    iface->disable_location_gathering = disable_location_gathering;
    iface->disable_location_gathering_finish = disable_location_gathering_finish;
}

static void
mm_broadband_modem_dell_dw5821e_class_init (MMBroadbandModemDellDw5821eClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMBroadbandModemDellDw5821ePrivate));
}
