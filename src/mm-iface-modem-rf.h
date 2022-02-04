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
 * Copyright (C) 2021 Fibocom Wireless Inc.
 */

#ifndef MM_IFACE_MODEM_RF_H
#define MM_IFACE_MODEM_RF_H

#include <glib-object.h>
#include <gio/gio.h>

#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#define MM_TYPE_IFACE_MODEM_RF               (mm_iface_modem_rf_get_type ())
#define MM_IFACE_MODEM_RF(obj)               (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_IFACE_MODEM_RF, MMIfaceModemRf))
#define MM_IS_IFACE_MODEM_RF(obj)            (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_IFACE_MODEM_RF))
#define MM_IFACE_MODEM_RF_GET_INTERFACE(obj) (G_TYPE_INSTANCE_GET_INTERFACE ((obj), MM_TYPE_IFACE_MODEM_RF, MMIfaceModemRf))

#define MM_IFACE_MODEM_RF_DBUS_SKELETON  "iface-modem-rf-dbus-skeleton"

typedef struct _MMIfaceModemRf MMIfaceModemRf;

struct _MMIfaceModemRf {
    GTypeInterface g_iface;

    /* Check for RF support (async) */
    void (* check_support)             (MMIfaceModemRf      *self,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);
    gboolean (* check_support_finish)  (MMIfaceModemRf  *self,
                                        GAsyncResult     *res,
                                        GError          **error);

    /* Get RF info  (async) */
    void (* get_rf_info)           (MMIfaceModemRf      *self,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data);
    GList* (* get_rf_info_finish)  (MMIfaceModemRf *self,
                                    GAsyncResult    *res,
                                    GError         **error);

    /* Setup RF info  (async) */
    void (* setup_rf_info)             (MMIfaceModemRf      *self,
                                        gboolean             enable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);
    gboolean (* setup_rf_info_finish)  (MMIfaceModemRf  *self,
                                        GAsyncResult    *res,
                                        GError          **error);
};

GType mm_iface_modem_rf_get_type (void);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (MMIfaceModemRf, g_object_unref)

/* Initialize Rf interface (async) */
void     mm_iface_modem_rf_initialize         (MMIfaceModemRf      *self,
                                               GCancellable        *cancellable,
                                               GAsyncReadyCallback  callback,
                                               gpointer             user_data);
gboolean mm_iface_modem_rf_initialize_finish  (MMIfaceModemRf  *self,
                                               GAsyncResult    *res,
                                               GError         **error);

/* Shutdown Rf interface */
void     mm_iface_modem_rf_shutdown           (MMIfaceModemRf *self);

/* Bind properties for simple GetStatus() */
void     mm_iface_modem_rf_bind_simple_status (MMIfaceModemRf*self,
                                               MMSimpleStatus *status);

void     mm_iface_modem_rf_update_rf_info (MMIfaceModemRf *self,GList *info_list);

#endif /* MM_IFACE_MODEM_RF_H */

