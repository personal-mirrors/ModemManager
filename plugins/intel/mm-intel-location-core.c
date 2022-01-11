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

#include <config.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MM_LOG_NO_OBJECT
#include "mm-log.h"
#include "mm-base-modem.h"
#include "mm-base-modem-at.h"
#include "mm-intel-location-core.h"

static GQuark intel_location_context_quark;
typedef struct IntelLocationCmdLookupTable cmd_t;

IntelLocationCmdLookupTable intel_m2_cmd_table  [] = {
    {LOCATION_START,           "AT+XLCSLSR",           TRUE,          il_position_fix_req_cb,        il_xlcslsr_create},
    {LOCATION_STOP,           "AT+XLSRSTOP",           FALSE,         il_stop_fix_req_cb,            NULL             }
};

/* The Location APIs are prefixed with "il", meaning Intel Location APIs
  * so, from here onwards il can be interpreted as Intel Location */

static void
il_context_free (IntelLocationContextPrivate *location_context)
{
    g_clear_object (&location_context->gps_port);
    g_regex_unref (location_context->location_regex->m2_regex->m2_nmea_regex);
    g_regex_unref (location_context->location_regex->m2_regex->xlsrstop_regex);
    g_slice_free (IntelLocationM2Regex, location_context->location_regex->m2_regex);
    g_slice_free (IntelLocationContextPrivate, location_context);
}

static void
il_setup_regex (IntelLocationRegex    *location_regex)
{
    location_regex->m2_regex = g_slice_new0 (IntelLocationM2Regex);
    location_regex->m2_regex->m2_nmea_regex =
        g_regex_new ("(?:\\r\\n)?(?:\\r\\n)?(\\$G.*)\\r\\n", G_REGEX_RAW, 0, NULL);
    location_regex->m2_regex->xlsrstop_regex =
        g_regex_new ("\\r\\n\\+XLSRSTOP:(.*)\\r\\n", G_REGEX_RAW, 0, NULL);
}

static IntelLocationContextPrivate *
il_create_location_context (MMSharedIntel *self)
{
    IntelLocationContextPrivate *location_context = NULL;

    intel_location_context_quark = g_quark_from_static_string (PRIVATE_TAG);
    location_context = g_slice_new0 (IntelLocationContextPrivate);
    location_context->location_regex = g_slice_new0 (IntelLocationRegex);
    location_context->location_engine_state = LOCATION_ENGINE_STATE_OFF;

    /* Setup parent class' MMBroadbandModemClass */
    g_assert (MM_SHARED_INTEL_GET_INTERFACE (self)->peek_parent_broadband_modem_class);
    location_context->broadband_modem_class_parent = MM_SHARED_INTEL_GET_INTERFACE (self)->peek_parent_broadband_modem_class (self);

    /* Setup parent class' MMIfaceModemLocation */
    g_assert (MM_SHARED_INTEL_GET_INTERFACE (self)->peek_parent_location_interface);
    location_context->iface_modem_location_parent = MM_SHARED_INTEL_GET_INTERFACE (self)->peek_parent_location_interface (self);

    g_object_set_qdata_full (G_OBJECT (self), intel_location_context_quark, location_context, (GDestroyNotify)il_context_free);
    return location_context;
}

void
il_context_init (MMBroadbandModem       *self)
{
    MMPortSerialAt              *at_port;
    IntelLocationContextPrivate *location_context;

    location_context = il_create_location_context (MM_SHARED_INTEL (self));
    at_port = mm_base_modem_peek_port_primary   (MM_BASE_MODEM (self));

    g_assert(at_port);
    location_context->cmd_table = intel_m2_cmd_table;
    il_setup_regex (location_context->location_regex);

    /* After running AT+XLSRSTOP we may get an unsolicited response
     * reporting its status, we just ignore it.
     */
    mm_port_serial_at_add_unsolicited_msg_handler (
        at_port,
        location_context->location_regex->m2_regex->xlsrstop_regex,
        NULL, NULL, NULL);

    /* make sure GPS is stopped in case it was left enabled */
    mm_base_modem_at_command_full (MM_BASE_MODEM (self),
                                   at_port,
                                   "+XLSRSTOP",
                                   3,
                                   FALSE,
                                   FALSE,
                                   NULL,
                                   NULL,
                                   NULL);
}

