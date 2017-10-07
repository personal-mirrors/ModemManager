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
 * Copyright (C) 2011 Ammonit Measurement GmbH
 * Copyright (C) 2011 Google Inc.
 * Copyright (C) 2016 Trimble Navigation Limited
 * Author: Aleksander Morgado <aleksander@lanedo.com>
 * Contributor: Matthew Stanger <matthew_stanger@trimble.com>
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "ModemManager.h"
#include "mm-modem-helpers.h"
#include "mm-serial-parsers.h"
#include "mm-log.h"
#include "mm-errors-types.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-iface-modem-messaging.h"
#include "mm-iface-modem-location.h"
#include "mm-base-modem-at.h"
#include "mm-broadband-modem-cinterion.h"
#include "mm-modem-helpers-cinterion.h"
#include "mm-common-cinterion.h"
#include "mm-broadband-bearer-cinterion.h"

static void iface_modem_init      (MMIfaceModem *iface);
static void iface_modem_3gpp_init (MMIfaceModem3gpp *iface);
static void iface_modem_messaging_init (MMIfaceModemMessaging *iface);
static void iface_modem_location_init (MMIfaceModemLocation *iface);

static MMIfaceModem *iface_modem_parent;
static MMIfaceModem3gpp *iface_modem_3gpp_parent;

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemCinterion, mm_broadband_modem_cinterion, MM_TYPE_BROADBAND_MODEM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_3GPP, iface_modem_3gpp_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_MESSAGING, iface_modem_messaging_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_LOCATION, iface_modem_location_init))

typedef enum {
    FEATURE_SUPPORT_UNKNOWN,
    FEATURE_NOT_SUPPORTED,
    FEATURE_SUPPORTED,
} FeatureSupport;

struct _MMBroadbandModemCinterionPrivate {
    /* Command to go into sleep mode */
    gchar *sleep_mode_cmd;

    /* Cached manual selection attempt */
    gchar *manual_operator_id;

    /* Cached supported bands in Cinterion format */
    guint supported_bands;

    /* Cached supported modes for SMS setup */
    GArray *cnmi_supported_mode;
    GArray *cnmi_supported_mt;
    GArray *cnmi_supported_bm;
    GArray *cnmi_supported_ds;
    GArray *cnmi_supported_bfr;

    /* +CIEV 'psinfo' indications */
    GRegex *ciev_psinfo_regex;

    /* Flags for feature support checks */
    FeatureSupport swwan_support;
    FeatureSupport sind_psinfo_support;
};

/*****************************************************************************/
/* Enable unsolicited events (SMS indications) (Messaging interface) */

