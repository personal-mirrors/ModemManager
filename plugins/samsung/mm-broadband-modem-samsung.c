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
 *
 * Copyright (C) 2012 Google Inc.
 * Author: Nathan Williams <njw@google.com>
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "ModemManager.h"
#include "mm-base-modem-at.h"
#include "mm-broadband-bearer-samsung.h"
#include "mm-broadband-modem-samsung.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-modem-helpers.h"
#include "mm-log.h"

static void iface_modem_init (MMIfaceModem *iface);
static void iface_modem_3gpp_init (MMIfaceModem3gpp *iface);

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemSamsung, mm_broadband_modem_samsung, MM_TYPE_BROADBAND_MODEM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_3GPP, iface_modem_3gpp_init));

/*****************************************************************************/

static gboolean
load_access_technologies_finish (MMIfaceModem *self,
                                 GAsyncResult *res,
                                 MMModemAccessTechnology *access_technologies,
                                 guint *mask,
                                 GError **error)
{
    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return FALSE;

    *access_technologies = (MMModemAccessTechnology) GPOINTER_TO_UINT (
        g_simple_async_result_get_op_res_gpointer (
            G_SIMPLE_ASYNC_RESULT (res)));
    *mask = MM_MODEM_ACCESS_TECHNOLOGY_ANY;
    return TRUE;
}

static MMModemAccessTechnology
nwstate_to_act (const char *str)
{
    /* small 'g' means CS, big 'G' means PS */
    if (!strcmp (str, "2g"))
        return MM_MODEM_ACCESS_TECHNOLOGY_GSM;
    else if (!strcmp (str, "2G-GPRS"))
        return MM_MODEM_ACCESS_TECHNOLOGY_GPRS;
    else if (!strcmp (str, "2G-EDGE"))
        return MM_MODEM_ACCESS_TECHNOLOGY_EDGE;
    else if (!strcmp (str, "3G"))
        return MM_MODEM_ACCESS_TECHNOLOGY_UMTS;
    else if (!strcmp (str, "3g"))
        return MM_MODEM_ACCESS_TECHNOLOGY_UMTS;
    else if (!strcmp (str, "R99"))
        return MM_MODEM_ACCESS_TECHNOLOGY_UMTS;
    else if (!strcmp (str, "3G-HSDPA") || !strcmp (str, "HSDPA"))
        return MM_MODEM_ACCESS_TECHNOLOGY_HSDPA;
    else if (!strcmp (str, "3G-HSUPA") || !strcmp (str, "HSUPA"))
        return MM_MODEM_ACCESS_TECHNOLOGY_HSUPA;
    else if (!strcmp (str, "3G-HSDPA-HSUPA") || !strcmp (str, "HSDPA-HSUPA"))
        return MM_MODEM_ACCESS_TECHNOLOGY_HSPA;

    return MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
}

static void
load_access_technologies_ready (MMIfaceModem *self,
                                GAsyncResult *res,
                                GSimpleAsyncResult *operation_result)
{
    GRegex *regex;
    GMatchInfo *info;
    gchar *str;
    const gchar *response;
    MMModemAccessTechnology act;
    GError *error = NULL;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error);
    if (!response) {
        mm_dbg ("Couldn't query access technology: '%s'", error->message);
        g_simple_async_result_take_error (operation_result, error);
        g_simple_async_result_complete (operation_result);
        g_object_unref (operation_result);
        return;
    }

    /*
     * %NWSTATE: <rssi>,<mccmnc>,<tech>,<connection state>,<regulation>
     *
     * <connection state> shows the actual access technology in-use when a
     * PS connection is active.
     */
    regex = g_regex_new (
        "%NWSTATE:\\s*(-?\\d+),(\\d+),([^,]*),([^,]*),(\\d+)",
        G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);

    act = MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
    g_regex_match (regex, response, 0, &info);
    if (g_match_info_matches (info)) {
        str = g_match_info_fetch (info, 4);
        if (!str || (strcmp (str, "-") == 0)) {
            g_free (str);
            str = g_match_info_fetch (info, 3);
        }
        if (str) {
            act = nwstate_to_act (str);
            g_free (str);
        }
    }
    g_match_info_free (info);
    g_regex_unref (regex);

    g_simple_async_result_set_op_res_gpointer (operation_result,
                                               GUINT_TO_POINTER (act),
                                               NULL);
    g_simple_async_result_complete_in_idle (operation_result);
    g_object_unref (operation_result);
}

static void
load_access_technologies (MMIfaceModem *self,
                          GAsyncReadyCallback callback,
                          gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        load_access_technologies);

    mm_base_modem_at_command (
        MM_BASE_MODEM (self),
        "%NWSTATE",
        3,
        FALSE,
        NULL, /* cancellable */
        (GAsyncReadyCallback)load_access_technologies_ready,
        result);
}

