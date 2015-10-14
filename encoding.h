/*
 * Copyright (C) 2014 Richard Burke
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifndef WED_ENCODING_H
#define WED_ENCODING_H

#include <stddef.h>
#include <assert.h>
#include "shared.h"
#include "hashmap.h"

struct BufferPos;

typedef enum {
    FF_UNIX,
    FF_WINDOWS
} FileFormat;

typedef enum {
    CIP_DEFAULT,
    CIP_SCREEN_LENGTH   
} CharInfoProperties;

typedef struct {
    int is_valid;
    size_t byte_length;
    size_t screen_length;
    int is_printable;
} CharInfo;

void en_utf8_char_info(CharInfo *, CharInfoProperties, const struct BufferPos *, const HashMap *);
size_t en_utf8_previous_char_offset(const struct BufferPos *);

#endif
