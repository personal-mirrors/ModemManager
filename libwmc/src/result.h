/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * Copyright (C) 2010 Red Hat, Inc.
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBWMC_RESULT_H
#define LIBWMC_RESULT_H

#include <stdint.h>

typedef struct WmcResult WmcResult;

int wmc_result_get_string (WmcResult *r,
                           const char *key,
                           const char **out_val);

int wmc_result_get_u8     (WmcResult *r,
                           const char *key,
                           uint8_t *out_val);

int wmc_result_get_u32    (WmcResult *r,
                           const char *key,
                           uint32_t *out_val);

WmcResult *wmc_result_ref     (WmcResult *r);

void       wmc_result_unref   (WmcResult *r);

#endif  /* LIBWMC_RESULT_H */
