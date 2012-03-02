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
#include "mm-log.h"

static void iface_modem_init (MMIfaceModem *iface);
static void iface_modem_3gpp_init (MMIfaceModem3gpp *iface);

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemSamsung, mm_broadband_modem_samsung, MM_TYPE_BROADBAND_MODEM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_3GPP, iface_modem_3gpp_init));

/*****************************************************************************/

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
        g_simple_async_result_complete_in_idle (operation_result);
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
    g_simple_async_result_complete_in_idle (operation_result);
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
}

static void
mm_broadband_modem_samsung_class_init (MMBroadbandModemSamsungClass *klass)
{
}
