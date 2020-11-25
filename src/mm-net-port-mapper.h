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
 * Copyright 2020 Google LLC
 */

#ifndef MM_NET_PORT_MAPPER_H
#define MM_NET_PORT_MAPPER_H

#include <glib-object.h>
#include <glib.h>

#include "kerneldevice/mm-kernel-device.h"

G_BEGIN_DECLS

#define MM_TYPE_NET_PORT_MAPPER            (mm_net_port_mapper_get_type ())
#define MM_NET_PORT_MAPPER(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_NET_PORT_MAPPER, MMNetPortMapper))
#define MM_NET_PORT_MAPPER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  MM_TYPE_NET_PORT_MAPPER, MMNetPortMapperClass))
#define MM_IS_NET_PORT_MAPPER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_NET_PORT_MAPPER))
#define MM_IS_NET_PORT_MAPPER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  MM_TYPE_NET_PORT_MAPPER))
#define MM_NET_PORT_MAPPER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  MM_TYPE_NET_PORT_MAPPER, MMNetPortMapperClass))

typedef struct _MMNetPortMapper      MMNetPortMapper;
typedef struct _MMNetPortMapperClass MMNetPortMapperClass;

typedef void (*MMNetPortMapperConfigureNet) (MMKernelDevice *device,
                                             const gchar    *physdev_uid);

GType            mm_net_port_mapper_get_type (void);
MMNetPortMapper *mm_net_port_mapper_get (void);

gchar *mm_net_port_mapper_get_ctrl_iface_name (MMNetPortMapper *self,
                                               const gchar     *net_iface_name);
#if defined WITH_QMI && QMI_QRTR_SUPPORTED
guint mm_net_port_mapper_get_mux_id (MMNetPortMapper *self,
                                     const gchar     *net_iface_name);
#endif

gchar *mm_net_port_mapper_get_ctrl_iface_physdev_uid (MMNetPortMapper *self,
                                                      const gchar     *net_iface_name);

void mm_net_port_mapper_configure_net_interface (MMNetPortMapper *self,
                                                 MMKernelDevice  *net_device);

void mm_net_port_mapper_register_port (MMNetPortMapper *self,
                                       const gchar     *ctl_iface_name,
                                       const gchar     *ctl_iface_subsystem,
                                       const gchar     *ctl_iface_physdev_uid,
                                       const gchar     *net_iface_name,
                                       guint            net_iface_mux_id,
                                       MMNetPortMapperConfigureNet configure);

void mm_net_port_mapper_unregister_port (MMNetPortMapper *self,
                                         const gchar     *ctl_iface_subsystem,
                                         const gchar     *ctl_iface_name);
G_END_DECLS

#endif /* MM_NET_PORT_MAPPER_H */
