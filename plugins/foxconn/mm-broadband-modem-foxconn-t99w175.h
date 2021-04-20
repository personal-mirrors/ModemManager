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

#ifndef MM_BROADBAND_MODEM_FOXCONN_T99W175_H
#define MM_BROADBAND_MODEM_FOXCONN_T99W175_H

#include "mm-broadband-modem-mbim.h"

#define MM_TYPE_BROADBAND_MODEM_FOXCONN_T99W175            (mm_broadband_modem_foxconn_t99w175_get_type ())
#define MM_BROADBAND_MODEM_FOXCONN_T99W175(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_BROADBAND_MODEM_FOXCONN_T99W175, MMBroadbandModemFoxconnT99w175))
#define MM_BROADBAND_MODEM_FOXCONN_T99W175_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  MM_TYPE_BROADBAND_MODEM_FOXCONN_T99W175, MMBroadbandModemFoxconnT99w175Class))
#define MM_IS_BROADBAND_MODEM_FOXCONN_T99W175(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_BROADBAND_MODEM_FOXCONN_T99W175))
#define MM_IS_BROADBAND_MODEM_FOXCONN_T99W175_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  MM_TYPE_BROADBAND_MODEM_FOXCONN_T99W175))
#define MM_BROADBAND_MODEM_FOXCONN_T99W175_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  MM_TYPE_BROADBAND_MODEM_FOXCONN_T99W175, MMBroadbandModemFoxconnT99w175Class))

typedef struct _MMBroadbandModemFoxconnT99w175 MMBroadbandModemFoxconnT99w175;
typedef struct _MMBroadbandModemFoxconnT99w175Class MMBroadbandModemFoxconnT99w175Class;
typedef struct _MMBroadbandModemFoxconnT99w175Private MMBroadbandModemFoxconnT99w175Private;

struct _MMBroadbandModemFoxconnT99w175 {
    MMBroadbandModemMbim parent;
    MMBroadbandModemFoxconnT99w175Private *priv;
};

struct _MMBroadbandModemFoxconnT99w175Class{
    MMBroadbandModemMbimClass parent;
};

GType mm_broadband_modem_foxconn_t99w175_get_type (void);

MMBroadbandModemFoxconnT99w175 *mm_broadband_modem_foxconn_t99w175_new (const gchar  *device,
									const gchar **driver,
									const gchar  *plugin,
									guint16       vendor_id,
									guint16       product_id);

#endif /* MM_BROADBAND_MODEM_FOXCONN_T99W175_H */