static gboolean
messaging_enable_unsolicited_events_finish (MMIfaceModemMessaging  *self,
                                            GAsyncResult           *res,
                                            GError               **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
cnmi_test_ready (MMBaseModem  *self,
                 GAsyncResult *res,
                 GTask        *task)
{
    GError *error = NULL;

    if (!mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static gboolean
value_supported (const GArray *array,
                 const guint value)
{
    guint i;

    if (!array)
        return FALSE;

    for (i = 0; i < array->len; i++) {
        if (g_array_index (array, guint, i) == value)
            return TRUE;
    }
    return FALSE;
}

static void
messaging_enable_unsolicited_events (MMIfaceModemMessaging *_self,
                                     GAsyncReadyCallback    callback,
                                     gpointer               user_data)
{
    MMBroadbandModemCinterion *self = MM_BROADBAND_MODEM_CINTERION (_self);
    GString                   *cmd;
    GError                    *error = NULL;
    GTask                     *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* AT+CNMI=<mode>,[<mt>[,<bm>[,<ds>[,<bfr>]]]] */
    cmd = g_string_new ("+CNMI=");

    /* Mode 2 or 1 */
    if (value_supported (self->priv->cnmi_supported_mode, 2))
        g_string_append_printf (cmd, "%u,", 2);
    else if (value_supported (self->priv->cnmi_supported_mode, 1))
        g_string_append_printf (cmd, "%u,", 1);
    else {
        error = g_error_new (MM_CORE_ERROR,
                             MM_CORE_ERROR_FAILED,
                             "SMS settings don't accept [2,1] <mode>");
        goto out;
    }

    /* mt 2 or 1 */
    if (value_supported (self->priv->cnmi_supported_mt, 2))
        g_string_append_printf (cmd, "%u,", 2);
    else if (value_supported (self->priv->cnmi_supported_mt, 1))
        g_string_append_printf (cmd, "%u,", 1);
    else {
        error = g_error_new (MM_CORE_ERROR,
                             MM_CORE_ERROR_FAILED,
                             "SMS settings don't accept [2,1] <mt>");
        goto out;
    }

    /* bm 2 or 0 */
    if (value_supported (self->priv->cnmi_supported_bm, 2))
        g_string_append_printf (cmd, "%u,", 2);
    else if (value_supported (self->priv->cnmi_supported_bm, 0))
        g_string_append_printf (cmd, "%u,", 0);
    else {
        error = g_error_new (MM_CORE_ERROR,
                             MM_CORE_ERROR_FAILED,
                             "SMS settings don't accept [2,0] <bm>");
        goto out;
    }

    /* ds 2, 1 or 0 */
    if (value_supported (self->priv->cnmi_supported_ds, 2))
        g_string_append_printf (cmd, "%u,", 2);
    else if (value_supported (self->priv->cnmi_supported_ds, 1))
        g_string_append_printf (cmd, "%u,", 1);
    else if (value_supported (self->priv->cnmi_supported_ds, 0))
        g_string_append_printf (cmd, "%u,", 0);
    else {
        error = g_error_new (MM_CORE_ERROR,
                             MM_CORE_ERROR_FAILED,
                             "SMS settings don't accept [2,1,0] <ds>");
        goto out;
    }

    /* bfr 1 */
    if (value_supported (self->priv->cnmi_supported_bfr, 1))
        g_string_append_printf (cmd, "%u", 1);
    /* otherwise, skip setting it */

out:
    /* Early error report */
    if (error) {
        g_task_return_error (task, error);
        g_object_unref (task);
        g_string_free (cmd, TRUE);
        return;
    }

    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              cmd->str,
                              3,
                              FALSE,
                              (GAsyncReadyCallback)cnmi_test_ready,
                              task);
    g_string_free (cmd, TRUE);
}

/*****************************************************************************/
/* Check if Messaging supported (Messaging interface) */

static gboolean
messaging_check_support_finish (MMIfaceModemMessaging  *self,
                                GAsyncResult           *res,
                                GError                **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
cnmi_format_check_ready (MMBaseModem  *_self,
                         GAsyncResult *res,
                         GTask        *task)
{
    MMBroadbandModemCinterion *self = MM_BROADBAND_MODEM_CINTERION (_self);
    GError                    *error = NULL;
    const gchar               *response;

    response = mm_base_modem_at_command_finish (_self, res, &error);
    if (error) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Parse */
    if (!mm_cinterion_parse_cnmi_test (response,
                                       &self->priv->cnmi_supported_mode,
                                       &self->priv->cnmi_supported_mt,
                                       &self->priv->cnmi_supported_bm,
                                       &self->priv->cnmi_supported_ds,
                                       &self->priv->cnmi_supported_bfr,
                                       &error)) {
        mm_warn ("error reading SMS setup: %s", error->message);
        g_error_free (error);
    }

    /* CNMI command is supported; assume we have full messaging capabilities */
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
messaging_check_support (MMIfaceModemMessaging *self,
                         GAsyncReadyCallback    callback,
                         gpointer               user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* We assume that CDMA-only modems don't have messaging capabilities */
    if (mm_iface_modem_is_cdma_only (MM_IFACE_MODEM (self))) {
        g_task_return_new_error (task,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_UNSUPPORTED,
                                 "CDMA-only modems don't have messaging capabilities");
        g_object_unref (task);
        return;
    }

    /* Check CNMI support */
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "+CNMI=?",
                              3,
                              TRUE,
                              (GAsyncReadyCallback)cnmi_format_check_ready,
                              task);
}

/*****************************************************************************/
/* Power down */

static gboolean
modem_power_down_finish (MMIfaceModem  *self,
                         GAsyncResult  *res,
                         GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
sleep_ready (MMBaseModem  *self,
             GAsyncResult *res,
             GTask        *task)
{
    GError *error = NULL;

    if (!mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error)) {
        /* Ignore errors */
        mm_dbg ("Couldn't send power down command: '%s'", error->message);
        g_error_free (error);
    }

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
send_sleep_mode_command (GTask *task)
{
    MMBroadbandModemCinterion *self;

    self = g_task_get_source_object (task);

    if (self->priv->sleep_mode_cmd && self->priv->sleep_mode_cmd[0]) {
        mm_base_modem_at_command (MM_BASE_MODEM (self),
                                  self->priv->sleep_mode_cmd,
                                  5,
                                  FALSE,
                                  (GAsyncReadyCallback)sleep_ready,
                                  task);
        return;
    }

    /* No default command; just finish without sending anything */
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
supported_functionality_status_query_ready (MMBaseModem  *_self,
                                            GAsyncResult *res,
                                            GTask        *task)
{
    MMBroadbandModemCinterion *self = MM_BROADBAND_MODEM_CINTERION (_self);
    const gchar               *response;
    GError                    *error = NULL;

    g_assert (self->priv->sleep_mode_cmd == NULL);

    response = mm_base_modem_at_command_finish (_self, res, &error);
    if (!response) {
        mm_warn ("Couldn't query supported functionality status: '%s'", error->message);
        g_error_free (error);
        self->priv->sleep_mode_cmd = g_strdup ("");
    } else {
        /* We need to get which power-off command to use to put the modem in low
         * power mode (with serial port open for AT commands, but with RF switched
         * off). According to the documentation of various Cinterion modems, some
         * support AT+CFUN=4 (HC25) and those which don't support it can use
         * AT+CFUN=7 (CYCLIC SLEEP mode with 2s timeout after last character
         * received in the serial port).
         *
         * So, just look for '4' in the reply; if not found, look for '7', and if
         * not found, report warning and don't use any.
         */
        if (strstr (response, "4") != NULL) {
            mm_dbg ("Device supports CFUN=4 sleep mode");
            self->priv->sleep_mode_cmd = g_strdup ("+CFUN=4");
        } else if (strstr (response, "7") != NULL) {
            mm_dbg ("Device supports CFUN=7 sleep mode");
            self->priv->sleep_mode_cmd = g_strdup ("+CFUN=7");
        } else {
            mm_warn ("Unknown functionality mode to go into sleep mode");
            self->priv->sleep_mode_cmd = g_strdup ("");
        }
    }

    send_sleep_mode_command (task);
}

static void
modem_power_down (MMIfaceModem        *_self,
                  GAsyncReadyCallback  callback,
                  gpointer             user_data)
{
    MMBroadbandModemCinterion *self = MM_BROADBAND_MODEM_CINTERION (_self);
    GTask                     *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* If sleep command already decided, use it. */
    if (self->priv->sleep_mode_cmd)
        send_sleep_mode_command (task);
    else
        mm_base_modem_at_command (
            MM_BASE_MODEM (self),
            "+CFUN=?",
            3,
            FALSE,
            (GAsyncReadyCallback)supported_functionality_status_query_ready,
            task);
}

/*****************************************************************************/
/* Modem Power Off */

#define MAX_POWER_OFF_WAIT_TIME_SECS 20

typedef struct {
    MMPortSerialAt *port;
    GRegex         *shutdown_regex;
    gboolean        shutdown_received;
    gboolean        smso_replied;
    gboolean        serial_open;
    guint           timeout_id;
} PowerOffContext;

static void
power_off_context_free (PowerOffContext *ctx)
{
    if (ctx->serial_open)
        mm_port_serial_close (MM_PORT_SERIAL (ctx->port));
    if (ctx->timeout_id)
        g_source_remove (ctx->timeout_id);
    mm_port_serial_at_add_unsolicited_msg_handler (ctx->port, ctx->shutdown_regex, NULL, NULL, NULL);
    g_object_unref (ctx->port);
    g_regex_unref (ctx->shutdown_regex);
    g_slice_free (PowerOffContext, ctx);
}

static gboolean
modem_power_off_finish (MMIfaceModem  *self,
                        GAsyncResult  *res,
                        GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
complete_power_off (GTask *task)
{
    PowerOffContext *ctx;

    ctx = g_task_get_task_data (task);

    if (!ctx->shutdown_received || !ctx->smso_replied)
        return;

    /* remove timeout right away */
    g_assert (ctx->timeout_id);
    g_source_remove (ctx->timeout_id);
    ctx->timeout_id = 0;

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
smso_ready (MMBaseModem  *self,
            GAsyncResult *res,
            GTask        *task)
{
    PowerOffContext *ctx;
    GError          *error = NULL;

    ctx = g_task_get_task_data (task);

    if (!mm_base_modem_at_command_full_finish (MM_BASE_MODEM (self), res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Set as replied and see if we can complete */
    ctx->smso_replied = TRUE;
    complete_power_off (task);
}

static void
shutdown_received (MMPortSerialAt *port,
                   GMatchInfo     *match_info,
                   GTask          *task)
{
    PowerOffContext *ctx;

    ctx = g_task_get_task_data (task);

    /* Cleanup handler right away, we don't want it called any more */
    mm_port_serial_at_add_unsolicited_msg_handler (port, ctx->shutdown_regex, NULL, NULL, NULL);

    /* Set as received and see if we can complete */
    ctx->shutdown_received = TRUE;
    complete_power_off (task);
}

static gboolean
power_off_timeout_cb (GTask *task)
{
    PowerOffContext *ctx;

    ctx = g_task_get_task_data (task);

    ctx->timeout_id = 0;

    /* The SMSO reply should have come earlier */
    g_warn_if_fail (ctx->smso_replied == TRUE);

    /* Cleanup handler right away, we no longer want to receive it */
    mm_port_serial_at_add_unsolicited_msg_handler (ctx->port, ctx->shutdown_regex, NULL, NULL, NULL);

    g_task_return_new_error (task,
                             MM_CORE_ERROR,
                             MM_CORE_ERROR_FAILED,
                             "Power off operation timed out");
    g_object_unref (task);

    return G_SOURCE_REMOVE;
}

static void
modem_power_off (MMIfaceModem        *self,
                 GAsyncReadyCallback  callback,
                 gpointer             user_data)
{
    GTask           *task;
    PowerOffContext *ctx;
    GError          *error = NULL;

    task = g_task_new (self, NULL, callback, user_data);

    ctx = g_slice_new0 (PowerOffContext);
    ctx->port = mm_base_modem_get_port_primary (MM_BASE_MODEM (self));
    ctx->shutdown_regex = g_regex_new ("\\r\\n\\^SHUTDOWN\\r\\n",
                                       G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    ctx->timeout_id = g_timeout_add_seconds (MAX_POWER_OFF_WAIT_TIME_SECS,
                                             (GSourceFunc)power_off_timeout_cb,
                                             task);
    g_task_set_task_data (task, ctx, (GDestroyNotify) power_off_context_free);

    /* We'll need to wait for a ^SHUTDOWN before returning the action, which is
     * when the modem tells us that it is ready to be shutdown */
    mm_port_serial_at_add_unsolicited_msg_handler (
        ctx->port,
        ctx->shutdown_regex,
        (MMPortSerialAtUnsolicitedMsgFn)shutdown_received,
        task,
        NULL);

    /* In order to get the ^SHUTDOWN notification, we must keep the port open
     * during the wait time */
    ctx->serial_open = mm_port_serial_open (MM_PORT_SERIAL (ctx->port), &error);
    if (G_UNLIKELY (error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Note: we'll use a timeout < MAX_POWER_OFF_WAIT_TIME_SECS for the AT command,
     * so we're sure that the AT command reply will always come before the timeout
     * fires */
    g_assert (MAX_POWER_OFF_WAIT_TIME_SECS > 5);
    mm_base_modem_at_command_full (MM_BASE_MODEM (self),
                                   ctx->port,
                                   "^SMSO",
                                   5,
                                   FALSE, /* allow_cached */
                                   FALSE, /* is_raw */
                                   NULL, /* cancellable */
                                   (GAsyncReadyCallback)smso_ready,
                                   task);
}

/*****************************************************************************/
/* Access technologies polling */

static gboolean
load_access_technologies_finish (MMIfaceModem             *self,
                                 GAsyncResult             *res,
                                 MMModemAccessTechnology  *access_technologies,
                                 guint                    *mask,
                                 GError                  **error)
{
    GError *inner_error = NULL;
    gssize val;

    val = g_task_propagate_int (G_TASK (res), &inner_error);
    if (inner_error) {
        g_propagate_error (error, inner_error);
        return FALSE;
    }

    *access_technologies = (MMModemAccessTechnology) val;
    *mask = MM_MODEM_ACCESS_TECHNOLOGY_ANY;
    return TRUE;
}

static void
smong_query_ready (MMBaseModem  *self,
                   GAsyncResult *res,
                   GTask        *task)
{
    const gchar             *response;
    GError                  *error = NULL;
    MMModemAccessTechnology  access_tech;

    response = mm_base_modem_at_command_finish (self, res, &error);
    if (!response || !mm_cinterion_parse_smong_response (response, &access_tech, &error))
        g_task_return_error (task, error);
    else
        g_task_return_int (task, (gssize) access_tech);
    g_object_unref (task);
}

static void
load_access_technologies (MMIfaceModem        *_self,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data)
{
    MMBroadbandModemCinterion *self = MM_BROADBAND_MODEM_CINTERION (_self);
    GTask                     *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* Abort access technology polling if ^SIND psinfo URCs are enabled */
    if (self->priv->sind_psinfo_support == FEATURE_SUPPORTED) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED, "No need to poll access technologies");
        g_object_unref (task);
        return;
    }

    mm_base_modem_at_command (
        MM_BASE_MODEM (self),
        "^SMONG",
        3,
        FALSE,
        (GAsyncReadyCallback)smong_query_ready,
        task);
}

/*****************************************************************************/
/* Disable unsolicited events (3GPP interface) */

static gboolean
modem_3gpp_disable_unsolicited_events_finish (MMIfaceModem3gpp  *self,
                                              GAsyncResult      *res,
                                              GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
parent_disable_unsolicited_events_ready (MMIfaceModem3gpp *self,
                                         GAsyncResult     *res,
                                         GTask            *task)
{
    GError *error = NULL;

    if (!iface_modem_3gpp_parent->disable_unsolicited_events_finish (self, res, &error)) {
        mm_warn ("Couldn't disable parent 3GPP unsolicited events: %s", error->message);
        g_error_free (error);
    }

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
parent_disable_unsolicited_messages (GTask *task)
{
    /* Chain up parent's disable */
    iface_modem_3gpp_parent->disable_unsolicited_events (
        MM_IFACE_MODEM_3GPP (g_task_get_source_object (task)),
        (GAsyncReadyCallback)parent_disable_unsolicited_events_ready,
        task);
}

static void
sind_psinfo_disable_ready (MMBaseModem  *self,
                           GAsyncResult *res,
                           GTask        *task)
{
    GError *error = NULL;

    if (!mm_base_modem_at_command_finish (self, res, &error)) {
        mm_warn ("Couldn't disable ^SIND psinfo notifications: %s", error->message);
        g_error_free (error);
    }

    parent_disable_unsolicited_messages (task);
}

static void
modem_3gpp_disable_unsolicited_events (MMIfaceModem3gpp    *_self,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data)
{
    MMBroadbandModemCinterion *self;
    GTask                     *task;

    self = MM_BROADBAND_MODEM_CINTERION (_self);

    task = g_task_new (self, NULL, callback, user_data);

    if (self->priv->sind_psinfo_support == FEATURE_SUPPORTED) {
        /* Disable access technology update reporting */
        mm_base_modem_at_command (MM_BASE_MODEM (self),
                                  "AT^SIND=\"psinfo\",0",
                                  3,
                                  FALSE,
                                  (GAsyncReadyCallback)sind_psinfo_disable_ready,
                                  task);
        return;
    }

    parent_disable_unsolicited_messages (task);
}

/*****************************************************************************/
/* Enable unsolicited events (3GPP interface) */

static gboolean
modem_3gpp_enable_unsolicited_events_finish (MMIfaceModem3gpp  *self,
                                             GAsyncResult      *res,
                                             GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
sind_psinfo_enable_ready (MMBaseModem  *_self,
                          GAsyncResult *res,
                          GTask        *task)
{
    MMBroadbandModemCinterion *self;
    GError                    *error = NULL;
    const gchar               *response;
    guint                      mode;
    guint                      val;

    self = MM_BROADBAND_MODEM_CINTERION (_self);
    if (!(response = mm_base_modem_at_command_finish (_self, res, &error))) {
        self->priv->sind_psinfo_support = FEATURE_NOT_SUPPORTED;
        mm_warn ("Couldn't enable ^SIND psinfo notifications: %s", error->message);
        g_error_free (error);
    } else if (!mm_cinterion_parse_sind_response (response, NULL, &mode, &val, &error)) {
        self->priv->sind_psinfo_support = FEATURE_NOT_SUPPORTED;
        mm_warn ("Couldn't parse ^SIND psinfo response: %s", error->message);
        g_error_free (error);
    } else {
        /* Flag ^SIND psinfo supported so that we don't poll */
        self->priv->sind_psinfo_support = FEATURE_SUPPORTED;

        /* Report initial access technology gathered right away */
        mm_dbg ("Reporting initial access technologies...");
        mm_iface_modem_update_access_technologies (MM_IFACE_MODEM (self),
                                                   mm_cinterion_get_access_technology_from_sind_psinfo (val),
                                                   MM_IFACE_MODEM_3GPP_ALL_ACCESS_TECHNOLOGIES_MASK);
    }

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
parent_enable_unsolicited_events_ready (MMIfaceModem3gpp *_self,
                                        GAsyncResult     *res,
                                        GTask            *task)
{
    MMBroadbandModemCinterion *self;
    GError                    *error = NULL;

    self = MM_BROADBAND_MODEM_CINTERION (_self);

    if (!iface_modem_3gpp_parent->enable_unsolicited_events_finish (_self, res, &error)) {
        mm_warn ("Couldn't enable parent 3GPP unsolicited events: %s", error->message);
        g_error_free (error);
    }

    if (self->priv->sind_psinfo_support != FEATURE_NOT_SUPPORTED) {
        /* Enable access technology update reporting */
        mm_base_modem_at_command (MM_BASE_MODEM (self),
                                  "AT^SIND=\"psinfo\",1",
                                  3,
                                  FALSE,
                                  (GAsyncReadyCallback)sind_psinfo_enable_ready,
                                  task);
        return;
    }

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
modem_3gpp_enable_unsolicited_events (MMIfaceModem3gpp    *self,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* Chain up parent's enable */
    iface_modem_3gpp_parent->enable_unsolicited_events (
        self,
        (GAsyncReadyCallback)parent_enable_unsolicited_events_ready,
        task);
}

/*****************************************************************************/
/* Setup/Cleanup unsolicited events (3GPP interface) */

static void
sind_psinfo_received (MMPortSerialAt            *port,
                      GMatchInfo                *match_info,
                      MMBroadbandModemCinterion *self)
{
    guint val;

    if (!mm_get_uint_from_match_info (match_info, 1, &val)) {
        mm_dbg ("Failed to convert psinfo value");
        return;
    }

    mm_iface_modem_update_access_technologies (MM_IFACE_MODEM (self),
                                               mm_cinterion_get_access_technology_from_sind_psinfo (val),
                                               MM_IFACE_MODEM_3GPP_ALL_ACCESS_TECHNOLOGIES_MASK);
}

static void
set_unsolicited_events_handlers (MMBroadbandModemCinterion *self,
                                 gboolean                   enable)
{
    MMPortSerialAt *ports[2];
    guint           i;

    ports[0] = mm_base_modem_peek_port_primary   (MM_BASE_MODEM (self));
    ports[1] = mm_base_modem_peek_port_secondary (MM_BASE_MODEM (self));

    /* Enable unsolicited events in given port */
    for (i = 0; i < G_N_ELEMENTS (ports); i++) {
        if (!ports[i])
            continue;

        mm_port_serial_at_add_unsolicited_msg_handler (
            ports[i],
            self->priv->ciev_psinfo_regex,
            enable ? (MMPortSerialAtUnsolicitedMsgFn)sind_psinfo_received : NULL,
            enable ? self : NULL,
            NULL);
    }
}

static gboolean
modem_3gpp_setup_cleanup_unsolicited_events_finish (MMIfaceModem3gpp  *self,
                                                    GAsyncResult      *res,
                                                    GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
parent_setup_unsolicited_events_ready (MMIfaceModem3gpp *self,
                                       GAsyncResult     *res,
                                       GTask            *task)
{
    GError *error = NULL;

    if (!iface_modem_3gpp_parent->setup_unsolicited_events_finish (self, res, &error))
        g_task_return_error (task, error);
    else {
        /* Our own setup now */
        set_unsolicited_events_handlers (MM_BROADBAND_MODEM_CINTERION (self), TRUE);
        g_task_return_boolean (task, TRUE);
    }
    g_object_unref (task);
}

static void
modem_3gpp_setup_unsolicited_events (MMIfaceModem3gpp    *self,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* Chain up parent's setup */
    iface_modem_3gpp_parent->setup_unsolicited_events (
        self,
        (GAsyncReadyCallback)parent_setup_unsolicited_events_ready,
        task);
}

static void
parent_cleanup_unsolicited_events_ready (MMIfaceModem3gpp *self,
                                         GAsyncResult     *res,
                                         GTask            *task)
{
    GError *error = NULL;

    if (!iface_modem_3gpp_parent->cleanup_unsolicited_events_finish (self, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
modem_3gpp_cleanup_unsolicited_events (MMIfaceModem3gpp    *self,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* Our own cleanup first */
    set_unsolicited_events_handlers (MM_BROADBAND_MODEM_CINTERION (self), FALSE);

    /* And now chain up parent's cleanup */
    iface_modem_3gpp_parent->cleanup_unsolicited_events (
        self,
        (GAsyncReadyCallback)parent_cleanup_unsolicited_events_ready,
        task);
}

/*****************************************************************************/
/* Load supported modes (Modem interface) */

static GArray *
load_supported_modes_finish (MMIfaceModem  *self,
                             GAsyncResult  *res,
                             GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
parent_load_supported_modes_ready (MMIfaceModem *self,
                                   GAsyncResult *res,
                                   GTask        *task)
{
    GError *error = NULL;
    GArray *all;
    GArray *combinations;
    GArray *filtered;
    MMModemModeCombination mode;

    all = iface_modem_parent->load_supported_modes_finish (self, res, &error);
    if (!all) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Build list of combinations */
    combinations = g_array_sized_new (FALSE, FALSE, sizeof (MMModemModeCombination), 3);

    /* 2G only */
    mode.allowed = MM_MODEM_MODE_2G;
    mode.preferred = MM_MODEM_MODE_NONE;
    g_array_append_val (combinations, mode);
    /* 3G only */
    mode.allowed = MM_MODEM_MODE_3G;
    mode.preferred = MM_MODEM_MODE_NONE;
    g_array_append_val (combinations, mode);

    if (mm_iface_modem_is_4g (self)) {
        /* 4G only */
        mode.allowed = MM_MODEM_MODE_4G;
        mode.preferred = MM_MODEM_MODE_NONE;
        g_array_append_val (combinations, mode);
        /* 2G, 3G and 4G */
        mode.allowed = (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G);
        mode.preferred = MM_MODEM_MODE_NONE;
        g_array_append_val (combinations, mode);
    } else {
        /* 2G and 3G */
        mode.allowed = (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G);
        mode.preferred = MM_MODEM_MODE_NONE;
        g_array_append_val (combinations, mode);
    }

    /* Filter out those unsupported modes */
    filtered = mm_filter_supported_modes (all, combinations);
    g_array_unref (all);
    g_array_unref (combinations);

    g_task_return_pointer (task, filtered, (GDestroyNotify) g_array_unref);
    g_object_unref (task);
}

static void
load_supported_modes (MMIfaceModem *self,
                      GAsyncReadyCallback callback,
                      gpointer user_data)
{
    /* Run parent's loading */
    iface_modem_parent->load_supported_modes (
        MM_IFACE_MODEM (self),
        (GAsyncReadyCallback)parent_load_supported_modes_ready,
        g_task_new (self, NULL, callback, user_data));
}

/*****************************************************************************/
/* Set current modes (Modem interface) */

static gboolean
set_current_modes_finish (MMIfaceModem  *self,
                          GAsyncResult  *res,
                          GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
allowed_access_technology_update_ready (MMBroadbandModemCinterion *self,
                                        GAsyncResult              *res,
                                        GTask                     *task)
{
    GError *error = NULL;

    mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error);
    if (error)
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
set_current_modes (MMIfaceModem        *_self,
                   MMModemMode          allowed,
                   MMModemMode          preferred,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
    MMBroadbandModemCinterion *self = MM_BROADBAND_MODEM_CINTERION (_self);
    gchar                     *command;
    GTask                     *task;

    g_assert (preferred == MM_MODEM_MODE_NONE);

    task = g_task_new (self, NULL, callback, user_data);

    /* We will try to simulate the possible allowed modes here. The
     * Cinterion devices do not seem to allow setting preferred access
     * technology in devices, but they allow restricting to a given
     * one:
     * - 2G-only is forced by forcing GERAN RAT (AcT=0)
     * - 3G-only is forced by forcing UTRAN RAT (AcT=2)
     * - 4G-only is forced by forcing E-UTRAN RAT (AcT=7)
     * - for the remaining ones, we default to automatic selection of RAT,
     *   which is based on the quality of the connection.
     */

    if (mm_iface_modem_is_4g (_self) && allowed == MM_MODEM_MODE_4G)
        command = g_strdup ("+COPS=,,,7");
    else if (mm_iface_modem_is_3g (_self) && allowed == MM_MODEM_MODE_3G)
        command = g_strdup ("+COPS=,,,2");
    else if (mm_iface_modem_is_2g (_self) && allowed == MM_MODEM_MODE_2G)
        command = g_strdup ("+COPS=,,,0");
    else {
        /* For any other combination (e.g. ANY or  no AcT given, defaults to Auto. For this case, we cannot provide
         * AT+COPS=,,, (i.e. just without a last value). Instead, we need to
         * re-run the last manual/automatic selection command which succeeded,
         * (or auto by default if none was launched) */
        if (self->priv->manual_operator_id)
            command = g_strdup_printf ("+COPS=1,2,\"%s\"", self->priv->manual_operator_id);
        else
            command = g_strdup ("+COPS=0");
    }

    mm_base_modem_at_command (
        MM_BASE_MODEM (self),
        command,
        20,
        FALSE,
        (GAsyncReadyCallback)allowed_access_technology_update_ready,
        task);

    g_free (command);
}

/*****************************************************************************/
/* Register in network (3GPP interface) */

static gboolean
register_in_network_finish (MMIfaceModem3gpp  *self,
                            GAsyncResult      *res,
                            GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
cops_write_ready (MMBaseModem  *_self,
                  GAsyncResult *res,
                  GTask        *task)
{
    MMBroadbandModemCinterion *self = MM_BROADBAND_MODEM_CINTERION (_self);
    GError                    *error = NULL;

    if (!mm_base_modem_at_command_full_finish (_self, res, &error))
        g_task_return_error (task, error);
    else {
        g_free (self->priv->manual_operator_id);
        self->priv->manual_operator_id = g_strdup (g_task_get_task_data (task));
        g_task_return_boolean (task, TRUE);
    }
    g_object_unref (task);
}

static void
register_in_network (MMIfaceModem3gpp    *self,
                     const gchar         *operator_id,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
    GTask *task;
    gchar *command;

    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, g_strdup (operator_id), g_free);

    /* If the user sent a specific network to use, lock it in. */
    if (operator_id)
        command = g_strdup_printf ("+COPS=1,2,\"%s\"", operator_id);
    else
        command = g_strdup ("+COPS=0");

    mm_base_modem_at_command_full (MM_BASE_MODEM (self),
                                   mm_base_modem_peek_best_at_port (MM_BASE_MODEM (self), NULL),
                                   command,
                                   120,
                                   FALSE,
                                   FALSE, /* raw */
                                   cancellable,
                                   (GAsyncReadyCallback)cops_write_ready,
                                   task);
    g_free (command);
}

/*****************************************************************************/
/* Supported bands (Modem interface) */

static GArray *
load_supported_bands_finish (MMIfaceModem  *self,
                             GAsyncResult  *res,
                             GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
scfg_test_ready (MMBaseModem  *_self,
                 GAsyncResult *res,
                 GTask        *task)
{
    MMBroadbandModemCinterion *self = MM_BROADBAND_MODEM_CINTERION (_self);
    const gchar               *response;
    GError                    *error = NULL;
    GArray                    *bands;

    response = mm_base_modem_at_command_finish (_self, res, &error);
    if (!response ||
        !mm_cinterion_parse_scfg_test (response,
                                       mm_broadband_modem_get_current_charset (MM_BROADBAND_MODEM (self)),
                                       &bands,
                                       &error))
        g_task_return_error (task, error);
    else {
        mm_cinterion_build_band (bands, 0, FALSE, &self->priv->supported_bands, NULL);
        g_assert (self->priv->supported_bands != 0);
        g_task_return_pointer (task, bands, (GDestroyNotify)g_array_unref);
    }
    g_object_unref (task);
}

static void
load_supported_bands (MMIfaceModem        *self,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "AT^SCFG=?",
                              3,
                              FALSE,
                              (GAsyncReadyCallback)scfg_test_ready,
                              task);
}

/*****************************************************************************/
/* Load current bands (Modem interface) */

static GArray *
load_current_bands_finish (MMIfaceModem  *self,
                           GAsyncResult  *res,
                           GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
get_band_ready (MMBaseModem  *self,
                GAsyncResult *res,
                GTask        *task)
{
    const gchar *response;
    GError      *error = NULL;
    GArray      *bands = NULL;

    response = mm_base_modem_at_command_finish (self, res, &error);
    if (!response ||
        !mm_cinterion_parse_scfg_response (response,
                                           mm_broadband_modem_get_current_charset (MM_BROADBAND_MODEM (self)),
                                           &bands,
                                           &error))
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, bands, (GDestroyNotify) g_array_unref);
    g_object_unref (task);
}

static void
load_current_bands (MMIfaceModem        *self,
                    GAsyncReadyCallback  callback,
                    gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "AT^SCFG=\"Radio/Band\"",
                              3,
                              FALSE,
                              (GAsyncReadyCallback)get_band_ready,
                              task);
}

/*****************************************************************************/
/* Set current bands (Modem interface) */

static gboolean
set_current_bands_finish (MMIfaceModem  *self,
                          GAsyncResult  *res,
                          GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
scfg_set_ready (MMBaseModem  *self,
                GAsyncResult *res,
                GTask        *task)
{
    GError *error = NULL;

    if (!mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
set_bands_3g (GTask  *task,
              GArray *bands_array)
{
    MMBroadbandModemCinterion *self;
    GError                    *error = NULL;
    guint                      band = 0;
    gchar                     *cmd;

    self = g_task_get_source_object (task);

    if (!mm_cinterion_build_band (bands_array,
                                  self->priv->supported_bands,
                                  FALSE, /* 2G and 3G */
                                  &band,
                                  &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Following the setup:
     *  AT^SCFG="Radion/Band",<rba>
     * We will set the preferred band equal to the allowed band, so that we force
     * the modem to connect at that specific frequency only. Note that we will be
     * passing a number here!
     *
     * The optional <rbe> field is set to 1, so that changes take effect
     * immediately.
     */
    cmd = g_strdup_printf ("^SCFG=\"Radio/Band\",%u,1", band);
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              cmd,
                              15,
                              FALSE,
                              (GAsyncReadyCallback)scfg_set_ready,
                              task);
    g_free (cmd);
}

static void
set_bands_2g (GTask  *task,
              GArray *bands_array)
{
    MMBroadbandModemCinterion *self;
    GError                    *error = NULL;
    guint                      band = 0;
    gchar                     *cmd;
    gchar                     *bandstr;

    self = g_task_get_source_object (task);

    if (!mm_cinterion_build_band (bands_array,
                                  self->priv->supported_bands,
                                  TRUE, /* 2G only */
                                  &band,
                                  &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Build string with the value, in the proper charset */
    bandstr = g_strdup_printf ("%u", band);
    bandstr = mm_broadband_modem_take_and_convert_to_current_charset (MM_BROADBAND_MODEM (self), bandstr);
    if (!bandstr) {
        g_task_return_new_error (task,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_UNSUPPORTED,
                                 "Couldn't convert band set to current charset");
        g_object_unref (task);
        return;
    }

    /* Following the setup:
     *  AT^SCFG="Radion/Band",<rbp>,<rba>
     * We will set the preferred band equal to the allowed band, so that we force
     * the modem to connect at that specific frequency only. Note that we will be
     * passing double-quote enclosed strings here!
     */
    cmd = g_strdup_printf ("^SCFG=\"Radio/Band\",\"%s\",\"%s\"", bandstr, bandstr);

    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              cmd,
                              15,
                              FALSE,
                              (GAsyncReadyCallback)scfg_set_ready,
                              task);

    g_free (cmd);
    g_free (bandstr);
}

static void
set_current_bands (MMIfaceModem        *self,
                   GArray              *bands_array,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
    GTask *task;

    /* The bands that we get here are previously validated by the interface, and
     * that means that ALL the bands given here were also given in the list of
     * supported bands. BUT BUT, that doesn't mean that the exact list of bands
     * will end up being valid, as not all combinations are possible. E.g,
     * Cinterion modems supporting only 2G have specific combinations allowed.
     */
    task = g_task_new (self, NULL, callback, user_data);
    if (mm_iface_modem_is_3g (self))
        set_bands_3g (task, bands_array);
    else
        set_bands_2g (task, bands_array);
}

/*****************************************************************************/
/* Flow control */

static gboolean
setup_flow_control_finish (MMIfaceModem  *self,
                           GAsyncResult  *res,
                           GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
setup_flow_control_ready (MMBaseModem  *self,
                          GAsyncResult *res,
                          GTask        *task)
{
    GError *error = NULL;

    if (!mm_base_modem_at_command_finish (self, res, &error))
        /* Let the error be critical. We DO need RTS/CTS in order to have
         * proper modem disabling. */
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
setup_flow_control (MMIfaceModem        *self,
                    GAsyncReadyCallback  callback,
                    gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* We need to enable RTS/CTS so that CYCLIC SLEEP mode works */
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "\\Q3",
                              3,
                              FALSE,
                              (GAsyncReadyCallback)setup_flow_control_ready,
                              task);
}

/*****************************************************************************/
/* Load unlock retries (Modem interface) */

typedef struct {
    MMUnlockRetries *retries;
    guint            i;
} LoadUnlockRetriesContext;

typedef struct {
    MMModemLock  lock;
    const gchar *command;
} UnlockRetriesMap;

static const UnlockRetriesMap unlock_retries_map [] = {
    { MM_MODEM_LOCK_SIM_PIN,     "^SPIC=\"SC\""   },
    { MM_MODEM_LOCK_SIM_PUK,     "^SPIC=\"SC\",1" },
    { MM_MODEM_LOCK_SIM_PIN2,    "^SPIC=\"P2\""   },
    { MM_MODEM_LOCK_SIM_PUK2,    "^SPIC=\"P2\",1" },
    { MM_MODEM_LOCK_PH_FSIM_PIN, "^SPIC=\"PS\""   },
    { MM_MODEM_LOCK_PH_FSIM_PUK, "^SPIC=\"PS\",1" },
    { MM_MODEM_LOCK_PH_NET_PIN,  "^SPIC=\"PN\""   },
    { MM_MODEM_LOCK_PH_NET_PUK,  "^SPIC=\"PN\",1" },
};

static void
load_unlock_retries_context_free (LoadUnlockRetriesContext *ctx)
{
    g_object_unref (ctx->retries);
    g_slice_free (LoadUnlockRetriesContext, ctx);
}

static MMUnlockRetries *
load_unlock_retries_finish (MMIfaceModem  *self,
                            GAsyncResult  *res,
                            GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void load_unlock_retries_context_step (GTask *task);

static void
spic_ready (MMBaseModem  *self,
            GAsyncResult *res,
            GTask        *task)
{
    LoadUnlockRetriesContext *ctx;
    const gchar              *response;
    GError                   *error = NULL;

    ctx = g_task_get_task_data (task);

    response = mm_base_modem_at_command_finish (self, res, &error);
    if (!response) {
        mm_dbg ("Couldn't load retry count for lock '%s': %s",
                mm_modem_lock_get_string (unlock_retries_map[ctx->i].lock),
                error->message);
        g_error_free (error);
    } else {
        guint val;

        response = mm_strip_tag (response, "^SPIC:");
        if (!mm_get_uint_from_str (response, &val))
            mm_dbg ("Couldn't parse retry count value for lock '%s'",
                    mm_modem_lock_get_string (unlock_retries_map[ctx->i].lock));
        else
            mm_unlock_retries_set (ctx->retries, unlock_retries_map[ctx->i].lock, val);
    }

    /* Go to next lock value */
    ctx->i++;
    load_unlock_retries_context_step (task);
}

static void
load_unlock_retries_context_step (GTask *task)
{
    MMBroadbandModemCinterion *self;
    LoadUnlockRetriesContext  *ctx;

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    if (ctx->i == G_N_ELEMENTS (unlock_retries_map)) {
        g_task_return_pointer (task, g_object_ref (ctx->retries), g_object_unref);
        g_object_unref (task);
        return;
    }

    mm_base_modem_at_command (
        MM_BASE_MODEM (self),
        unlock_retries_map[ctx->i].command,
        3,
        FALSE,
        (GAsyncReadyCallback)spic_ready,
        task);
}

static void
load_unlock_retries (MMIfaceModem        *self,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
    GTask                    *task;
    LoadUnlockRetriesContext *ctx;

    task = g_task_new (self, NULL, callback, user_data);

    ctx = g_slice_new0 (LoadUnlockRetriesContext);
    ctx->retries = mm_unlock_retries_new ();
    ctx->i = 0;
    g_task_set_task_data (task, ctx, (GDestroyNotify)load_unlock_retries_context_free);

    load_unlock_retries_context_step (task);
}

/*****************************************************************************/
/* After SIM unlock (Modem interface) */

#define MAX_AFTER_SIM_UNLOCK_RETRIES 15

typedef enum {
    CINTERION_SIM_STATUS_REMOVED        = 0,
    CINTERION_SIM_STATUS_INSERTED       = 1,
    CINTERION_SIM_STATUS_INIT_COMPLETED = 5,
} CinterionSimStatus;

typedef struct {
    guint retries;
    guint timeout_id;
} AfterSimUnlockContext;

static gboolean
after_sim_unlock_finish (MMIfaceModem  *self,
                         GAsyncResult  *res,
                         GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void after_sim_unlock_context_step (GTask *task);

static gboolean
simstatus_timeout_cb (GTask *task)
{
    AfterSimUnlockContext *ctx;

    ctx = g_task_get_task_data (task);
    ctx->timeout_id = 0;
    after_sim_unlock_context_step (task);
    return G_SOURCE_REMOVE;
}

static void
simstatus_check_ready (MMBaseModem  *self,
                       GAsyncResult *res,
                       GTask        *task)
{
    AfterSimUnlockContext *ctx;
    const gchar           *response;

    response = mm_base_modem_at_command_finish (self, res, NULL);
    if (response) {
        gchar *descr = NULL;
        guint val = 0;

        if (mm_cinterion_parse_sind_response (response, &descr, NULL, &val, NULL) &&
            g_str_equal (descr, "simstatus") &&
            val == CINTERION_SIM_STATUS_INIT_COMPLETED) {
            /* SIM ready! */
            g_free (descr);
            g_task_return_boolean (task, TRUE);
            g_object_unref (task);
            return;
        }

        g_free (descr);
    }

    /* Need to retry after 1 sec */
    ctx = g_task_get_task_data (task);
    g_assert (ctx->timeout_id == 0);
    ctx->timeout_id = g_timeout_add_seconds (1, (GSourceFunc)simstatus_timeout_cb, task);
}

static void
after_sim_unlock_context_step (GTask *task)
{
    MMBroadbandModemCinterion *self;
    AfterSimUnlockContext     *ctx;

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    if (ctx->retries == 0) {
        /* Too much wait, go on anyway */
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    /* Recheck */
    ctx->retries--;
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "^SIND=\"simstatus\",2",
                              3,
                              FALSE,
                              (GAsyncReadyCallback)simstatus_check_ready,
                              task);
}

static void
after_sim_unlock (MMIfaceModem        *self,
                  GAsyncReadyCallback  callback,
                  gpointer             user_data)
{
    GTask                 *task;
    AfterSimUnlockContext *ctx;

    task = g_task_new (self, NULL, callback, user_data);
    ctx = g_new0 (AfterSimUnlockContext, 1);
    ctx->retries = MAX_AFTER_SIM_UNLOCK_RETRIES;
    g_task_set_task_data (task, ctx, g_free);

    after_sim_unlock_context_step (task);
}

/*****************************************************************************/
/* Create Bearer (Modem interface) */

static MMBaseBearer *
cinterion_modem_create_bearer_finish (MMIfaceModem  *self,
                                      GAsyncResult  *res,
                                      GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
broadband_bearer_cinterion_new_ready (GObject      *unused,
                                      GAsyncResult *res,
                                      GTask        *task)
{
    MMBaseBearer *bearer;
    GError       *error = NULL;

    bearer = mm_broadband_bearer_cinterion_new_finish (res, &error);
    if (!bearer)
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, bearer, g_object_unref);
    g_object_unref (task);
}

static void
broadband_bearer_new_ready (GObject      *unused,
                            GAsyncResult *res,
                            GTask        *task)
{
    MMBaseBearer *bearer;
    GError       *error = NULL;

    bearer = mm_broadband_bearer_new_finish (res, &error);
    if (!bearer)
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, bearer, g_object_unref);
    g_object_unref (task);
}

static void
common_create_bearer (GTask *task)
{
    MMBroadbandModemCinterion *self;

    self = g_task_get_source_object (task);

    switch (self->priv->swwan_support) {
    case FEATURE_NOT_SUPPORTED:
        mm_dbg ("^SWWAN not supported, creating default bearer...");
        mm_broadband_bearer_new (MM_BROADBAND_MODEM (self),
                                 g_task_get_task_data (task),
                                 NULL, /* cancellable */
                                 (GAsyncReadyCallback)broadband_bearer_new_ready,
                                 task);
        return;
    case FEATURE_SUPPORTED:
        mm_dbg ("^SWWAN supported, creating cinterion bearer...");
        mm_broadband_bearer_cinterion_new (MM_BROADBAND_MODEM_CINTERION (self),
                                           g_task_get_task_data (task),
                                           NULL, /* cancellable */
                                           (GAsyncReadyCallback)broadband_bearer_cinterion_new_ready,
                                           task);
        return;
    default:
        g_assert_not_reached ();
    }
}

static void
swwan_test_ready (MMBaseModem  *_self,
                  GAsyncResult *res,
                  GTask        *task)
{
    MMBroadbandModemCinterion *self = MM_BROADBAND_MODEM_CINTERION (_self);

    /* Fetch the result to the SWWAN test. If no response given (error triggered),
     * assume unsupported */
    if (!mm_base_modem_at_command_finish (_self, res, NULL)) {
        mm_dbg ("SWWAN unsupported");
        self->priv->swwan_support = FEATURE_NOT_SUPPORTED;
    } else {
        mm_dbg ("SWWAN supported");
        self->priv->swwan_support = FEATURE_SUPPORTED;
    }

    /* Go on and create the bearer */
    common_create_bearer (task);
}

static void
cinterion_modem_create_bearer (MMIfaceModem        *_self,
                               MMBearerProperties  *properties,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
    MMBroadbandModemCinterion *self = MM_BROADBAND_MODEM_CINTERION (_self);
    GTask                     *task;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, g_object_ref (properties), g_object_unref);

    /* Newer Cinterion modems may support SWWAN, which is the same as WWAN.
     * Check to see if current modem supports it.*/
    if (self->priv->swwan_support != FEATURE_SUPPORT_UNKNOWN) {
        common_create_bearer (task);
        return;
    }

    /* If we don't have a data port, don't even bother checking for ^SWWAN
     * support. */
    if (!mm_base_modem_peek_best_data_port (MM_BASE_MODEM (self), MM_PORT_TYPE_NET)) {
        mm_dbg ("skipping ^SWWAN check as no data port is available");
        self->priv->swwan_support = FEATURE_NOT_SUPPORTED;
        common_create_bearer (task);
        return;
    }

    mm_dbg ("checking ^SWWAN support...");
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "^SWWAN=?",
                              6,
                              TRUE, /* may be cached */
                              (GAsyncReadyCallback) swwan_test_ready,
                              task);
}

/*****************************************************************************/

MMBroadbandModemCinterion *
mm_broadband_modem_cinterion_new (const gchar *device,
                                  const gchar **drivers,
                                  const gchar *plugin,
                                  guint16 vendor_id,
                                  guint16 product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_CINTERION,
                         MM_BASE_MODEM_DEVICE, device,
                         MM_BASE_MODEM_DRIVERS, drivers,
                         MM_BASE_MODEM_PLUGIN, plugin,
                         MM_BASE_MODEM_VENDOR_ID, vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         NULL);
}

static void
mm_broadband_modem_cinterion_init (MMBroadbandModemCinterion *self)
{
    /* Initialize private data */
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE ((self),
                                              MM_TYPE_BROADBAND_MODEM_CINTERION,
                                              MMBroadbandModemCinterionPrivate);

    /* Initialize private variables */
    self->priv->sind_psinfo_support = FEATURE_SUPPORT_UNKNOWN;
    self->priv->swwan_support       = FEATURE_SUPPORT_UNKNOWN;

    self->priv->ciev_psinfo_regex = g_regex_new ("\\r\\n\\+CIEV: psinfo,(\\d+)\\r\\n",
                                                 G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
}

static void
finalize (GObject *object)
{
    MMBroadbandModemCinterion *self = MM_BROADBAND_MODEM_CINTERION (object);

    g_free (self->priv->sleep_mode_cmd);
    g_free (self->priv->manual_operator_id);

    if (self->priv->cnmi_supported_mode)
        g_array_unref (self->priv->cnmi_supported_mode);
    if (self->priv->cnmi_supported_mt)
        g_array_unref (self->priv->cnmi_supported_mt);
    if (self->priv->cnmi_supported_bm)
        g_array_unref (self->priv->cnmi_supported_bm);
    if (self->priv->cnmi_supported_ds)
        g_array_unref (self->priv->cnmi_supported_ds);
    if (self->priv->cnmi_supported_bfr)
        g_array_unref (self->priv->cnmi_supported_bfr);

    g_regex_unref (self->priv->ciev_psinfo_regex);

    G_OBJECT_CLASS (mm_broadband_modem_cinterion_parent_class)->finalize (object);
}

static void
iface_modem_init (MMIfaceModem *iface)
{
    iface_modem_parent = g_type_interface_peek_parent (iface);

    iface->create_bearer = cinterion_modem_create_bearer;
    iface->create_bearer_finish = cinterion_modem_create_bearer_finish;
    iface->load_supported_modes = load_supported_modes;
    iface->load_supported_modes_finish = load_supported_modes_finish;
    iface->set_current_modes = set_current_modes;
    iface->set_current_modes_finish = set_current_modes_finish;
    iface->load_supported_bands = load_supported_bands;
    iface->load_supported_bands_finish = load_supported_bands_finish;
    iface->load_current_bands = load_current_bands;
    iface->load_current_bands_finish = load_current_bands_finish;
    iface->set_current_bands = set_current_bands;
    iface->set_current_bands_finish = set_current_bands_finish;
    iface->load_access_technologies = load_access_technologies;
    iface->load_access_technologies_finish = load_access_technologies_finish;
    iface->setup_flow_control = setup_flow_control;
    iface->setup_flow_control_finish = setup_flow_control_finish;
    iface->modem_after_sim_unlock = after_sim_unlock;
    iface->modem_after_sim_unlock_finish = after_sim_unlock_finish;
    iface->load_unlock_retries = load_unlock_retries;
    iface->load_unlock_retries_finish = load_unlock_retries_finish;
    iface->modem_power_down = modem_power_down;
    iface->modem_power_down_finish = modem_power_down_finish;
    iface->modem_power_off = modem_power_off;
    iface->modem_power_off_finish = modem_power_off_finish;
}

static void
iface_modem_3gpp_init (MMIfaceModem3gpp *iface)
{
    iface_modem_3gpp_parent = g_type_interface_peek_parent (iface);

    iface->enable_unsolicited_events = modem_3gpp_enable_unsolicited_events;
    iface->enable_unsolicited_events_finish = modem_3gpp_enable_unsolicited_events_finish;
    iface->disable_unsolicited_events = modem_3gpp_disable_unsolicited_events;
    iface->disable_unsolicited_events_finish = modem_3gpp_disable_unsolicited_events_finish;

    iface->setup_unsolicited_events = modem_3gpp_setup_unsolicited_events;
    iface->setup_unsolicited_events_finish = modem_3gpp_setup_cleanup_unsolicited_events_finish;
    iface->cleanup_unsolicited_events = modem_3gpp_cleanup_unsolicited_events;
    iface->cleanup_unsolicited_events_finish = modem_3gpp_setup_cleanup_unsolicited_events_finish;

    iface->register_in_network = register_in_network;
    iface->register_in_network_finish = register_in_network_finish;
}

static void
iface_modem_messaging_init (MMIfaceModemMessaging *iface)
{
    iface->check_support = messaging_check_support;
    iface->check_support_finish = messaging_check_support_finish;
    iface->enable_unsolicited_events = messaging_enable_unsolicited_events;
    iface->enable_unsolicited_events_finish = messaging_enable_unsolicited_events_finish;
}

static void
iface_modem_location_init (MMIfaceModemLocation *iface)
{
    mm_common_cinterion_peek_parent_location_interface (iface);

    iface->load_capabilities = mm_common_cinterion_location_load_capabilities;
    iface->load_capabilities_finish = mm_common_cinterion_location_load_capabilities_finish;
    iface->enable_location_gathering = mm_common_cinterion_enable_location_gathering;
    iface->enable_location_gathering_finish = mm_common_cinterion_enable_location_gathering_finish;
    iface->disable_location_gathering = mm_common_cinterion_disable_location_gathering;
    iface->disable_location_gathering_finish = mm_common_cinterion_disable_location_gathering_finish;
}

static void
mm_broadband_modem_cinterion_class_init (MMBroadbandModemCinterionClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMBroadbandModemCinterionPrivate));

    /* Virtual methods */
    object_class->finalize = finalize;
}