typedef struct {
    MMModemBand mm;
    char band[50];
} BandTable;

static BandTable modem_bands[12] = {
    /* Sort 3G first since it's preferred */
    { MM_MODEM_BAND_U2100, "FDD_BAND_I" },
    { MM_MODEM_BAND_U1900, "FDD_BAND_II" },
    { MM_MODEM_BAND_U1800, "FDD_BAND_III" },
    { MM_MODEM_BAND_U17IV, "FDD_BAND_IV" },
    { MM_MODEM_BAND_U850,  "FDD_BAND_V" },
    { MM_MODEM_BAND_U800,  "FDD_BAND_VI" },
    { MM_MODEM_BAND_U900,  "FDD_BAND_VIII" },
    { MM_MODEM_BAND_G850,  "G850" },
    /* 2G second */
    { MM_MODEM_BAND_DCS,   "DCS" },
    { MM_MODEM_BAND_EGSM,  "EGSM" },
    { MM_MODEM_BAND_PCS,   "PCS" },
    /* And ANY last since it's most inclusive */
    { MM_MODEM_BAND_ANY,   "ANY" },
};

static GArray *
load_supported_bands_finish (MMIfaceModem *self,
                             GAsyncResult *res,
                             GError **error)
{
    /* Never fails */
    return (GArray *) g_array_ref (g_simple_async_result_get_op_res_gpointer (
                                       G_SIMPLE_ASYNC_RESULT (res)));
}

static void
load_supported_bands (MMIfaceModem *self,
                      GAsyncReadyCallback callback,
                      gpointer user_data)
{
    GSimpleAsyncResult *result;
    GArray *bands;
    guint i;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        load_supported_bands);

    /*
     * The modem doesn't support telling us what bands are supported;
     * list everything we know about.
     */
    bands = g_array_sized_new (FALSE, FALSE, sizeof (MMModemBand), G_N_ELEMENTS (modem_bands));
    for (i = 0 ; i < G_N_ELEMENTS (modem_bands) ; i++) {
        if (modem_bands[i].mm != MM_MODEM_BAND_ANY)
            g_array_append_val(bands, modem_bands[i].mm);
    }

    g_simple_async_result_set_op_res_gpointer (result,
                                               bands,
                                               (GDestroyNotify)g_array_unref);
    g_simple_async_result_complete_in_idle (result);
    g_object_unref (result);
}

static GArray *
load_current_bands_finish (MMIfaceModem *self,
                           GAsyncResult *res,
                           GError **error)
{
    /* Never fails */
    return (GArray *) g_array_ref (g_simple_async_result_get_op_res_gpointer (
                                       G_SIMPLE_ASYNC_RESULT (res)));
}

static void
load_current_bands_ready (MMIfaceModem *self,
                          GAsyncResult *res,
                          GSimpleAsyncResult *operation_result)
{
    GRegex *r;
    GMatchInfo *info;
    GArray *bands;
    const gchar *response;
    GError *error;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error);
    if (!response) {
        mm_dbg ("Couldn't query supported bands: '%s'", error->message);
        g_simple_async_result_take_error (operation_result, error);
        g_simple_async_result_complete (operation_result);
        g_object_unref (operation_result);
        return;
    }

    /*
     * Response is a number of lines of the form:
     *   "EGSM": 0
     *   "FDD_BAND_I": 1
     *   ...
     * with 1 and 0 indicating whether the particular band is enabled or not.
     */
    r = g_regex_new ("^\"(\\w+)\": (\\d)",
                     G_REGEX_MULTILINE, G_REGEX_MATCH_NEWLINE_ANY,
                     NULL);
    g_assert (r != NULL);

    bands = g_array_sized_new (FALSE, FALSE, sizeof (MMModemBand),
                               G_N_ELEMENTS (modem_bands));

    g_regex_match (r, response, 0, &info);
    while (g_match_info_matches (info)) {
        gchar *band, *enabled;

        band = g_match_info_fetch (info, 1);
        enabled = g_match_info_fetch (info, 2);
        if (enabled[0] == '1') {
            guint i;
            for (i = 0 ; i < G_N_ELEMENTS (modem_bands); i++) {
                if (!strcmp (band, modem_bands[i].band)) {
                    g_array_append_val(bands, modem_bands[i].mm);
                    break;
                }
            }
        }
        g_free (band);
        g_free (enabled);
        g_match_info_next (info, NULL);
    }
    g_match_info_free (info);
    g_regex_unref (r);

    g_simple_async_result_set_op_res_gpointer (operation_result,
                                               bands,
                                               (GDestroyNotify)g_array_unref);
    g_simple_async_result_complete (operation_result);
    g_object_unref (operation_result);
}

