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
 * Copyright (C) 2020 Aleksander Morgado <aleksander@aleksander.es>
 * Copyright (C) 2021 Ivan Mikhanchuk <ivan.mikhanchuk@quectel.com>
 */

#include <config.h>

#include "mm-base-modem-at.h"
#include "mm-port-mbim.h"
#include "mm-broadband-modem-mbim.h"
#include "mm-log-object.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-firmware.h"
#include "mm-iface-modem-time.h"
#include "mm-shared-quectel.h"
#include "mm-modem-helpers-quectel.h"
#include "mm-broadband-modem-mbim-quectel.h"

static void iface_modem_firmware_init (MMIfaceModemFirmware *iface);
static void iface_modem_time_init     (MMIfaceModemTime     *iface);
static void shared_quectel_init       (MMSharedQuectel      *iface);

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemMbimQuectel, mm_broadband_modem_mbim_quectel, MM_TYPE_BROADBAND_MODEM_MBIM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_FIRMWARE, iface_modem_firmware_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_TIME, iface_modem_time_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_SHARED_QUECTEL, shared_quectel_init))

/*****************************************************************************/
/* Firmware update settings */

static MMFirmwareUpdateSettings *
firmware_load_update_settings_finish (MMIfaceModemFirmware  *self,
                                      GAsyncResult          *res,
                                      GError               **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
quectel_at_port_get_firmware_version_ready (MMBaseModem  *modem,
                                    GAsyncResult *res,
                                    GTask        *task)
{
    MMFirmwareUpdateSettings *update_settings;
    const gchar              *version;

    update_settings = mm_firmware_update_settings_new (MM_MODEM_FIRMWARE_UPDATE_METHOD_FIREHOSE);

    version = mm_base_modem_at_command_finish (modem, res, NULL);
    if (version)
        mm_firmware_update_settings_set_version (update_settings, version);
    g_task_return_pointer (task, update_settings, g_object_unref);
    g_object_unref (task);
}

static void
quectel_mbim_port_get_firmware_version_ready (MbimDevice   *device,
                                              GAsyncResult *res,
                                              GTask        *task)
{
    GError                   *error = NULL;
    MMFirmwareUpdateSettings *update_settings;
    g_autoptr(MbimMessage)    response = NULL;
    const guint8             *response_buffer;
    g_autofree gchar         *version = NULL;
    guint32                   sz;

    response = mbim_device_command_finish (device, res, &error);
    if (response &&
        mbim_message_response_get_result (response, MBIM_MESSAGE_TYPE_COMMAND_DONE, &error) &&
        (response_buffer = mbim_message_command_done_get_raw_information_buffer (response, &sz))) {
        version = mm_quectel_get_firmware_version (response_buffer, sz);
    }

    update_settings = mm_firmware_update_settings_new (MM_MODEM_FIRMWARE_UPDATE_METHOD_FIREHOSE);

    if (version && update_settings)
        mm_firmware_update_settings_set_version (update_settings, version);

    g_task_return_pointer (task, update_settings, g_object_unref);
    g_object_unref (task);
}


static void
firmware_load_update_settings (MMIfaceModemFirmware *self,
                               GAsyncReadyCallback   callback,
                               gpointer              user_data)
{
    GTask *task;
    g_autoptr(GError) error = NULL;
    MMPortMbim *mbim;
    MMPortSerialAt *at_port;

    task = g_task_new (self, NULL, callback, user_data);

    at_port = mm_base_modem_peek_best_at_port ( MM_BASE_MODEM (self), &error);
    if (at_port != NULL) {
        mm_base_modem_at_command_full (MM_BASE_MODEM (self),
                                at_port,
                                "+QGMR?",
                                3,
                                FALSE,
                                FALSE,
                                FALSE,
                                (GAsyncReadyCallback) quectel_at_port_get_firmware_version_ready,
                                task);
        return;
    }

    mbim = mm_broadband_modem_mbim_peek_port_mbim (MM_BROADBAND_MODEM_MBIM (self));
    if (mbim != NULL) {
        guint8 buffer[] = { 0x00, 0x01 };
        g_autoptr(MbimMessage)  message = NULL;

        message = mbim_message_command_new (0, MBIM_SERVICE_QDU, 7, MBIM_MESSAGE_COMMAND_TYPE_SET);
        mbim_message_command_append (message, buffer, sizeof (buffer));

        mbim_device_command (mm_port_mbim_peek_device (mbim),
                            message,
                            5,
                            NULL,
                            (GAsyncReadyCallback) quectel_mbim_port_get_firmware_version_ready,
                            task);
        return;
    }

    g_task_report_new_error (self,
                             callback,
                             user_data,
                             firmware_load_update_settings,
                             MM_CORE_ERROR,
                             MM_CORE_ERROR_FAILED,
                             "Couldn't find a port to fetch firmware info");
    return;
}

/*****************************************************************************/

MMBroadbandModemMbimQuectel *
mm_broadband_modem_mbim_quectel_new (const gchar  *device,
                                     const gchar **drivers,
                                     const gchar  *plugin,
                                     guint16       vendor_id,
                                     guint16       product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_MBIM_QUECTEL,
                         MM_BASE_MODEM_DEVICE,     device,
                         MM_BASE_MODEM_DRIVERS,    drivers,
                         MM_BASE_MODEM_PLUGIN,     plugin,
                         MM_BASE_MODEM_VENDOR_ID,  vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         /* include carrier information */
                         MM_IFACE_MODEM_FIRMWARE_IGNORE_CARRIER, FALSE,
                         /* MBIM bearer supports NET only */
                         MM_BASE_MODEM_DATA_NET_SUPPORTED, TRUE,
                         MM_BASE_MODEM_DATA_TTY_SUPPORTED, FALSE,
                         NULL);
}

static void
mm_broadband_modem_mbim_quectel_init (MMBroadbandModemMbimQuectel *self)
{
}

static void
iface_modem_firmware_init (MMIfaceModemFirmware *iface)
{
    iface->load_update_settings        = firmware_load_update_settings;
    iface->load_update_settings_finish = firmware_load_update_settings_finish;
}

static void
iface_modem_time_init (MMIfaceModemTime *iface)
{
    iface->check_support        = mm_shared_quectel_time_check_support;
    iface->check_support_finish = mm_shared_quectel_time_check_support_finish;
}

static void
shared_quectel_init (MMSharedQuectel *iface)
{
}

static void
mm_broadband_modem_mbim_quectel_class_init (MMBroadbandModemMbimQuectelClass *klass)
{
}
