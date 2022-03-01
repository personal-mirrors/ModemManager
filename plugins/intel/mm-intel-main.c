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
 * Copyright (C) 2021 Intel Corporation
 */

#include <config.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include "ModemManager.h"
#include "mm-base-modem.h"
#include "mm-base-modem-at.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-location.h"
#include "mm-intel-main.h"
#include "mm-intel-location-core.h"
#define MM_LOG_NO_OBJECT
#include "mm-log.h"


static void intel_init  (MMSharedIntel  *iface);
static void iface_modem_location_init (MMIfaceModemLocation *iface);

static MMIfaceModemLocation *iface_modem_location_parent;

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemMbimIntel, mm_broadband_modem_mbim_intel, MM_TYPE_BROADBAND_MODEM_MBIM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_LOCATION, iface_modem_location_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_SHARED_INTEL,  intel_init))


static void
mm_intel_setup_ports (MMBroadbandModem *self)
{
    MMPortSerialAt              *gnss_at_port;

    gnss_at_port = mm_base_modem_peek_port_primary   (MM_BASE_MODEM (self));
    if (gnss_at_port == NULL) {
        mm_err ("Intel primary AT port is not available");
        return;
    }
    g_object_set (G_OBJECT (gnss_at_port),
                  MM_PORT_SERIAL_SEND_DELAY, (guint64) 0,
                  MM_PORT_SERIAL_AT_SEND_LF, TRUE,
                  MM_PORT_SERIAL_SPEW_CONTROL, TRUE,
                  MM_PORT_SERIAL_AT_REMOVE_ECHO, FALSE,
                  NULL);

    il_context_init(self);
}

GType
mm_shared_intel_get_type (void)
{
    static GType intel_type = 0;

    if (!G_UNLIKELY (intel_type)) {
        static const GTypeInfo info = {
            sizeof (MMSharedIntel),  /* class_size */
            NULL,              /* base_init */
            NULL,              /* base_finalize */
        };

        intel_type = g_type_register_static (G_TYPE_INTERFACE, "MMSharedIntel", &info, 0);
        g_type_interface_add_prerequisite (intel_type, MM_TYPE_IFACE_MODEM);
    }

    return intel_type;
}

MMBroadbandModemMbimIntel *
mm_broadband_modem_mbim_intel_new (const gchar  *device,
                    const gchar  **drivers,
                    const gchar  *plugin,
                    guint16      vendor_id,
                    guint16      product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_MBIM_INTEL,
                         MM_BASE_MODEM_DEVICE,     device,
                         MM_BASE_MODEM_DRIVERS,    drivers,
                         MM_BASE_MODEM_PLUGIN,     plugin,
                         MM_BASE_MODEM_VENDOR_ID,  vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         MM_BASE_MODEM_DATA_NET_SUPPORTED, TRUE,
                         NULL);
}

static void
mm_broadband_modem_mbim_intel_init (MMBroadbandModemMbimIntel *self)
{
    /*nothing to be done here, but required for creating intel based modem instance*/
}

static void
iface_modem_location_init (MMIfaceModemLocation *iface)
{
    iface_modem_location_parent              = g_type_interface_peek_parent (iface);
    iface->load_capabilities                 = il_load_capabilities;
    iface->load_capabilities_finish          = il_load_capabilities_finish;
    iface->enable_location_gathering         = il_enable_location_gathering;
    iface->enable_location_gathering_finish  = il_enable_location_gathering_finish;
    iface->disable_location_gathering        = il_disable_location_gathering;
    iface->disable_location_gathering_finish = il_disable_location_gathering_finish;
}

static MMBroadbandModemClass *
peek_parent_broadband_modem_class (MMSharedIntel *self)
{
    return MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_mbim_intel_parent_class);
}

static MMIfaceModemLocation *
peek_parent_location_interface (MMSharedIntel *self)
{
    return iface_modem_location_parent;
}

static void
intel_init (MMSharedIntel *iface)
{
    mm_dbg ("Initializing intel plugin");
    iface->peek_parent_broadband_modem_class  = peek_parent_broadband_modem_class;
    iface->peek_parent_location_interface     = peek_parent_location_interface;
}

static void
mm_broadband_modem_mbim_intel_class_init (MMBroadbandModemMbimIntelClass *klass)
{
    MMBroadbandModemClass *broadband_modem_class = MM_BROADBAND_MODEM_CLASS (klass);
    broadband_modem_class->setup_ports           = mm_intel_setup_ports;
}
