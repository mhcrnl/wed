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

#include <stdio.h>
#include <string.h>
#include "tabbed_view.h"
#include "config.h"

static Status tv_update_buffer_view(TabbedView *, const Session *);
static size_t tv_determine_line_no_width(const Buffer *);
static void tv_determine_prompt_data(TabbedView *, const Session *);
static int tv_resize_buffer_view(const TabbedView *, BufferView *);
static void tv_update_buffer_tabs(TabbedView *, const Session *);
static void tv_update_status_bar(TabbedView *, Session *);
static size_t tv_status_file_info(TabbedView *, const Session *,
                                  size_t max_segment_width);
static size_t tv_status_pos_info(TabbedView *, const Session *,
                                 size_t max_segment_width);
static void tv_status_general_info(TabbedView *, Session *,
                                   size_t available_space);

void tv_init(TabbedView *tv, size_t rows, size_t cols)
{
    memset(tv, 0, sizeof(TabbedView));

    tv->rows = rows;
    tv->cols = cols;
}

void tv_free(TabbedView *tv)
{
    (void)tv;
}

Status tv_update(TabbedView *tv, Session *sess)
{
    RETURN_IF_FAIL(tv_update_buffer_view(tv, sess));
    tv_update_buffer_tabs(tv, sess);
    tv_update_status_bar(tv, sess);

    return STATUS_SUCCESS;
}

static Status tv_update_buffer_view(TabbedView *tv, const Session *sess)
{
    Buffer *buffer = sess->active_buffer;
    tv->is_prompt_active = se_prompt_active(sess);

    if (!tv->is_prompt_active && buffer != sess->error_buffer) {
        tv->last_line_no_width = tv->line_no_width;
        tv->line_no_width = tv_determine_line_no_width(buffer);
    }

    tv_determine_prompt_data(tv, sess);

    if (!tv_resize_buffer_view(tv, buffer->bv)) {
        return st_get_error(ERR_OUT_OF_MEMORY, "Unable to resize BufferView");
    }

    bv_update_view(sess, buffer);

    tv->bv = buffer->bv;
    
    return STATUS_SUCCESS;
}

static size_t tv_determine_line_no_width(const Buffer *buffer)
{
    if (!cf_bool(buffer->config, CV_LINENO)) {
        return 0;
    }

    char lineno_str[50];
    return snprintf(lineno_str, sizeof(lineno_str), "%zu ", bf_lines(buffer));
}

static void tv_determine_prompt_data(TabbedView *tv, const Session *sess)
{
    if (tv->is_prompt_active) {
        tv->prompt_text = pr_get_prompt_text(sess->prompt);
        tv->prompt_text_len = strlen(tv->prompt_text);
    } else {
        tv->prompt_text = NULL;
        tv->prompt_text_len = 0;
    }
}

static int tv_resize_buffer_view(const TabbedView *tv, BufferView *bv)
{
    size_t rows;
    size_t cols;

    if (tv->is_prompt_active) {
        rows = 1;
        cols = tv->cols - (tv->prompt_text_len + 1);
    } else {
        rows = tv->rows - 2;
        cols = tv->cols - tv->line_no_width;
    }

    return bv_resize(bv, rows, cols);
}

static void tv_update_buffer_tabs(TabbedView *tv, const Session *sess)
{
    /* Each tab has the format {Buffer Id} {Buffer Name} */
    const char *tab_fmt = " %zu %s ";
    size_t total_used_space = 0;
    size_t used_space = 0;
    Buffer *buffer;

    /* Determine which buffer tab we will list first */
    if (sess->active_buffer_index < tv->first_buffer_tab_index) {
        tv->first_buffer_tab_index = sess->active_buffer_index;
    } else {
        buffer = sess->active_buffer;
        size_t start_index = sess->active_buffer_index;

        while (1) {
            used_space = snprintf(tv->buffer_tabs[0], MAX_BUFFER_TAB_WIDTH,
                                  tab_fmt, start_index + 1,
                                  buffer->file_info.file_name);
            used_space = MIN(used_space, MAX_BUFFER_TAB_WIDTH);

            if ((total_used_space + used_space > tv->cols) ||
                start_index == 0 || 
                start_index == tv->first_buffer_tab_index) {
                break;
            }

            total_used_space += used_space;
            buffer = se_get_buffer(sess, --start_index);
        }

        if (total_used_space + used_space > tv->cols) {
            tv->first_buffer_tab_index = start_index + 1;
        }  

        total_used_space = 0;
        used_space = 0;
    }

    buffer = se_get_buffer(sess, tv->first_buffer_tab_index);
    tv->buffer_tab_num = 0;

    /* Draw as many tabs as we can fit on the screen */
    for (size_t buffer_index = tv->first_buffer_tab_index; 
         buffer_index < sess->buffer_num &&
         tv->buffer_tab_num < MAX_VISIBLE_BUFFER_TABS;
         buffer_index++) {

        used_space = snprintf(tv->buffer_tabs[tv->buffer_tab_num],
                              MAX_BUFFER_TAB_WIDTH, tab_fmt, buffer_index + 1,
                              buffer->file_info.file_name);
        used_space = MIN(used_space, MAX_BUFFER_TAB_WIDTH);

        if (total_used_space + used_space > tv->cols) {
            break;
        }

        tv->buffer_tab_num++;
        total_used_space += used_space;
        buffer = buffer->next;
    }
}