static void
load_current_bands (MMIfaceModem *self,
                    GAsyncReadyCallback callback,
                    gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        load_current_bands);
    mm_base_modem_at_command (
        MM_BASE_MODEM (self),
        "%IPBM?",
        3,
        FALSE,
        NULL,
        (GAsyncReadyCallback)load_current_bands_ready,
        result);
}

static MMModemMode
load_supported_modes_finish (MMIfaceModem *self,
                             GAsyncResult *res,
                             GError **error)
{
    return (MMModemMode) GPOINTER_TO_UINT (g_simple_async_result_get_op_res_gpointer (
                                               G_SIMPLE_ASYNC_RESULT (res)));
}

static void
load_supported_modes (MMIfaceModem *self,
                      GAsyncReadyCallback callback,
                      gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        load_supported_modes);

    g_simple_async_result_set_op_res_gpointer (result,
                                               GUINT_TO_POINTER (MM_MODEM_MODE_2G |
                                                                 MM_MODEM_MODE_3G),
                                               NULL);
    g_simple_async_result_complete_in_idle (result);
    g_object_unref (result);
}

typedef struct {
    MMModemMode allowed;
    MMModemMode preferred;
} ModePair;

static gboolean
load_allowed_modes_finish (MMIfaceModem *self,
                           GAsyncResult *res,
                           MMModemMode *allowed,
                           MMModemMode *preferred,
                           GError **error)
{
    ModePair *pair;

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return FALSE;

    pair = g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res));
    *allowed = pair->allowed;
    *preferred = pair->preferred;
    return TRUE;
}

static void
load_allowed_modes_ready (MMIfaceModem *self,
                          GAsyncResult *res,
                          GSimpleAsyncResult *operation_result)
{
    const gchar *response;
    GError *error = NULL;
    MMModemMode allowed, preferred;
    int mode, domain;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error);
    if (!response) {
        g_simple_async_result_take_error (operation_result, error);
        g_simple_async_result_complete (operation_result);
        g_object_unref (operation_result);
        return;
    }

    allowed = preferred = MM_MODEM_MODE_NONE;
    response = mm_strip_tag (response, "%IPSYS:");

    if (sscanf (response, " %d,%d", &mode, &domain)) {
        switch (mode) {
            case 0:
                allowed = preferred = MM_MODEM_MODE_2G;
                break;
            case 1:
                allowed = preferred = MM_MODEM_MODE_3G;
                break;
            case 2:
                allowed = MM_MODEM_MODE_2G | MM_MODEM_MODE_3G;
                preferred = MM_MODEM_MODE_2G;
                break;
            case 3:
                allowed = MM_MODEM_MODE_2G | MM_MODEM_MODE_3G;
                preferred = MM_MODEM_MODE_3G;
                break;
            case 5:
                allowed = MM_MODEM_MODE_2G | MM_MODEM_MODE_3G;
                break;
        }
    }

    if (allowed == MM_MODEM_MODE_NONE) {
        g_simple_async_result_set_error (operation_result,
                                         MM_CORE_ERROR,
                                         MM_CORE_ERROR_FAILED,
                                         "Invalid supported modes response: '%s'",
                                         response);
    } else {
        ModePair *pair;

        pair = g_new0 (ModePair, 1);
        pair->allowed = allowed;
        pair->preferred = preferred;
        g_simple_async_result_set_op_res_gpointer (operation_result,
                                                   pair,
                                                   g_free);
    }
    g_simple_async_result_complete (operation_result);
    g_object_unref (operation_result);
}

static void
load_allowed_modes (MMIfaceModem *self,
                         GAsyncReadyCallback callback,
                         gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        load_allowed_modes);
    mm_base_modem_at_command (
        MM_BASE_MODEM (self),
        "%IPSYS?",
        3,
        FALSE,
        NULL,
        (GAsyncReadyCallback)load_allowed_modes_ready,
        result);
}

static gboolean
set_allowed_modes_finish (MMIfaceModem *self,
                          GAsyncResult *res,
                          GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
set_allowed_modes_ready (MMBaseModem *self,
                         GAsyncResult *res,
                         GSimpleAsyncResult *operation_result)
{
    GError *error = NULL;

    if (!mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error))
        /* Let the error be critical */
        g_simple_async_result_take_error (operation_result, error);
    else
        g_simple_async_result_set_op_res_gboolean (operation_result, TRUE);

    g_simple_async_result_complete (operation_result);
    g_object_unref (operation_result);
}

