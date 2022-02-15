/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * libmm -- Access modem status & information from glib applications
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2021 Fibocom Wireless Inc.
 */

#ifndef _MM_MODEM_RF_H_
#define _MM_MODEM_RF_H_

#if !defined (__LIBMM_GLIB_H_INSIDE__) && !defined (LIBMM_GLIB_COMPILATION)
#error "Only <libmm-glib.h> can be included directly."
#endif

#include <ModemManager.h>

#include "mm-gdbus-modem.h"

G_BEGIN_DECLS

#define MM_TYPE_MODEM_RF            (mm_modem_rf_get_type ())
#define MM_MODEM_RF(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_MODEM_RF, MMMomdeMRf))
#define MM_MODEM_RF_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), MM_TYPE_MODEM_RF, MMMomdeMRfClass))
#define MM_IS_MODEM_RF(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_MODEM_RF))
#define MM_IS_MODEM_RF_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((obj), MM_TYPE_MODEM_RF))
#define MM_MODEM_RF_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), MM_TYPE_MODEM_RF, MMMomdeMRfClass))

typedef struct _MMModemRf MMModemRf;
typedef struct _MMModemRfClass MMModemRfClass;

/**
 * MMModemRf:
 *
 * The #MMModemRf structure contains private data and should only be accessed
 * using the provided API.
 */
struct _MMModemRf {
    /*< private >*/
    MmGdbusModemRfProxy parent;
    gpointer unused;
};

struct _MMModemRfClass {
    /*< private >*/
    MmGdbusModemRfProxyClass parent;
};

/**
 * MMModemRfInfo:
 *
 * The #MMModemRfInfo structure contains private data and should only be accessed
 * using the provided API.
 */
typedef struct _MMModemRfInfo MMModemRfInfo;

MMRfCellType mm_modem_rf_get_serving_cell_info (const MMModemRfInfo *info);

guint64 mm_modem_rf_get_center_frequency (const MMModemRfInfo *info);

guint32 mm_modem_rf_get_bandwidth (const MMModemRfInfo *info);

guint32 mm_modem_rf_get_rsrp (const MMModemRfInfo *info);

guint32 mm_modem_rf_get_rsrq (const MMModemRfInfo *info);

guint32 mm_modem_rf_get_sinr (const MMModemRfInfo *info);

guint32 mm_modem_rf_get_rssi (const MMModemRfInfo *info);

guint32 mm_modem_rf_get_connection_status (const MMModemRfInfo *info);

void    mm_modem_rf_rf_info_free (MMModemRfInfo *network);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (MMModemRfInfo, mm_modem_rf_rf_info_free)

GType   mm_modem_rf_get_type (void);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (MMModemRf, g_object_unref)

const gchar *mm_modem_rf_get_path (MMModemRf *self);
gchar       *mm_modem_rf_dup_path (MMModemRf *self);

void         mm_modem_rf_get_rf_info      (MMModemRf          *self,
                                           GCancellable       *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer            user_data);

gboolean mm_modem_rf_get_rf_info_sync     (MMModemRf    *self,
                                           GCancellable *cancellable,
                                           GError      **error);

gboolean mm_modem_rf_get_rf_info_finish   (MMModemRf    *self,
                                           GAsyncResult *res,
                                           GError      **error);

void     mm_modem_rf_setup_rf_info        (MMModemRf          *self,
                                           gboolean            enable,
                                           GCancellable       *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer            user_data);

gboolean mm_modem_rf_setup_rf_info_sync   (MMModemRf    *self,
                                           gboolean      enable,
                                           GCancellable *cancellable,
                                           GError      **error);

gboolean mm_modem_rf_setup_rf_info_finish (MMModemRf    *self,
                                           GAsyncResult *res,
                                           GError      **error);

GList* mm_modem_rf_get_rf_inf             (MMModemRf *self);


G_END_DECLS

#endif /* _MM_MODEM_RF_H_ */

