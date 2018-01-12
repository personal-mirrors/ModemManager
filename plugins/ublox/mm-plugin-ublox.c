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
 * Copyright (C) 2016 Aleksander Morgado <aleksander@aleksander.es>
 */

#include <string.h>
#include <gmodule.h>

#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-log.h"
#include "mm-serial-parsers.h"
#include "mm-broadband-modem-ublox.h"
#include "mm-plugin-ublox.h"

G_DEFINE_TYPE (MMPluginUblox, mm_plugin_ublox, MM_TYPE_PLUGIN)

MM_PLUGIN_DEFINE_MAJOR_VERSION
MM_PLUGIN_DEFINE_MINOR_VERSION

/*****************************************************************************/

static MMBaseModem *
create_modem (MMPlugin     *self,
              const gchar  *sysfs_path,
              const gchar **drivers,
              guint16       vendor,
              guint16       product,
              GList        *probes,
              GError      **error)
{
    return MM_BASE_MODEM (mm_broadband_modem_ublox_new (sysfs_path,
                                                        drivers,
                                                        mm_plugin_get_name (self),
                                                        vendor,
                                                        product));
}

static gboolean
grab_port (MMPlugin     *self,
           MMBaseModem  *modem,
           MMPortProbe  *probe,
           GError      **error)
{
    MMPortSerialAtFlag pflags = MM_PORT_SERIAL_AT_FLAG_NONE;
    MMKernelDevice *port;
    MMPortType port_type;

    port_type = mm_port_probe_get_port_type (probe);
    port = mm_port_probe_peek_port (probe);

    if (mm_kernel_device_get_property_as_boolean (port, "ID_MM_UBLOX_PRIMARY_PORT")) {
        mm_dbg ("(%s/%s)' port flagged as primary",
                mm_port_probe_get_port_subsys (probe),
                mm_port_probe_get_port_name (probe));
        pflags = MM_PORT_SERIAL_AT_FLAG_PRIMARY;
    } else if (mm_kernel_device_get_property_as_boolean (port, "ID_MM_UBLOX_SECONDARY_PORT")) {
        mm_dbg ("(%s/%s) port flagged as secondary",
                mm_port_probe_get_port_subsys (probe),
                mm_port_probe_get_port_name (probe));
        pflags = MM_PORT_SERIAL_AT_FLAG_SECONDARY;
    }

    return mm_base_modem_grab_port (modem, port, port_type, pflags, error);
}

/*****************************************************************************/
/* Custom init context */

/* Wait up to 20s for the +READY URC */
#define READY_WAIT_TIME_SECS 20

typedef struct {
    MMPortSerialAt *port;
    GRegex         *ready_regex;
    guint           timeout_id;
} CustomInitContext;

static void
custom_init_context_free (CustomInitContext *ctx)
{
    g_assert (!ctx->timeout_id);
    g_regex_unref (ctx->ready_regex);
    g_object_unref (ctx->port);
    g_slice_free (CustomInitContext, ctx);
}

static gboolean
ublox_custom_init_finish (MMPortProbe   *probe,
                          GAsyncResult  *result,
                          GError       **error)
{
    return g_task_propagate_boolean (G_TASK (result), error);
}

static gboolean
ready_timeout (GTask *task)
{
    CustomInitContext *ctx;
    MMPortProbe       *probe;

    ctx   = g_task_get_task_data     (task);
    probe = g_task_get_source_object (task);

    ctx->timeout_id = 0;

    mm_port_serial_at_add_unsolicited_msg_handler (ctx->port, ctx->ready_regex,
                                                   NULL, NULL, NULL);

    mm_dbg ("(%s/%s) timed out waiting for READY unsolicited message",
            mm_port_probe_get_port_subsys (probe),
            mm_port_probe_get_port_name   (probe));

    /* not an error really, we didn't probe anything yet, that's all */
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);

    return G_SOURCE_REMOVE;
}

