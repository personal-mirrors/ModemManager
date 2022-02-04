/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * mmcli -- Control modem status & access information from the command line
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2021 Fibocom Wireless Inc.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include <string.h>

#include <glib.h>
#include <gio/gio.h>

#define _LIBMM_INSIDE_MMCLI
#include <libmm-glib.h>

#include "mmcli.h"
#include "mmcli-common.h"
#include "mmcli-output.h"

/* Context */
typedef struct {
    MMManager *manager;
    GCancellable *cancellable;
    MMObject *object;
    MMModemRf *modem_rf;
} Context;
static Context *ctx;

/* Options */
static gboolean status_flag;
static gboolean rf_enable_flag;
static gboolean rf_disable_flag;
static gboolean get_rf_info_flag;

static GOptionEntry entries[] = {
    { "rf-status", 0, 0, G_OPTION_ARG_NONE, &status_flag,
      "Current status of the SAR",
      NULL
    },
    { "rf-enable-rf-info", 0, 0, G_OPTION_ARG_NONE, &rf_enable_flag,
      "Enable RF info notification",
      NULL
    },
    { "rf-disable-rf-info", 0, 0, G_OPTION_ARG_NONE, &rf_disable_flag,
      "Disable RF info notification",
      NULL
    },
    { "get-rf-info", 0, 0, G_OPTION_ARG_NONE, &get_rf_info_flag,
      "Get RF info",
      NULL
    },
    { NULL }
};

GOptionGroup *
mmcli_modem_rf_get_option_group (void)
{
    GOptionGroup *group;

    group = g_option_group_new ("rf",
                                "RF options:",
                                "Show RF options",
                                NULL,
                                NULL);
    g_option_group_add_entries (group, entries);

    return group;
}

gboolean
mmcli_modem_rf_options_enabled (void)
{
    static guint n_actions = 0;
    static gboolean checked = FALSE;

    if (checked)
        return !!n_actions;

    n_actions = (status_flag +
                 rf_enable_flag +
                 rf_disable_flag +
                 get_rf_info_flag);

    if (n_actions > 1) {
        g_printerr ("error: too many RF actions requested\n");
        exit (EXIT_FAILURE);
    }

    if (status_flag)
        mmcli_force_sync_operation ();

    checked = TRUE;
    return !!n_actions;
}

static void
context_free (void)
{
    if (!ctx)
        return;

    if (ctx->cancellable)
        g_object_unref (ctx->cancellable);
    if (ctx->modem_rf)
        g_object_unref (ctx->modem_rf);
    if (ctx->object)
        g_object_unref (ctx->object);
    if (ctx->manager)
        g_object_unref (ctx->manager);
    g_free (ctx);
}

static void
ensure_modem_rf (void)
{
    if (!ctx->modem_rf) {
        g_printerr ("error: modem has no RF capabilities\n");
        exit (EXIT_FAILURE);
    }

    /* Success */
}

void
mmcli_modem_rf_shutdown (void)
{
    context_free ();
}

static void
print_rf_status (void)
{
    GList        *rf_info;

    rf_info = mm_modem_rf_get_rf_inf(ctx->modem_rf);

    if (!rf_info) {
        g_printerr ("error: couldn't get RF info: \n");
        exit (EXIT_FAILURE);
    }

    mmcli_output_rf_info (rf_info);
    mmcli_output_dump ();
}


static void
enable_rf_info_process_reply (gboolean      result,
                              const GError *error)
{
    if (!result) {
        g_printerr ("error: couldn't enable RF info: '%s'\n",
                    error ? error->message : "unknown error");
        exit (EXIT_FAILURE);
    }

    g_print ("Successfully enabled RF info\n");
}

static void
disable_rf_info_process_reply (gboolean      result,
                       const GError *error)
{
    if (!result) {
        g_printerr ("error: couldn't disable RF info: '%s'\n",
                    error ? error->message : "unknown error");
        exit (EXIT_FAILURE);
    }

    g_print ("Successfully disabled RF info\n");
}

static void
get_rf_info_process_reply (gboolean      result,
                       const GError *error)
{
    if (!result) {
        g_printerr ("error: couldn't get RF info: '%s'\n",
                    error ? error->message : "unknown error");
        exit (EXIT_FAILURE);
    }

    g_print ("Successfully requested RF info\n");
}

static void
enable_rf_info_ready (MMModemRf   *modem,
                      GAsyncResult *result)
{
    gboolean res;
    GError *error = NULL;

    res = mm_modem_rf_setup_rf_info_finish(modem, result, &error);
    enable_rf_info_process_reply (res, error);

    mmcli_async_operation_done ();
}

