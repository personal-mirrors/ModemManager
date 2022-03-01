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

#ifndef MM_INTEL_LOCATION_CORE_H
#define MM_INTEL_LOCATION_CORE_H

#include <glib-object.h>
#include <gio/gio.h>

#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-broadband-modem.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-location.h"
#include "mm-intel-main.h"

#define MM_AT_COMMAND_SIZE 128

#define PRIVATE_TAG "intel-location-private-tag"

/**
 * LocationEngineState:
 * Flags describing GPS Engine states of the device
 *
 * Since: 1.20
 */
typedef enum {
    LOCATION_ENGINE_STATE_OFF,      /* Engine OFF      */
    LOCATION_ENGINE_STATE_DISABLED, /* Engine Disabled */
    LOCATION_ENGINE_STATE_ON        /* Engine ON       */
} LocationEngineState;

typedef struct {
    GRegex                *m2_nmea_regex;
    GRegex                *xlsrstop_regex;
} IntelLocationM2Regex;

typedef union {
    IntelLocationM2Regex  *m2_regex;
} IntelLocationRegex;

/**
 * IntelLocationGNSSCommands:
 * Flags used for fetching cmd data from Hash table
 *
 * Since: 1.19
 */
typedef enum  {
    LOCATION_START, /* Start the GNSS session */
    LOCATION_STOP,  /* Stop the GNSS session  */
    LOCATION_MAX_CMDS
} IntelLocationGNSSCommands;

typedef struct  {
    guint command_id;
    gchar command[MM_AT_COMMAND_SIZE];
    gboolean cmd_args_present;
    void (*cmd_response_cb)(MMBaseModem  *self,
                            GAsyncResult *res,
                            GTask        *task);
    void  (*create_command)(MMBaseModem  *self,
                            gchar        **cmd);
} IntelLocationCmdLookupTable;

typedef struct {
    /* Broadband modem class support */
    MMBroadbandModemClass       *broadband_modem_class_parent;

    /* Location interface support */
    MMIfaceModemLocation        *iface_modem_location_parent;
    MMModemLocationSource       supported_sources;
    MMModemLocationSource       enabled_sources;
    LocationEngineState         location_engine_state;
    MMPortSerialAt              *gps_port;
    IntelLocationRegex          *location_regex;
    IntelLocationCmdLookupTable *cmd_table;
} IntelLocationContextPrivate;

IntelLocationContextPrivate *il_get_location_context (MMSharedIntel *self);

void il_load_capabilities (MMIfaceModemLocation    *self,
                           GAsyncReadyCallback     callback,
                           gpointer                user_data);

MMModemLocationSource il_load_capabilities_finish (MMIfaceModemLocation   *self,
                                                   GAsyncResult           *res,
                                                   GError                 **error);

void il_enable_location_gathering (MMIfaceModemLocation    *self,
                                   MMModemLocationSource   source,
                                   GAsyncReadyCallback     callback,
                                   gpointer                user_data);

gboolean il_enable_location_gathering_finish  (MMIfaceModemLocation   *self,
                                               GAsyncResult           *res,
                                               GError                 **error);

void il_disable_location_gathering (MMIfaceModemLocation    *self,
                                    MMModemLocationSource   source,
                                    GAsyncReadyCallback     callback,
                                    gpointer                user_data);

gboolean il_disable_location_gathering_finish (MMIfaceModemLocation   *self,
                                               GAsyncResult           *res,
                                               GError                 **error);

void il_set_supl_server (MMIfaceModemLocation    *self,
                         const gchar             *supl,
                         GAsyncReadyCallback     callback,
                         gpointer                user_data);

gboolean il_set_supl_server_finish  (MMIfaceModemLocation   *self,
                                     GAsyncResult           *res,
                                     GError                 **error);

void il_xlcslsr_create (MMBaseModem  *self,
                        gchar        **cmd) ;

void il_context_init (MMBroadbandModem *self);

void il_position_fix_req_cb (MMBaseModem  *self,
                             GAsyncResult *res,
                             GTask        *task);

void il_stop_fix_req_cb (MMBaseModem  *self,
                         GAsyncResult *res,
                         GTask        *task);

#endif /* MM_INTEL_LOCATION_CORE_H */
