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
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include <libmm-glib.h>

#include "ModemManager.h"
#include "mm-broadband-modem-qmi-qcom-soc.h"
#include "mm-log.h"
#include "mm-net-port-mapper.h"

G_DEFINE_TYPE (MMBroadbandModemQmiQcomSoc, mm_broadband_modem_qmi_qcom_soc, MM_TYPE_BROADBAND_MODEM_QMI)

/*****************************************************************************/

static const QmiSioPort sio_port_per_port_number[] = {
    QMI_SIO_PORT_A2_MUX_RMNET0,
    QMI_SIO_PORT_A2_MUX_RMNET1,
    QMI_SIO_PORT_A2_MUX_RMNET2,
    QMI_SIO_PORT_A2_MUX_RMNET3,
    QMI_SIO_PORT_A2_MUX_RMNET4,
    QMI_SIO_PORT_A2_MUX_RMNET5,
    QMI_SIO_PORT_A2_MUX_RMNET6,
    QMI_SIO_PORT_A2_MUX_RMNET7
};

static MMPortQmi *
peek_port_qmi_for_data_in_bam_dmux (MMBroadbandModemQmi  *self,
                                    MMPort               *data,
                                    QmiSioPort           *out_sio_port,
                                    GError              **error)
{
    GList          *rpmsg_qmi_ports;
    MMPortQmi      *found = NULL;
    MMKernelDevice *net_port;
    gint            net_port_number;

    net_port = mm_port_peek_kernel_device (data);

    /* The dev_port notified by the bam-dmux driver indicates which SIO port we should be using */
    net_port_number = mm_kernel_device_get_attribute_as_int (net_port, "dev_port");
    if (net_port_number < 0 || net_port_number >= (gint) G_N_ELEMENTS (sio_port_per_port_number)) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_NOT_FOUND,
                     "Couldn't find SIO port number for 'net/%s'",
                     mm_port_get_device (data));
        return NULL;
    }

    /* Find one QMI port, we don't care which one */
    rpmsg_qmi_ports = mm_base_modem_find_ports (MM_BASE_MODEM (self),
                                                MM_PORT_SUBSYS_RPMSG,
                                                MM_PORT_TYPE_QMI);
    if (!rpmsg_qmi_ports) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_NOT_FOUND,
                     "Couldn't find any QMI port for 'net/%s'",
                     mm_port_get_device (data));
        return NULL;
    }

    /* Set outputs */
    if (out_sio_port)
        *out_sio_port = sio_port_per_port_number[net_port_number];
    found = MM_PORT_QMI (rpmsg_qmi_ports->data);

    g_list_free_full (rpmsg_qmi_ports, g_object_unref);

    return found;
}

#if defined WITH_QMI && QMI_QRTR_SUPPORTED

static MMPortQmi *
peek_port_qmi_for_data_in_ipa (MMBroadbandModemQmi  *self,
                               MMPort               *data,
                               guint                *out_mux_id,
                               GError              **error)
{
    GList           *qrtr_qmi_ports = NULL;
    MMPortQmi       *found = NULL;
    MMKernelDevice  *net_port;
    MMNetPortMapper *net_port_mapper;
    const gchar     *net_port_name;
    const gchar     *parent_port_name;

    net_port      = mm_port_peek_kernel_device (data);
    net_port_name = mm_kernel_device_get_name (net_port);

    /* Find the QMI port that was used to create the net port */
    net_port_mapper = mm_net_port_mapper_get ();
    parent_port_name = mm_net_port_mapper_get_ctrl_iface_name (net_port_mapper, net_port_name);
    if (parent_port_name) {
        qrtr_qmi_ports = mm_base_modem_find_ports (MM_BASE_MODEM (self),
                                                   MM_PORT_SUBSYS_QRTR,
                                                   MM_PORT_TYPE_QMI,
                                                   parent_port_name);
    }

    if (!qrtr_qmi_ports) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_NOT_FOUND,
                     "Couldn't find any QMI port for 'net/%s'",
                     mm_port_get_device (data));
        return NULL;
    }

    *out_mux_id = mm_net_port_mapper_get_mux_id (net_port_mapper, net_port_name);
    found = MM_PORT_QMI (qrtr_qmi_ports->data);

    g_list_free_full (qrtr_qmi_ports, g_object_unref);

    return found;
}
#endif

static MMPortQmi *
peek_port_qmi_for_data (MMBroadbandModemQmi  *self,
                        MMPort               *data,
                        QmiSioPort           *out_sio_port,
                        guint                *out_mux_id,
                        GError              **error)
{
    MMKernelDevice *net_port;
    const gchar    *net_port_driver;

    g_assert (MM_IS_BROADBAND_MODEM_QMI (self));
    g_assert (mm_port_get_subsys (data) == MM_PORT_SUBSYS_NET);

    net_port        = mm_port_peek_kernel_device (data);
    net_port_driver = mm_kernel_device_get_driver (net_port);

    if (g_strcmp0 (net_port_driver, "bam-dmux") == 0) {
#if defined WITH_QMI && QMI_QRTR_SUPPORTED
        *out_mux_id = QMI_DEVICE_MUX_ID_UNBOUND;
#else
        *out_mux_id = 0;
#endif
        return peek_port_qmi_for_data_in_bam_dmux (self, data, out_sio_port, error);
    }

#if defined WITH_QMI && QMI_QRTR_SUPPORTED
    if (g_strcmp0 (net_port_driver, "ipa") == 0) {
        *out_sio_port = QMI_SIO_PORT_NONE;
        return peek_port_qmi_for_data_in_ipa (self, data, out_mux_id, error);
    }
#endif

    g_set_error (error,
                 MM_CORE_ERROR,
                 MM_CORE_ERROR_FAILED,
                 "Unsupported QMI kernel driver for 'net/%s': %s",
                 mm_port_get_device (data),
                 net_port_driver);
    return NULL;
}

/*****************************************************************************/

MMBroadbandModemQmiQcomSoc *
mm_broadband_modem_qmi_qcom_soc_new (const gchar  *device,
                                     const gchar **drivers,
                                     const gchar  *plugin,
                                     guint16       vendor_id,
                                     guint16       product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_QMI_QCOM_SOC,
                         MM_BASE_MODEM_DEVICE,     device,
                         MM_BASE_MODEM_DRIVERS,    drivers,
                         MM_BASE_MODEM_PLUGIN,     plugin,
                         MM_BASE_MODEM_VENDOR_ID,  vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         /* QMI bearer supports NET only */
                         MM_BASE_MODEM_DATA_NET_SUPPORTED, TRUE,
                         MM_BASE_MODEM_DATA_TTY_SUPPORTED, FALSE,
                         NULL);
}

static void
mm_broadband_modem_qmi_qcom_soc_init (MMBroadbandModemQmiQcomSoc *self)
{
}

static void
mm_broadband_modem_qmi_qcom_soc_class_init (MMBroadbandModemQmiQcomSocClass *klass)
{
    MMBroadbandModemQmiClass *broadband_modem_qmi_class = MM_BROADBAND_MODEM_QMI_CLASS (klass);

    broadband_modem_qmi_class->peek_port_qmi_for_data = peek_port_qmi_for_data;
}
