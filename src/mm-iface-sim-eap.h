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
 * Copyright (C) 2011-2012 Google, Inc.
 */

#ifndef MM_IFACE_SIM_EAP_H
#define MM_IFACE_SIM_EAP_H

#include <glib-object.h>
#include <gio/gio.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-base-sim.h"
#include "mm-device.h"

#define MM_TYPE_IFACE_SIM_EAP                  (mm_iface_sim_eap_get_type ())
#define MM_IFACE_SIM_EAP(obj)                  (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_IFACE_SIM_EAP, MMIfaceSimEap))
#define MM_IS_IFACE_SIM_EAP(obj)               (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_IFACE_SIM_EAP))
#define MM_IFACE_SIM_EAP_GET_INTERFACE(obj)    (G_TYPE_INSTANCE_GET_INTERFACE ((obj), MM_TYPE_IFACE_SIM_EAP, MMIfaceSimEap))

#define MM_IFACE_SIM_EAP_DBUS_SKELETON  "iface-sim-eap-dbus-skeleton"

typedef struct _MMIfaceSimEap MMIfaceSimEap;

/*****************************************************************************/
/* EAP-SIM response */
typedef struct {
    guint32 out_sres1;
    guint32 out_sres2;
    guint32 out_sres3;

    guint64 out_kc1;
    guint64 out_kc2;
    guint64 out_kc3;

    guint32 out_n;
} SimAuthResponse;

/* EAP-AKA and EAP_AKAP response (hence AKAs plural) */
typedef struct {
    const guint8 *out_res;
    guint32 out_res_len;
    const guint8 *out_integrating_key;
    const guint8 *out_ciphering_key;
    const guint8 *out_auts;

} AkasAuthResponse;

struct _MMIfaceSimEap {
    GTypeInterface g_iface;

    /* Send an authentication request via EAP-SIM */

    void      (*sim_auth)        (MMIfaceSimEap *self,
                                  const guint8  **rands,
                                  guint32         rands_size,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);
    gboolean (*sim_auth_finish) (MMDevice               *device,
                                 SimAuthResponse        *resp,
                                 GAsyncResult           *res,
                                 GError                 **error);

    /* Send an authentication request via EAP-AKA */
    void     (*aka_auth)        (MMIfaceSimEap *self,
                                 const guint8  *rand,
                                 const guint8  *autn,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data);
    gboolean (*aka_auth_finish) (MMDevice         *device,
                                 AkasAuthResponse *resp,
                                 GAsyncResult     *res,
                                 GError           **error);

    /* Send an authentication request via EAP-AKAP */
    void     (*akap_auth)        (MMIfaceSimEap *self,
                                  const guint8  *rand,
                                  const guint8  *autn,
                                  const gchar   *network_name,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);
    gboolean (*akap_auth_finish) (MMDevice        *device,
                                  AkasAuthResponse *resp,
                                  GAsyncResult    *res,
                                  GError          **error);
};

GType mm_iface_sim_eap_get_type (void);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (MMIfaceSimEap, g_object_unref)

/* Initialize the Sim Eap interface (synchronous) */
void mm_iface_sim_eap_initialize (MMIfaceSimEap *self);

/* Disable/unexport the Sim Eap interface */
void mm_iface_sim_eap_disable (MMIfaceSimEap *self);

#endif /* MM_IFACE_SIM_EAP_H */