static void
disable_rf_info_ready (MMModemRf   *modem,
                       GAsyncResult *result)
{
    gboolean res;
    GError *error = NULL;

    res = mm_modem_rf_setup_rf_info_finish (modem, result, &error);
    disable_rf_info_process_reply (res, error);

    mmcli_async_operation_done ();
}

static void
get_rf_info_ready (MMModemRf   *modem,
                   GAsyncResult *result)
{
    gboolean res;
    GError *error = NULL;

    res = mm_modem_rf_get_rf_info_finish (modem, result, &error);
    get_rf_info_process_reply (res, error);

    mmcli_async_operation_done ();
}

static void
get_modem_ready (GObject      *source,
                 GAsyncResult *result)
{
    ctx->object = mmcli_get_modem_finish (result, &ctx->manager);
    ctx->modem_rf = mm_object_get_modem_rf (ctx->object);

    /* Setup operation timeout */
    if (ctx->modem_rf)
        mmcli_force_operation_timeout (G_DBUS_PROXY (ctx->modem_rf));

    ensure_modem_rf ();

    g_assert (!status_flag);

    /* Request to enable RF info */
    if (rf_enable_flag) {
        g_debug ("Asynchronously enabling RF info...");
        mm_modem_rf_setup_rf_info (ctx->modem_rf,
                                   TRUE,
                                   ctx->cancellable,
                                   (GAsyncReadyCallback)enable_rf_info_ready,
                                   NULL);
        return;
    }

    /* Request to disable RF info */
    if (rf_disable_flag) {
        g_debug ("Asynchronously disabling RF info...");
        mm_modem_rf_setup_rf_info (ctx->modem_rf,
                                   FALSE,
                                   ctx->cancellable,
                                   (GAsyncReadyCallback)disable_rf_info_ready,
                                   NULL);
        return;
    }

    /* Request to get RF info */
    if (get_rf_info_flag) {
        g_debug ("Asynchronously request RF info...");
        mm_modem_rf_get_rf_info (ctx->modem_rf,
                                 ctx->cancellable,
                                 (GAsyncReadyCallback)get_rf_info_ready,
                                 NULL);
        return;
    }

    g_warn_if_reached ();
}

void
mmcli_modem_rf_run_asynchronous (GDBusConnection *connection,
                                 GCancellable    *cancellable)
{
    /* Initialize context */
    ctx = g_new0 (Context, 1);
    if (cancellable)
        ctx->cancellable = g_object_ref (cancellable);

    /* Get proper modem */
    mmcli_get_modem (connection,
                     mmcli_get_common_modem_string (),
                     cancellable,
                     (GAsyncReadyCallback)get_modem_ready,
                     NULL);
}

void
mmcli_modem_rf_run_synchronous (GDBusConnection *connection)
{
    GError *error = NULL;

    /* Initialize context */
    ctx = g_new0 (Context, 1);
    ctx->object = mmcli_get_modem_sync (connection,
                                        mmcli_get_common_modem_string (),
                                        &ctx->manager);
    ctx->modem_rf = mm_object_get_modem_rf (ctx->object);

    /* Setup operation timeout */
    if (ctx->modem_rf)
        mmcli_force_operation_timeout (G_DBUS_PROXY (ctx->modem_rf));

    ensure_modem_rf ();

    /* Request to get status? */
    if (status_flag) {
        g_debug ("Printing RF status...");
        print_rf_status ();
        return;
    }

    /* Request to enable RF info */
    if (rf_enable_flag) {
        gboolean result;
        g_debug ("Synchronously enabling RF info...");
        result = mm_modem_rf_setup_rf_info_sync (ctx->modem_rf,
                                                 TRUE,
                                                 ctx->cancellable,
                                                 NULL);
        enable_rf_info_process_reply (result, error);
        return;
    }

    /* Request to disable RF info */
    if (rf_disable_flag) {
        gboolean result;
        g_debug ("Synchronously disabling RF info...");
        result = mm_modem_rf_setup_rf_info_sync (ctx->modem_rf,
                                                 FALSE,
                                                 ctx->cancellable,
                                                 NULL);
        disable_rf_info_process_reply (result, error);
        return;
    }

    /* Request to get RF info */
    if (get_rf_info_flag) {
        gboolean result;
        g_debug ("Synchronously request RF info...");
        result = mm_modem_rf_get_rf_info_sync (ctx->modem_rf,
                                               ctx->cancellable,
                                               NULL);
        get_rf_info_process_reply (result, error);
        return;
    }

    g_warn_if_reached ();
}