IntelLocationContextPrivate *
il_get_location_context (MMSharedIntel *self)
{
    if (G_UNLIKELY (!intel_location_context_quark))
        intel_location_context_quark = g_quark_try_string(PRIVATE_TAG);

    g_assert (intel_location_context_quark);
    return g_object_get_qdata (G_OBJECT (self), intel_location_context_quark);
}

static gboolean
il_is_system_location_enabled (void)
{
    GVariant               *value;
    gchar                  *location_status;
    GSettings              *settings;
    GSettingsSchema        *schema;
    GSettingsSchemaSource  *src_dir;

    src_dir = g_settings_schema_source_get_default ();
    schema  = g_settings_schema_source_lookup (src_dir, "org.gnome.system.location", TRUE);
    if (schema == NULL) {
        mm_warn ("org.gnome.system.location - schema not available");
        return TRUE;
    }

    settings = g_settings_new ("org.gnome.system.location");
    value = g_settings_get_value (settings, "enabled");
    location_status = g_variant_print (value, TRUE);
    mm_dbg ("org.gnome.system.location enabled [%s]", location_status);

    if (g_strcmp0(location_status,"true") == 0)
        return TRUE;
    return FALSE;
}

MMModemLocationSource
il_load_capabilities_finish (MMIfaceModemLocation  *self,
                             GAsyncResult          *res,
                             GError                **error)
{
    GError *inner_error = NULL;
    gssize  value;

    value = g_task_propagate_int (G_TASK (res), &inner_error);
    if (inner_error) {
        g_propagate_error (error, inner_error);
        return MM_MODEM_LOCATION_SOURCE_NONE;
    }

    return (MMModemLocationSource)value;
}

static void
parent_load_capabilities_ready (MMIfaceModemLocation *self,
                                GAsyncResult         *res,
                                GTask                *task)
{
    MMModemLocationSource        sources;
    GError                       *error = NULL;
    IntelLocationContextPrivate  *location_context;

    location_context = il_get_location_context (MM_SHARED_INTEL (self));
    sources = location_context->iface_modem_location_parent->load_capabilities_finish (self, res, &error);
    if (error) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* If parent already supports GPS sources, we won't do anything else */
    if (sources & (MM_MODEM_LOCATION_SOURCE_GPS_NMEA | MM_MODEM_LOCATION_SOURCE_GPS_RAW)) {
        g_task_return_int (task, sources);
        g_object_unref (task);
        return;
    }

    location_context->supported_sources |=
        (MM_MODEM_LOCATION_SOURCE_GPS_NMEA | MM_MODEM_LOCATION_SOURCE_GPS_RAW);

    sources |= location_context->supported_sources;
    g_task_return_int (task, sources);
    g_object_unref (task);
}

void
il_load_capabilities (MMIfaceModemLocation *self,
                      GAsyncReadyCallback   callback,
                      gpointer              user_data)
{
    GTask                        *task;
    IntelLocationContextPrivate  *location_context;
    MMModemLocationSource        sources;

    location_context = il_get_location_context (MM_SHARED_INTEL (self));
    g_assert (location_context->iface_modem_location_parent);
    task = g_task_new (self, NULL, callback, user_data);

    if (!location_context->iface_modem_location_parent->load_capabilities ||
            !location_context->iface_modem_location_parent->load_capabilities_finish) {
        /* no parent capabilities */

        location_context->supported_sources |=
            (MM_MODEM_LOCATION_SOURCE_GPS_NMEA | MM_MODEM_LOCATION_SOURCE_GPS_RAW);

        sources = location_context->supported_sources;

        g_task_return_int (task, sources);
        g_object_unref (task);
        return;
    }

    location_context->iface_modem_location_parent->load_capabilities (
        self,
        (GAsyncReadyCallback)parent_load_capabilities_ready,
        task);
}

