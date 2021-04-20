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
 * Copyright (C) 2021 Aleksander Morgado <aleksander@aleksander.es>
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

#include "mm-errors-types.h"
#include "mm-modem-helpers.h"
#include "mm-log-object.h"
#include "mm-iface-modem.h"
#include "mm-broadband-modem-foxconn-t99w175.h"

#if defined WITH_QMI
# include "mm-iface-modem-firmware.h"
# include "mm-shared-qmi.h"
#endif

#if defined WITH_QMI
static void iface_modem_firmware_init (MMIfaceModemFirmware *iface);
#endif

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemFoxconnT99w175, mm_broadband_modem_foxconn_t99w175, MM_TYPE_BROADBAND_MODEM_MBIM, 0
#if defined WITH_QMI
                        , G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_FIRMWARE, iface_modem_firmware_init)
#endif
                        )

typedef enum {
    FEATURE_SUPPORT_UNKNOWN,
    FEATURE_NOT_SUPPORTED,
    FEATURE_SUPPORTED
} FeatureSupport;

struct _MMBroadbandModemFoxconnT99w175Private {
    FeatureSupport unmanaged_gps_support;
};

/*****************************************************************************/
/* Firmware update settings
 *
 * We only support reporting firmware update settings when QMI support is built,
 * because this is the only clean way to get the expected firmware version to
 * report.
 */

#if defined WITH_QMI

static MMFirmwareUpdateSettings *
firmware_load_update_settings_finish (MMIfaceModemFirmware  *self,
                                      GAsyncResult          *res,
                                      GError               **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
foxconn_get_firmware_version_ready (QmiClientDms *client,
                                    GAsyncResult *res,
                                    GTask        *task)
{
    QmiMessageDmsFoxconnGetFirmwareVersionOutput *output;
    GError                                       *error = NULL;
    MMFirmwareUpdateSettings                     *update_settings = NULL;
    const gchar                                  *str;

    output = qmi_client_dms_foxconn_get_firmware_version_finish (client, res, &error);
    if (!output || !qmi_message_dms_foxconn_get_firmware_version_output_get_result (output, &error))
        goto out;

    /* Create update settings now */
    update_settings = mm_firmware_update_settings_new (MM_MODEM_FIRMWARE_UPDATE_METHOD_MBIM_QDU);

    qmi_message_dms_foxconn_get_firmware_version_output_get_version (output, &str, NULL);
    mm_firmware_update_settings_set_version (update_settings, str);

 out:
    if (error)
        g_task_return_error (task, error);
    else {
        g_assert (update_settings);
        g_task_return_pointer (task, update_settings, g_object_unref);
    }
    g_object_unref (task);
    if (output)
        qmi_message_dms_foxconn_get_firmware_version_output_unref (output);
}

static void
firmware_load_update_settings (MMIfaceModemFirmware *self,
                               GAsyncReadyCallback   callback,
                               gpointer              user_data)
{
    GTask                                       *task;
    QmiMessageDmsFoxconnGetFirmwareVersionInput *input = NULL;
    QmiClient                                   *client = NULL;

    task = g_task_new (self, NULL, callback, user_data);

    client = mm_shared_qmi_peek_client (MM_SHARED_QMI (self),
                                        QMI_SERVICE_DMS,
                                        MM_PORT_QMI_FLAG_DEFAULT,
                                        NULL);
    if (!client) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "Unable to load T99w175 version info: no QMI DMS client available");
        g_object_unref (task);
        return;
    }

    input = qmi_message_dms_foxconn_get_firmware_version_input_new ();
    qmi_message_dms_foxconn_get_firmware_version_input_set_version_type (
        input,
        QMI_DMS_FOXCONN_FIRMWARE_VERSION_TYPE_FIRMWARE_MCFG_APPS,
        NULL);
    qmi_client_dms_foxconn_get_firmware_version (
        QMI_CLIENT_DMS (client),
        input,
        10,
        NULL,
        (GAsyncReadyCallback)foxconn_get_firmware_version_ready,
        task);
    qmi_message_dms_foxconn_get_firmware_version_input_unref (input);
}

#endif

/*****************************************************************************/

MMBroadbandModemFoxconnT99w175 *
mm_broadband_modem_foxconn_t99w175_new (const gchar  *device,
                                        const gchar **drivers,
                                        const gchar  *plugin,
                                        guint16       vendor_id,
                                        guint16       product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_FOXCONN_T99W175,
                         MM_BASE_MODEM_DEVICE,     device,
                         MM_BASE_MODEM_DRIVERS,    drivers,
                         MM_BASE_MODEM_PLUGIN,     plugin,
                         MM_BASE_MODEM_VENDOR_ID,  vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         /* MBIM bearer supports NET only */
                         MM_BASE_MODEM_DATA_NET_SUPPORTED, TRUE,
                         MM_BASE_MODEM_DATA_TTY_SUPPORTED, FALSE,
                         MM_IFACE_MODEM_CARRIER_CONFIG_MAPPING,              PKGDATADIR "/mm-foxconn-t99w175-carrier-mapping.conf",
                         NULL);
}

static void
mm_broadband_modem_foxconn_t99w175_init (MMBroadbandModemFoxconnT99w175 *self)
{
    /* Initialize private data */
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, MM_TYPE_BROADBAND_MODEM_FOXCONN_T99W175, MMBroadbandModemFoxconnT99w175Private);
}

#if defined WITH_QMI

static void
iface_modem_firmware_init (MMIfaceModemFirmware *iface)
{
    iface->load_update_settings = firmware_load_update_settings;
    iface->load_update_settings_finish = firmware_load_update_settings_finish;
}

#endif

static void
mm_broadband_modem_foxconn_t99w175_class_init (MMBroadbandModemFoxconnT99w175Class *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMBroadbandModemFoxconnT99w175Private));
}