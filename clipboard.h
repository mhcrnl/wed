/*
 * Copyright (C) 2016 Richard Burke
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

#ifndef WED_CLIPBOARD_H
#define WED_CLIPBOARD_H

#include "buffer.h"

typedef enum {
    CT_INTERNAL,
    CT_EXTERNAL
} ClipboardType;

typedef struct {
    ClipboardType type;
    TextSelection text_selection;
} Clipboard;

void cl_init(Clipboard *);
void cl_free(Clipboard *);
Status cl_copy(Clipboard *, Buffer *);
Status cl_paste(Clipboard *, Buffer *);
Status cl_cut(Clipboard *, Buffer *);

#endif