static void
il_nmea_indication_cb (MMPortSerialAt *port,
                       GMatchInfo     *info,
                       MMSharedIntel        *self)
{
    gchar *trace = g_match_info_fetch (info, 1);

    mm_iface_modem_location_gps_update (MM_IFACE_MODEM_LOCATION (self), trace);
    g_free (trace);
}

static gboolean
il_handle_gnss_session_based_on_state_finish (MMSharedIntel       *self,
                                              GAsyncResult  *res,
                                              GError        **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
il_position_fix_req_cb (MMBaseModem  *self,
                        GAsyncResult *res,
                        GTask        *task)
{
    LocationEngineState          state;
    const gchar                  *response;
    GError                       *error = NULL;
    IntelLocationContextPrivate  *location_context;

    location_context = il_get_location_context (MM_SHARED_INTEL (self));
    response = mm_base_modem_at_command_full_finish (self, res, &error);

    if (!response) {
        mm_err ("Error enabling location request");
        g_task_return_error (task, error);
        g_object_unref (task);
        g_clear_object (&location_context->gps_port);
        return;
    }

    state = GPOINTER_TO_UINT (g_task_get_task_data (task));

    g_assert (location_context->gps_port);
    location_context->location_engine_state = state;

    mm_port_serial_at_add_unsolicited_msg_handler (
        location_context->gps_port,
        location_context->location_regex->m2_regex->m2_nmea_regex,
        (MMPortSerialAtUnsolicitedMsgFn)il_nmea_indication_cb,
        self,
        NULL);

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
il_start_gnss_session (GTask *task)
{
    MMSharedIntel                      *self;
    IntelLocationContextPrivate  *location_context;
    gchar                        *cmd;
    IntelLocationCmdLookupTable  *cmd_table;

    self  = g_task_get_source_object (task);
    location_context  = il_get_location_context (self);

    /* Look for an AT port to use for GPS. Prefer secondary port if there is one,
     * otherwise use primary */
    g_assert (!location_context->gps_port);
    location_context->gps_port = mm_base_modem_get_port_secondary (MM_BASE_MODEM (self));

    if (!location_context->gps_port) {
        location_context->gps_port = mm_base_modem_get_port_primary (MM_BASE_MODEM (self));
        if (!location_context->gps_port) {
            g_task_return_new_error (task,
                                     MM_CORE_ERROR,
                                     MM_CORE_ERROR_FAILED,
                                     "No valid port found to control GPS");
            g_object_unref (task);
            return;
        }
    }
    g_assert (location_context->gps_port);

    cmd_table = &(location_context->cmd_table[LOCATION_START]);

    if (!cmd_table->cmd_args_present)
        cmd = cmd_table->command;
    else
        cmd_table->create_command (MM_BASE_MODEM (self), &cmd);

    /*  Start a Fix */
    mm_base_modem_at_command_full (MM_BASE_MODEM (self),
                                   location_context->gps_port,
                                   cmd,
                                   3,
                                   FALSE,
                                   FALSE, /* raw */
                                   NULL, /* cancellable */
                                   (GAsyncReadyCallback)cmd_table->cmd_response_cb,
                                   task);

    if (cmd_table->cmd_args_present)
        g_free (cmd);
}

void
il_stop_fix_req_cb (MMBaseModem  *self,
                    GAsyncResult *res,
                    GTask        *task)
{
    LocationEngineState          state;
    const gchar                  *response;
    GError                       *error = NULL;
    IntelLocationContextPrivate  *location_context;

    response = mm_base_modem_at_command_full_finish (self, res, &error);
    if (!response) {
        mm_err ("Error while processing stop location fix request");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    location_context = il_get_location_context (MM_SHARED_INTEL (self));
    state = GPOINTER_TO_UINT (g_task_get_task_data (task));

    g_assert (location_context->gps_port);

    mm_port_serial_at_add_unsolicited_msg_handler (
        location_context->gps_port,
        location_context->location_regex->m2_regex->m2_nmea_regex,
        NULL,
        NULL,
        NULL);

    g_clear_object (&location_context->gps_port);
    location_context->location_engine_state = LOCATION_ENGINE_STATE_OFF;

    /* If already reached requested state, we're done */
    if (state == location_context->location_engine_state) {
        /* If we had an error when requesting this specific state, report it */
        if (error)
            g_task_return_error (task, error);
        else
            g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    /* Ignore errors if the stop operation was an intermediate one */
    g_clear_error (&error);
}

static void
il_stop_gnss_session (GTask *task)
{
    MMSharedIntel                     *self;
    IntelLocationContextPrivate *location_context;
    gchar                       *cmd;
    IntelLocationCmdLookupTable *cmd_table;

    self = g_task_get_source_object (task);
    location_context = il_get_location_context (self);

    g_assert (location_context->gps_port);

    cmd_table = &(location_context->cmd_table[LOCATION_STOP]);

    if (!cmd_table->cmd_args_present)
        cmd = cmd_table->command;
    else
        cmd_table->create_command (MM_BASE_MODEM (self), &cmd);

    mm_base_modem_at_command_full (MM_BASE_MODEM (self),
                                   location_context->gps_port,
                                   cmd,
                                   3,
                                   FALSE,
                                   FALSE, /* raw */
                                   NULL, /* cancellable */
                                   (GAsyncReadyCallback)cmd_table->cmd_response_cb,
                                   task);

}

static void
il_handle_gnss_session_based_on_state (MMSharedIntel                  *self,
                                       LocationEngineState      state,
                                       GAsyncReadyCallback      callback,
                                       gpointer                 user_data)
{
    GTask                       *task;
    IntelLocationContextPrivate *location_context;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, GUINT_TO_POINTER (state), NULL);

    location_context = il_get_location_context (self);

    /* If already in the requested state, we're done */
    if (state == location_context->location_engine_state) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    /* If states are different we always STOP first */
    if (location_context->location_engine_state != LOCATION_ENGINE_STATE_OFF) {
        il_stop_gnss_session (task);
        return;
    }

    /* If GPS already stopped, go on to START right away */
    g_assert (state != LOCATION_ENGINE_STATE_OFF);
    il_start_gnss_session (task);
}

static LocationEngineState
il_get_location_engine_state (MMModemLocationSource sources)
{
    /* If at lease one of GPS nmea/raw sources enabled, engine started */
    if (sources & (MM_MODEM_LOCATION_SOURCE_GPS_NMEA | MM_MODEM_LOCATION_SOURCE_GPS_RAW)) {
        return LOCATION_ENGINE_STATE_ON;
    }
    /* If no GPS nmea/raw sources enabled, engine stopped */
    return LOCATION_ENGINE_STATE_OFF;
}

gboolean
il_disable_location_gathering_finish (MMIfaceModemLocation  *self,
                                      GAsyncResult          *res,
                                      GError                **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
il_disable_gnss_state_ready (MMSharedIntel      *self,
                             GAsyncResult *res,
                             GTask        *task)
{
    MMModemLocationSource        source;
    GError                       *error = NULL;
    IntelLocationContextPrivate  *location_context;

    location_context = il_get_location_context (MM_SHARED_INTEL (self));

    if (!il_handle_gnss_session_based_on_state_finish (self, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    source = GPOINTER_TO_UINT (g_task_get_task_data (task));
    location_context->enabled_sources &= ~source;

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
parent_disable_location_gathering_ready (MMIfaceModemLocation *self,
                                         GAsyncResult         *res,
                                         GTask                *task)
{
    GError                      *error;
    IntelLocationContextPrivate *location_context;

    location_context = il_get_location_context (MM_SHARED_INTEL (self));

    g_assert (location_context->iface_modem_location_parent);
    if (!location_context->iface_modem_location_parent->disable_location_gathering_finish (self, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

void
il_disable_location_gathering (MMIfaceModemLocation   *self,
                               MMModemLocationSource  source,
                               GAsyncReadyCallback    callback,
                               gpointer               user_data)
{
    IntelLocationContextPrivate *location_context;
    GTask                       *task;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, GUINT_TO_POINTER (source), NULL);

    location_context = il_get_location_context (MM_SHARED_INTEL (self));
    g_assert (location_context->iface_modem_location_parent);

    /* Only consider request if it applies to one of the sources we are
     * supporting, otherwise run parent disable
     */
    if (!(location_context->supported_sources & source)) {
        /* If disabling implemented by the parent, run it. */
        if (location_context->iface_modem_location_parent->disable_location_gathering &&
            location_context->iface_modem_location_parent->disable_location_gathering_finish) {
            location_context->iface_modem_location_parent->disable_location_gathering (
                    self,
                    source,
                    (GAsyncReadyCallback)parent_disable_location_gathering_ready,
                    task);
            return;
        }
        /* Otherwise, we're done */
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    /* We only expect GPS sources here */
    g_assert (source & (MM_MODEM_LOCATION_SOURCE_GPS_NMEA | MM_MODEM_LOCATION_SOURCE_GPS_RAW));

    location_context->enabled_sources &= ~source;

    /* Update engine based on the expected sources */
    il_handle_gnss_session_based_on_state (MM_SHARED_INTEL (self),
                                           il_get_location_engine_state (location_context->enabled_sources & ~source),
                                           (GAsyncReadyCallback) il_disable_gnss_state_ready,
                                           task);
}

gboolean
il_enable_location_gathering_finish (MMIfaceModemLocation  *self,
                                     GAsyncResult          *res,
                                     GError                **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
il_enable_gnss_state_ready (MMSharedIntel      *self,
                            GAsyncResult *res,
                            GTask        *task)
{
    GError                           *error = NULL;
    IntelLocationContextPrivate      *location_context;

    location_context = il_get_location_context (MM_SHARED_INTEL (self));
    if (!location_context)
        mm_warn ("Could not get location context");

    if (!il_handle_gnss_session_based_on_state_finish (self, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

void
il_enable_location_gathering (MMIfaceModemLocation   *self,
                              MMModemLocationSource  source,
                              GAsyncReadyCallback    callback,
                              gpointer               user_data)
{
    IntelLocationContextPrivate  *location_context;
    GTask                        *task;
    GError                       *error = NULL;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, GUINT_TO_POINTER (source), NULL);

    location_context = il_get_location_context (MM_SHARED_INTEL (self));
    g_assert (location_context->iface_modem_location_parent);

    /* We only expect GPS sources here */
    g_assert (source & (MM_MODEM_LOCATION_SOURCE_GPS_NMEA | MM_MODEM_LOCATION_SOURCE_GPS_RAW));

    location_context->enabled_sources |= source;

    if (!il_is_system_location_enabled()) {
        error = g_error_new (MM_CORE_ERROR,
                             MM_CORE_ERROR_UNSUPPORTED,
                             "Gnome location settings are disabled");

        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Update engine based on the expected sources */
    il_handle_gnss_session_based_on_state (MM_SHARED_INTEL (self),
                                           il_get_location_engine_state (location_context->enabled_sources | source),
                                           (GAsyncReadyCallback) il_enable_gnss_state_ready,
                                           task);
}

void
il_xlcslsr_create(MMBaseModem  *self,
                  gchar        **cmd)
{
    /* AT+XLCSLSR=<transport_protocol>[,<pos_mode>[,<client_id>,<client_id_type>[,
    * <mlc_number>,<mlc_number_type>[,<interval>[,<service_type_id>
    * [,<pseudonym_indicator>[,<loc_response_type>[,<nmea_mask>[,<gnss_type>]]]]
    * ]]]]]
    */

    *cmd  = g_strdup_printf ("AT+XLCSLSR=1,1,,,,,1,,,2");
}
