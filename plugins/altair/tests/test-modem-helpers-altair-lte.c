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
 * Copyright (C) 2013 Google Inc.
 *
 */

#include <stdarg.h>
#include <stdio.h>
#include <glib.h>
#include <glib-object.h>
#include <locale.h>

#include "mm-modem-helpers-altair-lte.h"

/*****************************************************************************/
/* Test +CEER responses */

typedef struct {
    const gchar *str;
    const gchar *result;
} CeerTest;

static const CeerTest ceer_tests[] = {
    { "", "" }, /* Special case, sometimes the response is empty, treat it as a valid response. */
    { "+CEER:", "" },
    { "+CEER: EPS_AND_NON_EPS_SERVICES_NOT_ALLOWED", "EPS_AND_NON_EPS_SERVICES_NOT_ALLOWED" },
    { "+CEER: NO_SUITABLE_CELLS_IN_TRACKING_AREA", "NO_SUITABLE_CELLS_IN_TRACKING_AREA" },
    { "WRONG RESPONSE", NULL },
    { NULL, NULL }
};

static void
test_ceer (void)
{
    guint i;

    for (i = 0; ceer_tests[i].str; ++i) {
        GError *error = NULL;
        gchar *result;

        result = mm_altair_parse_ceer_response (ceer_tests[i].str, &error);
        if (ceer_tests[i].result) {
            g_assert (g_strcmp0 (ceer_tests[i].result, result) == 0);
            g_assert (error == NULL);
            g_free (result);
        }
        else {
            g_assert (result == NULL);
            g_assert (error != NULL);
            g_error_free (error);
        }
    }
}

int main (int argc, char **argv)
{
    setlocale (LC_ALL, "");

    g_type_init ();
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/MM/altair/ceer", test_ceer);

    return g_test_run ();
}
