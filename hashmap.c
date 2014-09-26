/*
 * Copyright (C) 2014 Richard Burke
 * Except murmurhash2
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

#include "wed.h"
#include <stdlib.h>
#include <string.h>
#include "hashmap.h"

#define HM_BUCKET_NUM_BLOCK 100
#define HM_SEED 24842118
#define HM_MAX_LOAD_FACTOR 0.75

static HashMapNode *new_hashmapnode(char *, uint32_t, void *);
static HashMapNode *get_bucket(HashMap *, char *, uint32_t *, size_t *);
static int resize_required(HashMap *);
static int resize_hashmap(HashMap *, size_t);
static void free_hashmapnodes(HashMap *);
static void free_hashmapnode(HashMapNode *);

/* Code in this file doesn't use alloc and ralloc from
 * util.c in order to make it easier to reuse elsewhere */

HashMap *new_hashmap(void)
{
    return new_sized_hashmap(HM_BUCKET_NUM_BLOCK);
}

HashMap *new_sized_hashmap(size_t size)
{
    if (size == 0) {
        return NULL;
    }

    HashMap *hashmap = malloc(sizeof(HashMap));

    if (hashmap == NULL) {
        return NULL;
    }

    hashmap->buckets = new_sized_list(size);

    if (hashmap->buckets == NULL) {
        free(hashmap);
        return NULL;
    }

    hashmap->bucket_num = size;
    hashmap->entry_num = 0;

    return hashmap;
}

static HashMapNode *new_hashmapnode(char *key, uint32_t hash, void *value)
{
    HashMapNode *node = malloc(sizeof(HashMapNode));

    if (node == NULL) {
        return NULL;
    }

    node->key = strdup(key);
    node->hash = hash;
    node->value = value;
    node->next = NULL;

    return node;
}

/* MurmurHash2 was written by Austin Appleby, and is placed in the public
   domain. The author (Austin Appleby) disclaims copyright to the source code
   of the murmurhash2 function. */
uint32_t murmurhash2(const void *key, int len, uint32_t seed)
{
    const uint32_t m = 0x5bd1e995;
    const int r = 24;

    uint32_t h = seed ^ len;

    const unsigned char * data = (const unsigned char *)key;

    while (len >= 4) {
        uint32_t k = *(uint32_t *)data;

        k *= m;
        k ^= k >> r;
        k *= m;

        h *= m;
        h ^= k;

        data += 4;
        len -= 4;
    }

    switch (len) {
        case 3: h ^= data[2] << 16;
        case 2: h ^= data[1] << 8;
        case 1: h ^= data[0]; h *= m;
    }

    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;

    return h;
}

static HashMapNode *get_bucket(HashMap *hashmap, char *key, uint32_t *hash, size_t *index)
{
    *hash = murmurhash2(key, strlen(key), HM_SEED);
    *index = *hash % hashmap->bucket_num;
    HashMapNode *node = list_get(hashmap->buckets, *index);

    if (node == NULL) {
        return NULL;
    }

    while (node != NULL) {
        if (strcmp(node->key, key) == 0) {
            return node;
        }

        node = node->next;
    }

    return NULL;
}

int hashmap_set(HashMap *hashmap, char *key, void *value)
{
    uint32_t hash;
    size_t index;
    HashMapNode *node = get_bucket(hashmap, key, &hash, &index);

    if (node != NULL) {
        node->value = value;
        return 1;
    }

    if ((node = new_hashmapnode(key, hash, value)) == NULL) {
        return 0;
    }

    node->next = list_get(hashmap->buckets, index);
    list_set(hashmap->buckets, node, index);

    hashmap->entry_num++;

    if (resize_required(hashmap)) {
        resize_hashmap(hashmap, hashmap->bucket_num * 2); 
    }

    return 1;
}

void *hashmap_get(HashMap *hashmap, char *key)
{
    uint32_t hash;
    size_t index;
    HashMapNode *node = get_bucket(hashmap, key, &hash, &index);

    if (node == NULL) {
        return NULL;
    }

    return node->value;
}

int hashmap_delete(HashMap *hashmap, char *key)
{
    uint32_t hash;
    size_t index;
    HashMapNode *node = get_bucket(hashmap, key, &hash, &index);

    if (node == NULL) {
        return 0;
    }

    HashMapNode *first = list_get(hashmap->buckets, index);

    if (node == first) {
        list_set(hashmap->buckets, node->next, index);
    } else {
        while (first->next != node) {
            first = first->next;
        }

        first->next = node->next;
    }

    free_hashmapnode(node);

    hashmap->entry_num--;

    return 1;
}

void hashmap_clear(HashMap *hashmap)
{
    free_hashmapnodes(hashmap);
    list_clear(hashmap->buckets);
    hashmap->entry_num = 0;
}

size_t hashmap_size(HashMap *hashmap)
{
    return hashmap->entry_num;
}

char **hashmap_get_keys(HashMap *hashmap)
{
    char **keys = malloc(sizeof(char *) * hashmap->entry_num);

    if (keys == NULL) {
        return NULL;
    }

    size_t index = 0;
    HashMapNode *node;

    for (size_t k = 0; k < hashmap->bucket_num; k++) {
        node = list_get(hashmap->buckets, k);

        while (node != NULL) {
            keys[index++] = node->key;
            node = node->next;
        } 
    }

    return keys;
}

/* Only checks to see if hashmap bucket number should be increased */
static int resize_required(HashMap *hashmap)
{
    double load_factor = (double)hashmap->entry_num / hashmap->bucket_num;

    return load_factor > HM_MAX_LOAD_FACTOR;
}

static int resize_hashmap(HashMap *hashmap, size_t size)
{
    List *buckets = new_sized_list(size); 

    if (buckets == NULL) {
        return 0;
    }

    size_t index;
    HashMapNode *node, *next;

    for (size_t k = 0; k < hashmap->bucket_num; k++) {
        node = list_get(hashmap->buckets, k);

        while (node != NULL) {
            next = node->next;
            index = node->hash % size;
            node->next = list_get(buckets, index);
            list_set(buckets, node, index);
            node = next;
        } 
    }

    free_list(hashmap->buckets);

    hashmap->buckets = buckets;
    hashmap->bucket_num = size;

    return 1;
}

void free_hashmap(HashMap *hashmap)
{
    if (hashmap == NULL) {
        return;
    }

    free_hashmapnodes(hashmap);
    free_list(hashmap->buckets);
    free(hashmap);
}

static void free_hashmapnodes(HashMap *hashmap)
{
    HashMapNode *node, *next;

    for (size_t k = 0; k < hashmap->bucket_num; k++) {
        node = list_get(hashmap->buckets, k);

        while (node != NULL) {
            next = node->next;
            free_hashmapnode(node);
            node = next;
        } 
    }
}

static void free_hashmapnode(HashMapNode *node)
{
    if (node == NULL) {
        return;
    }

    free(node->key);
    free(node);
}
