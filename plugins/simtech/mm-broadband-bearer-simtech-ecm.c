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
 * Copyright (C) 2022 Disruptive Technologies Research AS
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-broadband-bearer-simtech-ecm.h"
#include "mm-base-modem-at.h"
#include "mm-log-object.h"
#include "mm-modem-helpers.h"

G_DEFINE_TYPE (MMBroadbandBearerSimtechEcm, mm_broadband_bearer_simtech_ecm, MM_TYPE_BROADBAND_BEARER)
static void dial_3gpp_context_step (GTask *task);

/*****************************************************************************/
/* 3GPP Dialing (sub-step of the 3GPP Connection sequence) */

typedef enum {
    DIAL_3GPP_STEP_FIRST,
    DIAL_3GPP_STEP_LPWA_MODE_CHECK,
    DIAL_3GPP_STEP_PS_ATTACH,
    DIAL_3GPP_STEP_AUTHENTICATE,
    DIAL_3GPP_STEP_CONNECT,
    DIAL_3GPP_STEP_LAST
} Dial3gppStep;

typedef struct {
    MMBaseModem *modem;
    MMPortSerialAt *primary;
    guint cid;
    MMPort *data;
    Dial3gppStep step;
} Dial3gppContext;

static void
dial_3gpp_context_free (Dial3gppContext *ctx)
{
    if (ctx->data)
        g_object_unref (ctx->data);
    g_object_unref (ctx->primary);
    g_object_unref (ctx->modem);
    g_slice_free (Dial3gppContext, ctx);
}

