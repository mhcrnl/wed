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

#include <stdlib.h>
#include <string.h>
#include "list.h"

static int grow_required(List *);
static int shrink_required(List *);
static int resize_list(List *, int);

/* Code in this file doesn't use alloc and ralloc from
 * util.c in order to make it easier to reuse elsewhere */

List *new_list()
{
    List* list = new_sized_list(LIST_ALLOC);        
    list->size = 0;
    return list;
}

List *new_sized_list(size_t size)
{
    List *list = calloc(1, sizeof(List));

    if (list == NULL) {
        return NULL;
    }

    list->size = size;
    list->allocated = size;
    list->values = calloc(1, sizeof(void *) * list->allocated);

    if (list->values == NULL) {
        free(list);
        return NULL;
    }

    return list; 
}

static int grow_required(List *list)
{
    return list->size == list->allocated;
}

static int shrink_required(List *list)
{
    return list->size < (list->allocated / 2);
}

static int resize_list(List *list, int resize_type)
{
    /* In case an empty list is created */
    if (resize_type == LIST_EXPAND && list->size < 2) {
        list->allocated++;
    }

    size_t new_size = list->allocated + ((list->allocated / 2) * resize_type);
    list->allocated = new_size;
    list->values = realloc(list->values, sizeof(void *) * list->allocated);

    if (list->values == NULL) {
        return 0;
    }

    if (resize_type == LIST_EXPAND) {
        /* Zero out the new part of the lists memory */
        memset(list->values + list->size, 0, sizeof(void *) * (list->allocated - list->size));
    }

    return 1;
}

size_t list_size(List *list)
{
    return list->size;
}

void *list_get(List *list, size_t index)
{
    void *value = NULL;

    if (index < list->size) {
        value = list->values[index];
    }

    return value;
}

void list_set(List *list, void *value, size_t index)
{
    if (index < list->size) {
        list->values[index] = value;
    }
}

int list_add(List *list, void *value)
{
    if (grow_required(list) && !resize_list(list, LIST_EXPAND)) {
        return 0;
    }

    list->values[list->size++] = value;    

    return 1;
}

int list_add_at(List *list, void *value, size_t index)
{
    if (index >= list->size) {
        return 0;
    }

    if (grow_required(list) && !resize_list(list, LIST_EXPAND)) {
        return 0;
    }

    for (size_t k = list->size++; k > index; k--) {
        list->values[k] = list->values[k - 1];
    }

    list->values[index] = value;

    return 1;
}

void *list_pop(List *list)
{
    void *value = NULL;

    if (list->size > 0) {
        value = list->values[--list->size];

        if (shrink_required(list) && !resize_list(list, LIST_SHRINK)) {
            return NULL;
        }
    }

    return value;    
}

void *list_remove_at(List *list, size_t index)
{
    void *value = NULL;

    if (list->size && index < list->size) {
        value = list->values[index];

        for (size_t k = index; k < list->size; k++) {
            list->values[k] = list->values[k + 1];   
        }

        list->size--;

        if (shrink_required(list) && !resize_list(list, LIST_SHRINK)) {
            return NULL;
        }
    }

    return value;
}

void list_clear(List *list)
{
    memset(list->values, 0, sizeof(void *) * list->allocated);
    list->size = 0;
}

void free_list(List *list)
{
    if (list) {
        free(list->values);
        free(list);
    } 
}
