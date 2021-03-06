/*
 * Copyright (C) 2015 Richard Burke
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

#ifndef WED_SEARCH_H
#define WED_SEARCH_H

#include "shared.h"
#include "status.h"
#include "buffer_pos.h"
#include "text_search.h"
#include "regex_search.h"
#include "search_util.h"

/* Limit the number of matches that can be found when bs_find_all is called.
 * A static limit avoids the need to repeatedly allocate memory and also acts
 * as a guard against searches which generate a large number of matches */
#define MAX_SEARCH_MATCH_NUM 1000

typedef enum {
    BST_TEXT,
    BST_REGEX
} BufferSearchType;

/* Store search matches */
typedef struct {
    Range match_ranges[MAX_SEARCH_MATCH_NUM]; /* Array of matches */
    size_t match_num; /* Number of matches in positions array */
    size_t current_match_index; /* The current match displayed */
} SearchMatches;

/* Search structure which abstracts text
 * and regex searches */
struct BufferSearch {
    SearchOptions opt; /* Case sensitivity, direction, etc ... */
    BufferPos start_pos; /* Search starting position. line_no = 0 if not set */
    BufferPos last_match_pos; /* Last match position. line_no = 0 if no match */
    BufferSearchType search_type; /* Current search type */
    BufferSearchType last_search_type; /* Last search type */
    int advance_from_last_match; /* If searching from same pos as the previous
                                    match pos then advance the search pos */
    /* The two fields below are only used when start_pos is specified
     * i.e. During find a replace when the search has an explicit end */
    int wrapped; /* Has search wrapped */
    int finished; /* Has entire buffer been searched */
    int invalid; /* Set to true when a search has been marked invalid
                    due to changed options e.g. case sensitivity has been
                    toggled. When true the search will need to be 
                    reinitialised */
    /* Searches are either text or regex based. The structures in the 
     * union below contain the search type specific data */
    union {
        TextSearch text;
        RegexSearch regex;
    } type;
    /* When bs_find_all is called matches are stored in this structure */
    SearchMatches matches;
};

typedef struct BufferSearch BufferSearch;

Status bs_init(BufferSearch *, const BufferPos *start_pos,
               const char *pattern, size_t pattern_len);
Status bs_reinit(BufferSearch *, const BufferPos *start_pos,
                 const char *pattern, size_t pattern_len);
void bs_reset(BufferSearch *, const BufferPos *start_pos);
Status bs_init_default_opt(BufferSearch *);
void bs_free(BufferSearch *);
Status bs_find_next(BufferSearch *, const BufferPos *start_pos,
                    int *found_match);
size_t bs_match_length(const BufferSearch *);
Status bs_find_all(BufferSearch *, const BufferPos *current_pos);

#endif
