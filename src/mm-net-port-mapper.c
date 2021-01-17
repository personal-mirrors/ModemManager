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

#include <config.h>

#if defined WITH_QMI
# include <libqmi-glib.h>
#endif

#include <ModemManager.h>

#include "mm-utils.h"

#include "mm-net-port-mapper.h"

struct _MMNetPortMapper {
    GObject parent;
    /* The container of Net ports created by ModemManager */
    GHashTable *ports;
};

struct _MMNetPortMapperClass {
    GObjectClass parent;
};

static void log_object_iface_init (MMLogObjectInterface *iface);

G_DEFINE_TYPE_EXTENDED (MMNetPortMapper, mm_net_port_mapper, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_LOG_OBJECT, log_object_iface_init))

/*****************************************************************************/

static gchar *
log_object_build_id (MMLogObject *_self)
{
    return g_strdup ("net-port-mapper");
}

/*****************************************************************************/
typedef struct {
    gchar                      *subsystem;
    gchar                      *name;
    gchar                      *physdev_uid;
    guint                       mux_id;
    MMNetPortMapperConfigureNet configure_cb;
} CtrlPortInfo;

static void
ctrl_port_info_free (CtrlPortInfo *info)
{
    g_free (info->subsystem);
    g_free (info->name);
    g_free (info->physdev_uid);
    g_free (info);
}

/*****************************************************************************/
void
mm_net_port_mapper_register_port (MMNetPortMapper *self,
                                  const gchar     *ctl_iface_name,
                                  const gchar     *ctl_iface_subsystem,
                                  const gchar     *ctl_iface_physdev_uid,
                                  const gchar     *net_iface_name,
                                  guint            net_iface_mux_id,
                                  MMNetPortMapperConfigureNet configure_cb)
{
    CtrlPortInfo *ctrl_port_info;

    g_assert_nonnull (ctl_iface_name);
    g_assert_nonnull (ctl_iface_subsystem);
    g_assert_nonnull (ctl_iface_physdev_uid);
    g_assert_nonnull (net_iface_name);

    if (g_hash_table_lookup (self->ports, net_iface_name)) {
        mm_obj_warn (self,
                     "the net port '%s' has already been registered",
                     net_iface_name);
        return;
    }

    mm_obj_dbg (self,
                "registering control iface '%s' with net iface '%s'",
                ctl_iface_name,
                net_iface_name);

    ctrl_port_info               = g_new0 (CtrlPortInfo, 1);
    ctrl_port_info->subsystem    = g_strdup (ctl_iface_subsystem);
    ctrl_port_info->name         = g_strdup (ctl_iface_name);
    ctrl_port_info->physdev_uid  = g_strdup (ctl_iface_physdev_uid);
    ctrl_port_info->mux_id       = net_iface_mux_id;
    ctrl_port_info->configure_cb = configure_cb;

    g_hash_table_insert (self->ports, g_strdup (net_iface_name), ctrl_port_info);
}

void
mm_net_port_mapper_unregister_port (MMNetPortMapper *self,
                                    const gchar     *net_iface_name)
{
    if (!g_hash_table_remove (self->ports, net_iface_name))
        mm_obj_info (self,
                    "unable to unregister net iface '%s'",
                    net_iface_name);
}

void
mm_net_port_mapper_configure_net_interface (MMNetPortMapper *self,
                                            MMKernelDevice  *net_device)
{
    CtrlPortInfo *ctrl_port_info;

    ctrl_port_info = g_hash_table_lookup (self->ports, mm_kernel_device_get_name (net_device));
    if (ctrl_port_info && ctrl_port_info->configure_cb)
        ctrl_port_info->configure_cb (net_device, ctrl_port_info->physdev_uid);
}

gchar *
mm_net_port_mapper_get_ctrl_iface_name (MMNetPortMapper *self,
                                        const gchar     *net_iface_name)
{
    CtrlPortInfo *ctrl_port_info;

    ctrl_port_info = g_hash_table_lookup (self->ports, net_iface_name);

    return ctrl_port_info ? ctrl_port_info->name : NULL;
}

#if defined WITH_QMI && QMI_QRTR_SUPPORTED
guint
mm_net_port_mapper_get_mux_id (MMNetPortMapper *self,
                               const gchar     *net_iface_name)
{
    CtrlPortInfo *ctrl_port_info;

    ctrl_port_info = g_hash_table_lookup (self->ports, net_iface_name);

    return ctrl_port_info ? ctrl_port_info->mux_id : QMI_DEVICE_MUX_ID_UNBOUND;
}
#endif

gchar *
mm_net_port_mapper_get_ctrl_iface_physdev_uid (MMNetPortMapper *self,
                                               const gchar     *net_iface_name)
{
    CtrlPortInfo *ctrl_port_info;

    ctrl_port_info = g_hash_table_lookup (self->ports, net_iface_name);

    return ctrl_port_info ? ctrl_port_info->physdev_uid : NULL;
}

/*****************************************************************************/

static void
mm_net_port_mapper_init (MMNetPortMapper *self)
{
    /* Setup the internal list of port objects */
    self->ports = g_hash_table_new_full (g_str_hash,
                                         g_str_equal,
                                         g_free,
                                         (GDestroyNotify) ctrl_port_info_free);
}

static void
finalize (GObject *object)
{
    MMNetPortMapper *self = MM_NET_PORT_MAPPER (object);

    g_hash_table_destroy (self->ports);

    G_OBJECT_CLASS (mm_net_port_mapper_parent_class)->finalize (object);
}

static void
log_object_iface_init (MMLogObjectInterface *iface)
{
    iface->build_id = log_object_build_id;
}

static void
mm_net_port_mapper_class_init (MMNetPortMapperClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = finalize;
}

MM_DEFINE_SINGLETON_GETTER (MMNetPortMapper,
                            mm_net_port_mapper_get,
                            MM_TYPE_NET_PORT_MAPPER)