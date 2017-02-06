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
 * Copyright (C) 2008 - 2009 Novell, Inc.
 * Copyright (C) 2009 - 2012 Red Hat, Inc.
 * Copyright (C) 2012 Aleksander Morgado <aleksander@gnu.org>
 */

#include <string.h>
#include <gmodule.h>

#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-log.h"
#include "mm-plugin-zte.h"
#include "mm-broadband-modem-zte.h"
#include "mm-broadband-modem-zte-icera.h"

#if defined WITH_QMI
#include "mm-broadband-modem-qmi.h"
#endif

#if defined WITH_MBIM
#include "mm-broadband-modem-mbim.h"
#endif

G_DEFINE_TYPE (MMPluginZte, mm_plugin_zte, MM_TYPE_PLUGIN)

int mm_plugin_major_version = MM_PLUGIN_MAJOR_VERSION;
int mm_plugin_minor_version = MM_PLUGIN_MINOR_VERSION;

/*****************************************************************************/
/* Custom commands for AT probing */

/* Many ZTE devices will flood the port with "Message waiting" indications
 * and eventually fill up the serial buffer and crash.  We need to turn off
 * that indicator.  See NetworkManager commits
 * 	1235f71b20c92cded4abd976ccc5010649aae1a0 and
 * 	f38ad328acfdc6ce29dd1380602c546b064161ae for more details.
 *
 * We use this command also for checking AT support in the port.
 */
static const MMPortProbeAtCommand custom_at_probe[] = {
    { "ATE0+CPMS?", 3, mm_port_probe_response_processor_is_at },
    { "ATE0+CPMS?", 3, mm_port_probe_response_processor_is_at },
    { "ATE0+CPMS?", 3, mm_port_probe_response_processor_is_at },
    { NULL }
};

/*****************************************************************************/

static MMBaseModem *
create_modem (MMPlugin *self,
              const gchar *sysfs_path,
              const gchar **drivers,
              guint16 vendor,
              guint16 product,
              GList *probes,
              GError **error)
{
#if defined WITH_QMI
    if (mm_port_probe_list_has_qmi_port (probes)) {
        mm_dbg ("QMI-powered ZTE modem found...");
        return MM_BASE_MODEM (mm_broadband_modem_qmi_new (sysfs_path,
                                                          drivers,
                                                          mm_plugin_get_name (self),
                                                          vendor,
                                                          product));
    }
#endif

#if defined WITH_MBIM
    if (mm_port_probe_list_has_mbim_port (probes)) {
        mm_dbg ("MBIM-powered ZTE modem found...");
        return MM_BASE_MODEM (mm_broadband_modem_mbim_new (sysfs_path,
                                                           drivers,
                                                           mm_plugin_get_name (self),
                                                           vendor,
                                                           product));
    }
#endif

    if (mm_port_probe_list_is_icera (probes))
        return MM_BASE_MODEM (mm_broadband_modem_zte_icera_new (sysfs_path,
                                                                drivers,
                                                                mm_plugin_get_name (self),
                                                                vendor,
                                                                product));

    return MM_BASE_MODEM (mm_broadband_modem_zte_new (sysfs_path,
                                                      drivers,
                                                      mm_plugin_get_name (self),
                                                      vendor,
                                                      product));
}

static gboolean
grab_port (MMPlugin *self,
           MMBaseModem *modem,
           MMPortProbe *probe,
           GError **error)
{
    GUdevDevice *port;
    MMPortSerialAtFlag pflags = MM_PORT_SERIAL_AT_FLAG_NONE;
    MMPortType ptype;

    port = mm_port_probe_peek_port (probe);

    /* Ignore net ports on non-Icera non-QMI modems */
    ptype = mm_port_probe_get_port_type (probe);
    if (ptype == MM_PORT_TYPE_NET && MM_IS_BROADBAND_MODEM_ZTE (modem)) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_UNSUPPORTED,
                     "Ignoring net port in ZTE modem");
        return FALSE;
    }

    if (mm_port_probe_is_at (probe)) {
        /* Look for port type hints */
        if (g_udev_device_get_property_as_boolean (port, "ID_MM_ZTE_PORT_TYPE_MODEM")) {
            mm_dbg ("ZTE: AT port '%s/%s' flagged as primary",
                    mm_port_probe_get_port_subsys (probe),
                    mm_port_probe_get_port_name (probe));
            pflags = MM_PORT_SERIAL_AT_FLAG_PRIMARY;
        } else if (g_udev_device_get_property_as_boolean (port, "ID_MM_ZTE_PORT_TYPE_AUX")) {
            mm_dbg ("ZTE: AT port '%s/%s' flagged as secondary",
                    mm_port_probe_get_port_subsys (probe),
                    mm_port_probe_get_port_name (probe));
            pflags = MM_PORT_SERIAL_AT_FLAG_SECONDARY;
        }
    }

    if (g_udev_device_get_property_as_boolean (port, "ID_MM_ZTE_ICERA_DHCP")) {
        mm_dbg ("ZTE: Icera-based modem will use DHCP");
        g_object_set (modem,
                      MM_BROADBAND_MODEM_ICERA_DEFAULT_IP_METHOD, MM_BEARER_IP_METHOD_DHCP,
                      NULL);
    }

    return mm_base_modem_grab_port (modem,
                                    mm_port_probe_get_port_subsys (probe),
                                    mm_port_probe_get_port_name (probe),
                                    mm_port_probe_get_parent_path (probe),
                                    ptype,
                                    pflags,
                                    error);
}

/*****************************************************************************/

G_MODULE_EXPORT MMPlugin *
mm_plugin_create (void)
{
    static const gchar *subsystems[] = { "tty", "net", "usb", NULL };
    static const guint16 vendor_ids[] = { 0x19d2, 0 };

    return MM_PLUGIN (
        g_object_new (MM_TYPE_PLUGIN_ZTE,
                      MM_PLUGIN_NAME,               "ZTE",
                      MM_PLUGIN_ALLOWED_SUBSYSTEMS, subsystems,
                      MM_PLUGIN_ALLOWED_VENDOR_IDS, vendor_ids,
                      MM_PLUGIN_CUSTOM_AT_PROBE,    custom_at_probe,
                      MM_PLUGIN_ALLOWED_AT,         TRUE,
                      MM_PLUGIN_ALLOWED_QCDM,       TRUE,
                      MM_PLUGIN_ALLOWED_QMI,        TRUE,
                      MM_PLUGIN_ALLOWED_MBIM,       TRUE,
                      MM_PLUGIN_ICERA_PROBE,        TRUE,
                      NULL));
}

static void
mm_plugin_zte_init (MMPluginZte *self)
{
}

static void
mm_plugin_zte_class_init (MMPluginZteClass *klass)
{
    MMPluginClass *plugin_class = MM_PLUGIN_CLASS (klass);

    plugin_class->create_modem = create_modem;
    plugin_class->grab_port = grab_port;
}