static void
ready_received (MMPortSerialAt   *port,
                GMatchInfo       *info,
                GTask            *task)
{
    CustomInitContext *ctx;
    MMPortProbe       *probe;

    ctx   = g_task_get_task_data     (task);
    probe = g_task_get_source_object (task);

    g_source_remove (ctx->timeout_id);
    ctx->timeout_id = 0;

    mm_dbg ("(%s/%s) READY received: port is AT",
            mm_port_probe_get_port_subsys (probe),
            mm_port_probe_get_port_name   (probe));

    /* Flag as an AT port right away */
    mm_port_probe_set_result_at (probe, TRUE);

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
wait_for_ready (GTask *task)
{
    CustomInitContext *ctx;
    MMPortProbe       *probe;

    ctx   = g_task_get_task_data     (task);
    probe = g_task_get_source_object (task);

    mm_dbg ("(%s/%s) waiting for READY unsolicited message...",
            mm_port_probe_get_port_subsys (probe),
            mm_port_probe_get_port_name   (probe));

    /* Configure a regex on the TTY, so that we stop the custom init
     * as soon as +READY URC is received */
    mm_port_serial_at_add_unsolicited_msg_handler (ctx->port,
                                                   ctx->ready_regex,
                                                   (MMPortSerialAtUnsolicitedMsgFn) ready_received,
                                                   task,
                                                   NULL);

    /* Otherwise, let the custom init timeout in some seconds. */
    ctx->timeout_id = g_timeout_add_seconds (READY_WAIT_TIME_SECS, (GSourceFunc) ready_timeout, task);
}

static void
quick_at_ready (MMPortSerialAt *port,
                GAsyncResult   *res,
                GTask          *task)
{
    CustomInitContext *ctx;
    MMPortProbe       *probe;
    const gchar       *response;
    GError            *error = NULL;

    ctx   = g_task_get_task_data     (task);
    probe = g_task_get_source_object (task);

    response = mm_port_serial_at_command_finish (port, res, &error);
    if (error) {
        /* On a timeout error, wait for READY URC */
        if (g_error_matches (error, MM_SERIAL_ERROR, MM_SERIAL_ERROR_RESPONSE_TIMEOUT)) {
            wait_for_ready (task);
            goto out;
        }
        /* On an unknown error, make it fatal */
        if (!mm_serial_parser_v1_is_known_error (error)) {
            mm_warn ("(%s/%s) custom port initialization logic failed: %s",
                    mm_port_probe_get_port_subsys (probe),
                    mm_port_probe_get_port_name   (probe),
                    error->message);
            goto out_complete;
        }
    }

    mm_dbg ("(%s/%s) port is AT",
            mm_port_probe_get_port_subsys (probe),
            mm_port_probe_get_port_name   (probe));
    mm_port_probe_set_result_at (probe, TRUE);

out_complete:
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
out:
    g_clear_error (&error);
}

static void
ublox_custom_init (MMPortProbe         *probe,
                   MMPortSerialAt      *port,
                   GCancellable        *cancellable,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
    GTask             *task;
    CustomInitContext *ctx;

    task = g_task_new (probe, cancellable, callback, user_data);

    ctx = g_slice_new0 (CustomInitContext);
    ctx->port = g_object_ref (port);
    ctx->ready_regex = g_regex_new ("\\r\\n\\+AT:\\s*READY\\r\\n",
                                    G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    g_task_set_task_data (task, ctx, (GDestroyNotify) custom_init_context_free);

    /* If the device hasn't been plugged in right away, we assume it was already
     * running for some time. We validate the assumption with a quick AT probe,
     * and if it times out, we run the explicit READY wait from scratch (e.g.
     * to cope with the case where MM starts after the TTY has been exposed but
     * where the device was also just reseted) */
    if (!mm_device_get_hotplugged (mm_port_probe_peek_device (probe))) {
        mm_port_serial_at_command (ctx->port,
                                   "AT",
                                   1,
                                   FALSE, /* raw */
                                   FALSE, /* allow_cached */
                                   g_task_get_cancellable (task),
                                   (GAsyncReadyCallback)quick_at_ready,
                                   task);
        return;
    }

    /* Device hotplugged, wait for READY URC */
    wait_for_ready (task);
}

/*****************************************************************************/

G_MODULE_EXPORT MMPlugin *
mm_plugin_create (void)
{
    static const gchar *subsystems[] = { "tty", "net", NULL };
    static const guint16 vendor_ids[] = { 0x1546, 0 };
    static const gchar *vendor_strings[] = { "u-blox", NULL };
    static const MMAsyncMethod custom_init = {
        .async  = G_CALLBACK (ublox_custom_init),
        .finish = G_CALLBACK (ublox_custom_init_finish),
    };

    return MM_PLUGIN (g_object_new (MM_TYPE_PLUGIN_UBLOX,
                                    MM_PLUGIN_NAME,                   "u-blox",
                                    MM_PLUGIN_ALLOWED_SUBSYSTEMS,     subsystems,
                                    MM_PLUGIN_ALLOWED_VENDOR_IDS,     vendor_ids,
                                    MM_PLUGIN_ALLOWED_VENDOR_STRINGS, vendor_strings,
                                    MM_PLUGIN_ALLOWED_AT,             TRUE,
                                    MM_PLUGIN_SEND_DELAY,             (guint64) 0,
                                    MM_PLUGIN_CUSTOM_INIT,            &custom_init,
                                    NULL));
}

static void
mm_plugin_ublox_init (MMPluginUblox *self)
{
}

static void
mm_plugin_ublox_class_init (MMPluginUbloxClass *klass)
{
    MMPluginClass *plugin_class = MM_PLUGIN_CLASS (klass);

    plugin_class->create_modem = create_modem;
    plugin_class->grab_port    = grab_port;
}