static void
set_allowed_modes (MMIfaceModem *self,
                   MMModemMode modes,
                   MMModemMode preferred,
                   GAsyncReadyCallback callback,
                   gpointer user_data)
{
    GSimpleAsyncResult *result;
    gint value;
    gchar *command;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        set_allowed_modes);

    /*
     * The core has checked the following:
     *  - that 'modes' are a subset of the allowed modes
     *  - that 'preferred' is one mode, and a subset of 'modes'
     */
    if (modes == MM_MODEM_MODE_2G)
        value = 0;
    else if (modes == MM_MODEM_MODE_3G)
        value = 1;
    else if (modes == (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G) &&
             preferred == MM_MODEM_MODE_2G)
        value = 2;
    else if (modes == (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G) &&
             preferred == MM_MODEM_MODE_3G)
        value = 3;
    else if (modes == (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G) &&
             preferred == MM_MODEM_MODE_NONE)
        value = 5;
    else {
        g_simple_async_result_set_error (result,
                                         MM_CORE_ERROR,
                                         MM_CORE_ERROR_INVALID_ARGS,
                                         "Couldn't set allowed modes, "
                                         "unsupported combination of allowed (%d) and "
                                         "preferred (%d)",
                                         modes, preferred);
        g_simple_async_result_complete_in_idle (result);
        g_object_unref (result);
        return;
    }

    command = g_strdup_printf ("%%IPSYS=%d", value);
    mm_base_modem_at_command (
        MM_BASE_MODEM (self),
        command,
        3,
        FALSE,
        NULL,
        (GAsyncReadyCallback)set_allowed_modes_ready,
        result);
    g_free (command);
}

MMBroadbandModemSamsung *
mm_broadband_modem_samsung_new (const gchar *device,
                                 const gchar *driver,
                                 const gchar *plugin,
                                 guint16 vendor_id,
                                 guint16 product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_SAMSUNG,
                         MM_BASE_MODEM_DEVICE, device,
                         MM_BASE_MODEM_DRIVER, driver,
                         MM_BASE_MODEM_PLUGIN, plugin,
                         MM_BASE_MODEM_VENDOR_ID, vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         NULL);
}

static MMBearer *
modem_create_bearer_finish (MMIfaceModem *self,
                            GAsyncResult *res,
                            GError **error)
{
    MMBearer *bearer;

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return NULL;

    bearer = g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res));

    return g_object_ref (bearer);
}

static void
broadband_bearer_new_ready (GObject *source,
                            GAsyncResult *res,
                            GSimpleAsyncResult *simple)
{
    MMBearer *bearer = NULL;
    GError *error = NULL;

    bearer = mm_broadband_bearer_samsung_new_finish (res, &error);
    if (!bearer)
        g_simple_async_result_take_error (simple, error);
    else
        g_simple_async_result_set_op_res_gpointer (simple,
                                                   bearer,
                                                   (GDestroyNotify)g_object_unref);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
modem_create_bearer (MMIfaceModem *self,
                     MMBearerProperties *properties,
                     GAsyncReadyCallback callback,
                     gpointer user_data)
{
    GSimpleAsyncResult *result;

    /* Set a new ref to the bearer object as result */
    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_create_bearer);

    mm_broadband_bearer_samsung_new (MM_BROADBAND_MODEM_SAMSUNG (self),
                                     properties,
                                     NULL, /* cancellable */
                                     (GAsyncReadyCallback)broadband_bearer_new_ready,
                                     result);
}

static void
mm_broadband_modem_samsung_init (MMBroadbandModemSamsung *self)
{
}

static void
iface_modem_3gpp_init (MMIfaceModem3gpp *iface)
{
}

static void
iface_modem_init (MMIfaceModem *iface)
{
    iface->create_bearer = modem_create_bearer;
    iface->create_bearer_finish = modem_create_bearer_finish;
    iface->load_supported_bands = load_supported_bands;
    iface->load_supported_bands_finish = load_supported_bands_finish;
    iface->load_current_bands = load_current_bands;
    iface->load_current_bands_finish = load_current_bands_finish;
    iface->load_supported_modes = load_supported_modes;
    iface->load_supported_modes_finish = load_supported_modes_finish;
    iface->load_allowed_modes = load_allowed_modes;
    iface->load_allowed_modes_finish = load_allowed_modes_finish;
    iface->set_allowed_modes = set_allowed_modes;
    iface->set_allowed_modes_finish = set_allowed_modes_finish;
    iface->load_access_technologies = load_access_technologies;
    iface->load_access_technologies_finish = load_access_technologies_finish;
}

static void
mm_broadband_modem_samsung_class_init (MMBroadbandModemSamsungClass *klass)
{
}
