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

#ifndef MM_INTEL_MAIN_H
#define MM_INTEL_MAIN_H

#include "mm-broadband-modem.h"
#include "mm-broadband-modem-mbim.h"
#include "mm-iface-modem-location.h"

#define MM_TYPE_BROADBAND_MODEM_MBIM_INTEL            (mm_broadband_modem_mbim_intel_get_type ())
#define MM_BROADBAND_MODEM_MBIM_INTEL(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_BROADBAND_MODEM_MBIM_INTEL, MMBroadbandModemMbimIntel))
#define MM_BROADBAND_MODEM_MBIM_INTEL_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  MM_TYPE_BROADBAND_MODEM_MBIM_INTEL, MMBroadbandModemMbimIntelClass))
#define MM_IS_BROADBAND_MODEM_MBIM_INTEL(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_BROADBAND_MODEM_MBIM_INTEL))
#define MM_IS_BROADBAND_MODEM_MBIM_INTEL_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  MM_TYPE_BROADBAND_MODEM_MBIM_INTEL))
#define MM_BROADBAND_MODEM_MBIM_INTEL_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  MM_TYPE_BROADBAND_MODEM_MBIM_INTEL, MMBroadbandModemMbimIntelClass))

#define MM_TYPE_SHARED_INTEL               (mm_shared_intel_get_type ())
#define MM_SHARED_INTEL(obj)               (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_SHARED_INTEL, MMSharedIntel))
#define MM_IS_SHARED_INTEL(obj)            (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_SHARED_INTEL))
#define MM_SHARED_INTEL_GET_INTERFACE(obj) (G_TYPE_INSTANCE_GET_INTERFACE ((obj), MM_TYPE_SHARED_INTEL, MMSharedIntel))

typedef struct _MMBroadbandModemMbimIntel MMBroadbandModemMbimIntel;
typedef struct _MMBroadbandModemMbimIntelClass MMBroadbandModemMbimIntelClass;

typedef struct _MMSharedIntel MMSharedIntel;

struct _MMSharedIntel {
    GTypeInterface g_iface;

    /* Peek broadband modem class of the parent class of the object */
    MMBroadbandModemClass * (* peek_parent_broadband_modem_class) (MMSharedIntel *self);

    /* Peek location interface of the parent class of the object */
    MMIfaceModemLocation *  (* peek_parent_location_interface)    (MMSharedIntel *self);
};

struct _MMBroadbandModemMbimIntel {
    MMBroadbandModemMbim parent;
};

struct _MMBroadbandModemMbimIntelClass {
    MMBroadbandModemMbimClass parent;
};

GType mm_shared_intel_get_type(void);

GType mm_broadband_modem_mbim_intel_get_type (void);

MMBroadbandModemMbimIntel *mm_broadband_modem_mbim_intel_new (const gchar  *device,
                                  const gchar  **drivers,
                                  const gchar  *plugin,
                                  guint16      vendor_id,
                                  guint16      product_id);

#endif /* MM_INTEL_MAIN_H */