static void tv_update_status_bar(TabbedView *tv, Session *sess)
{
    /* Split the status bar into 2 or 3 segments. Then determine
     * how much we can fit in each segment */
    size_t segment_num = 2;

    if (se_has_msgs(sess)) {
        segment_num = 3;
    }

    /* Segment 1: File info e.g. file path, file name, readonly, ...
     * Segment 2: Messages e.g. "Save Success" (Only exists if messages exist)
     * Segment 3: Position info e.g. Line No, Col No, ... */

    size_t max_segment_width = MIN(tv->cols / segment_num,
                                   MAX_STATUS_BAR_SECTION_WIDTH);

    size_t file_info_size = tv_status_file_info(tv, sess, max_segment_width);
    size_t file_pos_size = tv_status_pos_info(tv, sess, max_segment_width);

    if (segment_num == 3) {
        size_t available_space = tv->cols - file_info_size -
    /* The 3 is for a "| " seperator at the start and one space at the end */
                                 file_pos_size - 3;
        tv_status_general_info(tv, sess, available_space);
    } else {
        tv->status_bar[1][0] = '\0';
    }
}

static size_t tv_status_file_info(TabbedView *tv, const Session *sess,
                                  size_t max_segment_width)
{
    size_t file_info_free = max_segment_width;
    const FileInfo *file_info = &sess->active_buffer->file_info;

    char *file_info_text = " ";

    if (!fi_file_exists(file_info)) {
        file_info_text = " [new] ";
    } else if (!fi_can_write_file(file_info)) {
        file_info_text = " [readonly] ";
    }

    file_info_free -= strlen(file_info_text);
    size_t file_info_size;
    char *file_path = NULL;

    if (fi_file_exists(file_info)) {
        file_path = file_info->abs_path;
    } else if (fi_has_file_path(file_info)) {
        file_path = file_info->rel_path;
    }

    /* If we have the full file path and there's enough room 
     * to display it then do so, otherwise display the file name */
    if (file_path == NULL || strlen(file_path) > file_info_free) {
        file_path = file_info->file_name;
    }
    
    if (strlen(file_path) > file_info_free) {
        /* Print as much of the file name as we can
         * with 3 trailing dots to indicate it's not complete */
        int file_char_num = file_info_free - 3;
        file_info_size = snprintf(tv->status_bar[0], max_segment_width,
                                  " \"%.*s...\"%s", file_char_num,
                                  file_path, file_info_text);
    } else {
        file_info_size = snprintf(tv->status_bar[0], max_segment_width,
                                  " \"%s\"%s", file_path, file_info_text);
    }

    return file_info_size;
}

static size_t tv_status_pos_info(TabbedView *tv, const Session *sess,
                                 size_t max_segment_width)
{
    const Buffer *buffer = sess->active_buffer;
    const BufferView *bv = buffer->bv;
    const BufferPos *screen_start = &bv->screen_start;
    const BufferPos *pos = &buffer->pos;
    char rel_pos[5] = { '\0' };

    size_t line_num = bf_lines(buffer);
    size_t lines_above = screen_start->line_no - 1;
    size_t lines_below;

    if ((screen_start->line_no + bv->rows - 1) >= line_num) {
        lines_below = 0; 
    } else {
        lines_below = line_num - (screen_start->line_no + bv->rows - 1);
    }

    if (lines_below == 0) {
        if (lines_above == 0) {
            strcpy(rel_pos, "All");
        } else {
            strcpy(rel_pos, "Bot");
        }
    } else if (lines_above == 0) {
        strcpy(rel_pos, "Top");
    } else {
        int pos_pct = (int)((lines_above /
                            (double)(lines_above + lines_below)) * 100);
        snprintf(rel_pos, sizeof(rel_pos), "%2d%%%%", pos_pct);
    }

    char buf_size[64];
    bytes_to_str(bf_length(buffer), buf_size, sizeof(buf_size));

    const char *file_type_name = se_get_file_type_display_name(sess, buffer);

    if (is_null_or_empty(file_type_name)) {
        file_type_name = "Plain Text";
    }

    const char *file_format =
        bf_get_fileformat(buffer) == FF_UNIX ? "LF" : "CRLF";

    /* Attempt to print as much info as space allows */

    int pos_info_size = snprintf(tv->status_bar[2],
                                 MAX_STATUS_BAR_SECTION_WIDTH,
                                 "%s | %s | %s | %zu:%zu | %s",
                                 buf_size, file_type_name, file_format,
                                 pos->line_no, pos->col_no,
                                 rel_pos);

    if (pos_info_size < 0 || (size_t)pos_info_size > max_segment_width) {
        pos_info_size = snprintf(tv->status_bar[2], max_segment_width, 
                                 "%zu:%zu ", pos->line_no, pos->col_no);
    }

    return pos_info_size;
}

static void tv_status_general_info(TabbedView *tv, Session *sess,
                                   size_t available_space)
{
    char *msg = bf_join_lines_string(sess->msg_buffer, ". ");
    se_clear_msgs(sess);

    if (msg == NULL) {
        return;
    }

    size_t msg_length = strlen(msg);

    if (msg_length > available_space) {
        /* TODO F12 functionality to view full message text not implemented */
        char *fmt = "%%.%ds... (F12 view full) |";
        char msg_fmt[MAX_STATUS_BAR_SECTION_WIDTH];
        msg_length = available_space - strlen(fmt) + 5;
        snprintf(msg_fmt, MAX_STATUS_BAR_SECTION_WIDTH, fmt, msg_length);
        snprintf(tv->status_bar[1], available_space, msg_fmt, msg);
    } else {
        snprintf(tv->status_bar[1], available_space, "%s", msg); 
    }

    free(msg);
}

void tv_resize(TabbedView *tv, size_t rows, size_t cols)
{
    tv->rows = rows;
    tv->cols = cols;
    tv->line_no_width = 0;
}
