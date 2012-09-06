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
 * Copyright (C) 2012 Google, Inc.
 */

#ifndef MM_SIM_QMI_H
#define MM_SIM_QMI_H

#include <glib.h>
#include <glib-object.h>

#include "mm-sim.h"

#define MM_TYPE_SIM_QMI            (mm_sim_qmi_get_type ())
#define MM_SIM_QMI(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_SIM_QMI, MMSimQmi))
#define MM_SIM_QMI_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  MM_TYPE_SIM_QMI, MMSimQmiClass))
#define MM_IS_SIM_QMI(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_SIM_QMI))
#define MM_IS_SIM_QMI_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  MM_TYPE_SIM_QMI))
#define MM_SIM_QMI_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  MM_TYPE_SIM_QMI, MMSimQmiClass))

typedef struct _MMSimQmi MMSimQmi;
typedef struct _MMSimQmiClass MMSimQmiClass;

struct _MMSimQmi {
    MMSim parent;
};

struct _MMSimQmiClass {
    MMSimClass parent;
};

GType mm_sim_qmi_get_type (void);

void   mm_sim_qmi_new        (MMBaseModem *modem,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data);
MMSim *mm_sim_qmi_new_finish (GAsyncResult  *res,
                              GError       **error);

#endif /* MM_SIM_QMI_H */
