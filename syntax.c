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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "syntax.h"
#include "regex_util.h"
#include "util.h"

static void sy_add_match(SyntaxMatches *, const SyntaxMatch *);
static int sy_match_cmp(const void *, const void *);

int sy_str_to_token(SyntaxToken *token, const char *token_str)
{
    static const char *syn_tokens[] = {
        [ST_NORMAL]     = "normal",
        [ST_COMMENT]    = "comment",
        [ST_CONSTANT]   = "constant",
        [ST_SPECIAL]    = "special",
        [ST_IDENTIFIER] = "identifier",
        [ST_STATEMENT]  = "statement",
        [ST_TYPE]       = "type",
        [ST_ERROR]      = "error",
        [ST_TODO]       = "todo"
    };

    for (size_t k = 0; k < ST_ENTRY_NUM; k++) {
        if (strcmp(syn_tokens[k], token_str) == 0) {
            *token = k;
            return 1;
        }
    }

    return 0;
}

Status sy_new_pattern(SyntaxPattern **syn_pattern_ptr, const Regex *regex, SyntaxToken token)
{
    assert(syn_pattern_ptr != NULL);
    assert(regex != NULL);
    assert(!is_null_or_empty(regex->regex_pattern));

    SyntaxPattern *syn_pattern = malloc(sizeof(SyntaxPattern));

    if (syn_pattern == NULL) {
        return st_get_error(ERR_OUT_OF_MEMORY, "Out Of Memory - "
                            "Unable to allocate SyntaxPattern");
    }

    memset(syn_pattern, 0, sizeof(SyntaxPattern));

    Status status = re_compile_custom_error_msg(&syn_pattern->regex, regex, 
                                                "pattern ");

    if (!STATUS_IS_SUCCESS(status)) {
        free(syn_pattern);
        return status; 
    }

    syn_pattern->token = token;
    *syn_pattern_ptr = syn_pattern;

    return STATUS_SUCCESS;
}

void syn_free_pattern(SyntaxPattern *syn_pattern)
{
    if (syn_pattern == NULL) {
        return;
    }

    re_free_instance(&syn_pattern->regex);
    free(syn_pattern);
}

SyntaxDefinition *sy_new_def(SyntaxPattern *patterns)
{
    assert(patterns != NULL);

    SyntaxDefinition *syn_def = malloc(sizeof(SyntaxDefinition));

    if (syn_def == NULL) {
        return NULL;
    }

    syn_def->patterns = patterns;

    return syn_def;
}

void sy_free_def(SyntaxDefinition *syn_def)
{
    if (syn_def == NULL) {
        return;
    }

    SyntaxPattern *next;

    while (syn_def->patterns != NULL) {
        next = syn_def->patterns->next;
        syn_free_pattern(syn_def->patterns);
        syn_def->patterns = next;
    }

    free(syn_def);
}

SyntaxMatches *sy_get_syntax_matches(const SyntaxDefinition *syn_def, 
                                     const char *str, size_t str_len,
                                     size_t offset)
{
    if (str_len == 0) {
        return NULL;
    }

    SyntaxMatches *syn_matches = malloc(sizeof(SyntaxMatches));

    if (syn_matches == NULL) {
        return NULL;
    }

    syn_matches->match_num = 0;
    syn_matches->current_match = 0;
    syn_matches->offset = offset;
    
    SyntaxPattern *pattern = syn_def->patterns;
    SyntaxMatch syn_match;
    RegexResult result;
    Status status;

    while (pattern != NULL) {
        size_t offset = 0;

        while (syn_matches->match_num < MAX_SYNTAX_MATCH_NUM &&
               offset < str_len) {
            status = re_exec(&result, &pattern->regex, str, str_len, offset);

            if (!(STATUS_IS_SUCCESS(status) && result.match)) {
                break;
            }

            syn_match.offset = result.output_vector[0];
            syn_match.length = result.match_length;
            syn_match.token = pattern->token;

            sy_add_match(syn_matches, &syn_match);

            offset = result.output_vector[0] + result.match_length;
        } 

        pattern = pattern->next;
    }

    qsort(syn_matches->matches, syn_matches->match_num, sizeof(SyntaxMatch), sy_match_cmp);

    return syn_matches;
}

static void sy_add_match(SyntaxMatches *syn_matches, const SyntaxMatch *syn_match)
{
    if (syn_matches->match_num == 0) {
        syn_matches->matches[syn_matches->match_num++] = *syn_match;
        return;
    }

    for (size_t k = 0; k < syn_matches->match_num; k++) {
        if (syn_match->offset >= syn_matches->matches[k].offset &&
            syn_match->offset < syn_matches->matches[k].offset + 
                                syn_matches->matches[k].length) {
            return; 
        }
    }

    syn_matches->matches[syn_matches->match_num++] = *syn_match;
}

static int sy_match_cmp(const void *v1, const void *v2)
{
    const SyntaxMatch *m1 = (const SyntaxMatch *)v1;
    const SyntaxMatch *m2 = (const SyntaxMatch *)v2;

    if (m1->offset == m2->offset) {
        return (int)m2->length - (int)m1->length;
    }

    return (int)m1->offset - (int)m2->offset;
}

const SyntaxMatch *sy_get_syntax_match(SyntaxMatches *syn_matches, size_t offset)
{
    if (syn_matches == NULL || syn_matches->match_num == 0 ||
        syn_matches->offset > offset) {
        return NULL;
    }

    offset -= syn_matches->offset;

    while (syn_matches->current_match < syn_matches->match_num) {
        const SyntaxMatch *syn_match = &syn_matches->matches[syn_matches->current_match];

        if (offset < syn_match->offset) {
            break;
        } else if (offset < syn_match->offset + syn_match->length) {
            return syn_match;
        }

        syn_matches->current_match++;
    }

    return NULL;
}