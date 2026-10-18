/*
 * common.h
 * contains some common functions
 *
 * Copyright (c) 2026 Nikias Bassen, All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */
#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include "time64.h"

#define MAC_EPOCH 978307200

size_t dtostr(char *buf, size_t bufsize, double realval);
int num_digits_i(int64_t i);
int num_digits_u(uint64_t i);
int plist_real_to_time64(double realval, Time64_T *timev);

#endif