static MMPort *
dial_3gpp_finish (MMBroadbandBearer *self,
                  GAsyncResult *res,
                  GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
cnmp_ready (MMBaseModem  *modem,
            GAsyncResult *res,
            GTask        *task)
{
    Dial3gppContext *ctx;
    GError *error = NULL;

    ctx = g_task_get_task_data (task);
    if (!mm_base_modem_at_command_finish (modem, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Go on */
    ctx->step++;
    dial_3gpp_context_step (task);
}

static void
cnmp_query_ready (MMBaseModem  *modem,
                  GAsyncResult *res,
                  GTask        *task)
{
    Dial3gppContext *ctx;
    guint            modepref = 0;
    const gchar     *response, *p;
    GError          *error = NULL;

    ctx = g_task_get_task_data (task);
    response = mm_base_modem_at_command_finish (modem, res, &error);
    if (!response) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    p = mm_strip_tag (response, "+CNMP:");
    if (!p) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    modepref = atoi (p);
    if (modepref != 38) {
        /* set lpwa preferred mode */
        mm_base_modem_at_command (modem,
                                  "+CNMP=38",
                                  10,
                                  FALSE, /* allow_cached */
                                  (GAsyncReadyCallback) cnmp_ready,
                                  task);
        return;
    }
    /* Go on */
    ctx->step++;
    dial_3gpp_context_step (task);
}

static void
cgatt_ready (MMBaseModem  *modem,
             GAsyncResult *res,
             GTask        *task)
{
    Dial3gppContext *ctx;
    GError *error = NULL;

    ctx = g_task_get_task_data (task);
    if (!mm_base_modem_at_command_finish (modem, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Go on */
    ctx->step++;
    dial_3gpp_context_step (task);
}

static void
auth_ready (MMBaseModem  *modem,
            GAsyncResult *res,
            GTask        *task)
{
    Dial3gppContext *ctx;
    GError *error = NULL;

    ctx = g_task_get_task_data (task);
    if (!mm_base_modem_at_command_finish (modem, res, &error)) {
        mm_obj_dbg (modem, "ECM Bearer: authentication failed: %s", error->message);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Go on */
    ctx->step++;
    dial_3gpp_context_step (task);
}

static void
cgact_ready (MMBaseModem *modem,
             GAsyncResult *res,
             GTask *task)
{
    Dial3gppContext *ctx;
    GError *error = NULL;

    ctx = g_task_get_task_data (task);
    if (!mm_base_modem_at_command_finish (modem, res, &error)) {
        mm_obj_dbg (modem, "ECM Bearer: activation of PDP context failed: %s", error->message);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Go on */
    ctx->step++;
    dial_3gpp_context_step (task);
}

static void
dial_3gpp_context_step (GTask *task)
{
    MMBearerAllowedAuth          allowed_auth;
    MMBroadbandBearerSimtechEcm *self;
    Dial3gppContext             *ctx;
    MMBearerIpFamily             ip_family;
    const gchar                 *apn;
    const gchar                 *user;
    const gchar                 *password;
    gchar                       *command;
    gchar                       *quoted_apn;
    gchar                       *quoted_user;
    gchar                       *quoted_password;
    guint                        auth = 0;
    guint                        ip_type = 0;

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    if (g_task_return_error_if_cancelled (task)) {
        g_object_unref (task);
        return;
    }

    if (ctx->cid != 1) {
        mm_obj_dbg (self, "ECM bearer: configured for ctx id=1, but provided for ctx id=%u", ctx->cid);
        g_task_return_new_error (task,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_UNSUPPORTED,
                                 "ECM bearer: Unsupported ctx id (%u)",
                                 ctx->cid);
        g_object_unref (task);
        return;
    }

    switch (ctx->step) {
    case DIAL_3GPP_STEP_FIRST:
        ctx->step++;
        /* fall through */

    case DIAL_3GPP_STEP_LPWA_MODE_CHECK:
        mm_base_modem_at_command (ctx->modem,
                                  "+CNMP?",
                                  10,
                                  FALSE, /* allow_cached */
                                  (GAsyncReadyCallback) cnmp_query_ready,
                                  task);
        return;

    case DIAL_3GPP_STEP_PS_ATTACH:
        mm_base_modem_at_command (ctx->modem,
                                  "+CGATT=1",
                                  10,
                                  FALSE, /* allow_cached */
                                  (GAsyncReadyCallback) cgatt_ready,
                                  task);
        return;

    case DIAL_3GPP_STEP_AUTHENTICATE:
        apn = mm_bearer_properties_get_apn (mm_base_bearer_peek_config (MM_BASE_BEARER (self)));
        user = mm_bearer_properties_get_user (mm_base_bearer_peek_config (MM_BASE_BEARER (self)));
        password = mm_bearer_properties_get_password (mm_base_bearer_peek_config (MM_BASE_BEARER (self)));

        if (apn && user && password) {
            ip_family = mm_bearer_properties_get_ip_type (mm_base_bearer_peek_config (MM_BASE_BEARER (self)));
            if (ip_family ==  MM_BEARER_IP_FAMILY_IPV4)
                ip_type = 1;
            else if (ip_family ==  MM_BEARER_IP_FAMILY_IPV6)
                ip_type = 2;
            else if (ip_family ==  MM_BEARER_IP_FAMILY_IPV4V6)
                ip_type = 0;
            else {
                g_task_return_new_error (task,
                                         MM_CORE_ERROR,
                                         MM_CORE_ERROR_UNSUPPORTED,
                                         "ECM bearer: Unsupported MMBearerIpFamily (%u)",
                                         ip_family);
                g_object_unref (task);
                return;
            }

            allowed_auth = mm_bearer_properties_get_allowed_auth (mm_base_bearer_peek_config (MM_BASE_BEARER (self)));
            if (allowed_auth == MM_BEARER_ALLOWED_AUTH_NONE) {
                mm_obj_dbg (self, "ECM bearer: not using authentication");
                auth = 0;
            } else if (allowed_auth & MM_BEARER_ALLOWED_AUTH_CHAP) {
                mm_obj_dbg (self, "ECM bearer: using CHAP authentication method");
                auth = 2;
            } else if (allowed_auth & MM_BEARER_ALLOWED_AUTH_PAP) {
                mm_obj_dbg (self, "ECM bearer: using PAP authentication method");
                auth = 1;
            } else {
                g_task_return_new_error (task,
                                         MM_CORE_ERROR,
                                         MM_CORE_ERROR_UNSUPPORTED,
                                         "ECM bearer: Unsupported authentication method (%u)",
                                         ip_family);
                g_object_unref (task);
                return;
            }
            quoted_apn = mm_port_serial_at_quote_string (apn);
            quoted_user = mm_port_serial_at_quote_string (user);
            quoted_password = mm_port_serial_at_quote_string (password);
            command = g_strdup_printf ("+CNCFG=%u,%u,%s,%s,%s,%u",
                                       ctx->cid,
                                       ip_type,
                                       quoted_apn,
                                       quoted_password,
                                       quoted_user,
                                       auth);
            g_free (quoted_apn);
            g_free (quoted_user);
            g_free (quoted_password);
            mm_base_modem_at_command (ctx->modem,
                                      command,
                                      10,
                                      FALSE, /* allow_cached */
                                      (GAsyncReadyCallback) auth_ready,
                                      task);
            g_free (command);
            return;
        }else {
            mm_obj_dbg (self, "ECM bearer: not using authentication");
        }
        ctx->step++;
        /* fall through */

    case DIAL_3GPP_STEP_CONNECT:
        /* We need a net data port */
        ctx->data = mm_base_modem_get_best_data_port (ctx->modem, MM_PORT_TYPE_NET);
        if (!ctx->data) {
            g_task_return_new_error (task,
                                    MM_CORE_ERROR,
                                    MM_CORE_ERROR_NOT_FOUND,
                                    "ECM bearer: No valid data port found to launch connection");
            g_object_unref (task);
            return;
        }

        command = g_strdup_printf ("+CGACT=1,%d", ctx->cid);
        mm_base_modem_at_command (ctx->modem,
                                  command,
                                  MM_BASE_BEARER_DEFAULT_CONNECTION_TIMEOUT,
                                  FALSE, /* allow_cached */
                                  (GAsyncReadyCallback) cgact_ready,
                                  task);
        g_free (command);
        return;

    case DIAL_3GPP_STEP_LAST:
        g_task_return_pointer (task,
                               g_object_ref (ctx->data),
                               g_object_unref);
        g_object_unref (task);
        return;

    default:
        g_assert_not_reached ();
    }
}

static void
dial_3gpp (MMBroadbandBearer  *self,
           MMBaseModem        *modem,
           MMPortSerialAt     *primary,
           guint               cid,
           GCancellable       *cancellable,
           GAsyncReadyCallback callback,
           gpointer            user_data)
{
    Dial3gppContext *ctx;
    GTask *task;

    g_assert (primary != NULL);

    ctx = g_slice_new0 (Dial3gppContext);
    ctx->modem = g_object_ref (modem);
    ctx->primary = g_object_ref (primary);
    ctx->cid = cid;
    ctx->step = DIAL_3GPP_STEP_FIRST;

    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify)dial_3gpp_context_free);

    dial_3gpp_context_step (task);
}

/*****************************************************************************/
/* 3GPP disconnect */

static gboolean
disconnect_3gpp_finish (MMBroadbandBearer *self,
                        GAsyncResult      *res,
                        GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
cgact_deactivate_ready (MMBaseModem  *modem,
                        GAsyncResult *res,
                        GTask        *task)
{
    GError *error = NULL;

    if (!mm_base_modem_at_command_finish (modem, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
disconnect_3gpp (MMBroadbandBearer  *self,
                 MMBroadbandModem   *modem,
                 MMPortSerialAt     *primary,
                 MMPortSerialAt     *secondary,
                 MMPort             *data,
                 guint               cid,
                 GAsyncReadyCallback callback,
                 gpointer            user_data)
{
    GTask *task;
    g_autofree gchar *cmd = NULL;

    g_assert (primary != NULL);

    task = g_task_new (self, NULL, callback, user_data);
    /* if no cids are specified, all the active contexts would be deactivated */
    mm_obj_dbg (self, "ECM bearer: deactivating PDP context (%d)", cid);
    cmd = (cid > 0 ? g_strdup_printf ("+CGACT=0,%d", cid) : g_strdup_printf ("+CGACT=0"));
    mm_base_modem_at_command (MM_BASE_MODEM (modem),
                              cmd,
                              MM_BASE_BEARER_DEFAULT_DISCONNECTION_TIMEOUT,
                              FALSE, /* allow_cached */
                              (GAsyncReadyCallback) cgact_deactivate_ready,
                              task);
    return;
}

/*****************************************************************************/

MMBaseBearer *
mm_broadband_bearer_simtech_ecm_new_finish (GAsyncResult *res,
                                            GError      **error)
{
    GObject *bearer;
    GObject *source;

    source = g_async_result_get_source_object (res);
    bearer = g_async_initable_new_finish (G_ASYNC_INITABLE (source), res, error);
    g_object_unref (source);

    if (!bearer)
        return NULL;

    /* Only export valid bearers */
    mm_base_bearer_export (MM_BASE_BEARER (bearer));

    return MM_BASE_BEARER (bearer);
}

void
mm_broadband_bearer_simtech_ecm_new (MMBroadbandModemSimtech *modem,
                                     MMBearerProperties      *config,
                                     GCancellable            *cancellable,
                                     GAsyncReadyCallback      callback,
                                     gpointer                 user_data)
{
    g_async_initable_new_async (MM_TYPE_BROADBAND_BEARER_SIMTECH_ECM,
                                G_PRIORITY_DEFAULT,
                                cancellable,
                                callback,
                                user_data,
                                MM_BASE_BEARER_MODEM, modem,
                                MM_BASE_BEARER_CONFIG, config,
                                NULL);
}

static void
mm_broadband_bearer_simtech_ecm_init (MMBroadbandBearerSimtechEcm *self)
{
}

static void
mm_broadband_bearer_simtech_ecm_class_init (MMBroadbandBearerSimtechEcmClass *klass)
{
    MMBroadbandBearerClass *broadband_bearer_class = MM_BROADBAND_BEARER_CLASS (klass);

    broadband_bearer_class->dial_3gpp = dial_3gpp;
    broadband_bearer_class->dial_3gpp_finish = dial_3gpp_finish;
    broadband_bearer_class->disconnect_3gpp = disconnect_3gpp;
    broadband_bearer_class->disconnect_3gpp_finish = disconnect_3gpp_finish;
}
