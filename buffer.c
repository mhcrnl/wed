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

#include "wed.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "session.h"
#include "buffer.h"
#include "util.h"
#include "status.h"
#include "display.h"
#include "file.h"
#include "config.h"

static void reset_buffer(Buffer *);
static Line *add_to_buffer(const char *, size_t, Line *, int);
static int is_selection(Direction *);
static void default_movement_selection_handler(Buffer *, int, Direction *);
static Status pos_change_real_line(Buffer *, BufferPos *, Direction, int);
static Status pos_change_screen_line(Buffer *, BufferPos *, Direction, int);
static Status advance_pos_to_line_offset(Buffer *, BufferPos *, int);
static void update_line_col_offset(Buffer *, BufferPos *);
static Status delete_line_segment(Line *, size_t, size_t);

Buffer *new_buffer(FileInfo file_info)
{
    Buffer *buffer = alloc(sizeof(Buffer));

    buffer->file_info = file_info;
    init_bufferpos(&buffer->pos);
    init_bufferpos(&buffer->screen_start);
    init_bufferpos(&buffer->select_start);
    buffer->lines = NULL;
    buffer->next = NULL;
    buffer->line_col_offset = 0;
    buffer->config = new_hashmap();

    return buffer;
}

Buffer *new_empty_buffer(void)
{
    FileInfo file_info;
    init_empty_fileinfo(&file_info);
    Buffer *buffer = new_buffer(file_info); 
    buffer->lines = buffer->pos.line = buffer->screen_start.line = new_line();
    return buffer;
}

void free_buffer(Buffer *buffer)
{
    if (buffer == NULL) {
        return;
    }

    free_fileinfo(buffer->file_info);
    free_config(buffer->config);

    Line *line = buffer->lines;
    Line *tmp = NULL;

    while (line != NULL) {
        tmp = line->next;
        free_line(line);
        line = tmp;
    }

    free(buffer);
}

Line *new_line(void)
{
    return new_sized_line(0);
}

Line *new_sized_line(size_t length)
{
    size_t alloc_num = (length / LINE_ALLOC) + 1;

    Line *line = alloc(sizeof(Line));
    line->text = alloc(alloc_num * LINE_ALLOC);
    line->alloc_num = alloc_num;
    line->length = 0; 
    line->screen_length = 0;
    line->is_dirty = 0;
    *line->text = '\0';
    line->next = NULL;
    line->prev = NULL;

    return line;
}

void free_line(Line *line)
{
    if (line == NULL) {
        return;
    }

    free(line->text);
    free(line);
}

int init_bufferpos(BufferPos *pos)
{
    if (pos == NULL) {
        return 0;
    }

    pos->line = NULL;
    pos->offset = 0;

    return 1;
}

TextSelection *new_textselection(Range range)
{
    TextSelection *text_selection = alloc(sizeof(TextSelection));

    if (range.start.line == range.end.line) {
        text_selection->type = TST_STRING;
        text_selection->text.string = get_line_segment(range.start.line, range.start.offset, range.end.offset);
    } else {
        text_selection->type = TST_LINE;
        Line *line = text_selection->text.lines = clone_line_segment(range.start.line, range.start.offset, range.start.line->length);
        line->prev = NULL;
        Line *prev = line;

        while ((line = line->next) != range.end.line) {
            line = clone_line(line);    
            prev->next = line;
            line->prev = prev;
            prev = line;
        }

        line = clone_line_segment(range.end.line, 0, range.end.offset); 
        prev->next = line;
        line->prev = prev;
        line->next = NULL;
    }

    return text_selection;
}

void free_textselection(TextSelection *text_selection)
{
    if (text_selection == NULL) {
        return;
    }

    if (text_selection->type == TST_STRING) {
        free(text_selection->text.string); 
    } else {
        Line *line = text_selection->text.lines;
        Line *next;

        while (line != NULL) {
            next = line->next;
            free_line(line);
            line = next;
        }
    }

    free(text_selection);
}

/* Returns deep copy of a line */
Line *clone_line(Line *line)
{
    Line *clone = alloc(sizeof(Line));
    *clone = *line;
    clone->text = alloc(line->alloc_num * LINE_ALLOC);
    memcpy(clone->text, line->text, line->length);

    return clone;
}

/* TODO When editing functionality is added this function
 * will need to expand and shrink a line.
 * Also need to consider adding a function to determine
 * how much to expand or shrink by.
 * Also pick better function name. */
void resize_line_text_if_req(Line *line, size_t new_size)
{
    if (line == NULL) {
        return;
    }

    size_t allocated = line->alloc_num * LINE_ALLOC;

    if (new_size > allocated) {
        resize_line_text(line, new_size);  
    } else if (new_size < (allocated - LINE_ALLOC)) {
        resize_line_text(line, new_size);
    }
}

void resize_line_text(Line *line, size_t new_size)
{
    if (line == NULL) {
        return;
    }

    line->alloc_num = (new_size / LINE_ALLOC) + 1;

    line->text = ralloc(line->text, line->alloc_num * LINE_ALLOC);
}

Status clear_buffer(Buffer *buffer)
{
    Line *line = buffer->lines;
    Line *next;

    reset_buffer(buffer);

    while (line != NULL) {
        next = line->next;
        free_line(line);
        line = next;
    }

    return STATUS_SUCCESS;
}

static void reset_buffer(Buffer *buffer)
{
    buffer->lines = new_line();
    buffer->pos.line = buffer->screen_start.line = buffer->lines;
    buffer->pos.offset = buffer->screen_start.offset = 0;
    buffer->line_col_offset = 0;
    select_reset(buffer);
}

/* Loads file into buffer structure */
Status load_buffer(Buffer *buffer)
{
    if (!file_exists(buffer->file_info)) {
        /* If the file represented by this buffer doesn't exist
         * then the buffer content is empty */
        buffer->lines = buffer->pos.line = buffer->screen_start.line = new_line();
        return STATUS_SUCCESS;
    }

    FILE *input_file = fopen(buffer->file_info.rel_path, "rb");

    if (input_file == NULL) {
        return raise_param_error(ERR_UNABLE_TO_OPEN_FILE, STR_VAL(buffer->file_info.file_name));
    } 

    char buf[FILE_BUF_SIZE];
    size_t read;
    Line *line = buffer->lines = new_line();

    while ((read = fread(buf, sizeof(char), FILE_BUF_SIZE, input_file)) > 0) {
        if (read != FILE_BUF_SIZE && ferror(input_file)) {
            fclose(input_file);
            return raise_param_error(ERR_UNABLE_TO_READ_FILE, STR_VAL(buffer->file_info.file_name));
        } 

        line = add_to_buffer(buf, read, line, read < FILE_BUF_SIZE);
    }

    fclose(input_file);

    line->screen_length = line_screen_length(line, 0, line->length);

    if (buffer->lines) {
        buffer->pos.line = buffer->screen_start.line = buffer->lines;
    }

    return STATUS_SUCCESS;
}

/* Used when loading a file into a buffer */
static Line *add_to_buffer(const char buffer[], size_t bsize, Line *line, int eof)
{
    size_t idx = 0;

    while (idx < bsize) {
        if (line->length > 0 && ((line->length % LINE_ALLOC) == 0)) {
            resize_line_text(line, line->length + LINE_ALLOC);
        }

        if (buffer[idx] == '\n' && !(eof && idx == (bsize - 1))) {
            line->screen_length = line_screen_length(line, 0, line->length);
            line->next = new_line();
            line->next->prev = line;
            line = line->next;
        } else {
            line->text[line->length++] = buffer[idx];
        }

        idx++;
    }

    return line;
}

/* TODO Write to temporary file then move it to the filename we want to write to */
Status write_buffer(Buffer *buffer)
{
    if (buffer == NULL || buffer->file_info.rel_path == NULL ||
        buffer->lines == NULL) {
        /* TODO Raise error */        
        return STATUS_SUCCESS;
    }

    FILE *output_file = fopen(buffer->file_info.rel_path, "wb");

    if (output_file == NULL) {
        /* TODO Raise error */        
    }

    Line *line = buffer->lines;

    while (line->next != NULL) {
        fwrite(line->text, sizeof(char), line->length, output_file);
        fputc('\n', output_file);
        line = line->next;
    }

    fwrite(line->text, sizeof(char), line->length, output_file);

    int fail = ferror(output_file);

    fclose(output_file);

    if (fail) {
        /* TODO Raise error */
    }

    return STATUS_SUCCESS;
}

size_t buffer_byte_num(Buffer *buffer)
{
    if (buffer == NULL || buffer->lines == NULL) {
        return 0;
    }

    Line *line = buffer->lines;
    size_t bytes = 0;

    while (line->next != NULL) {
        bytes += line->length + 1;
        line = line->next;
    }

    bytes += line->length;

    return bytes;
}

size_t buffer_line_num(Buffer *buffer)
{
    if (buffer == NULL || buffer->lines == NULL) {
        return 0;
    }

    Line *line = buffer->lines;
    size_t line_num = 1;

    while (line->next != NULL) {
        line_num++;
        line = line->next;
    }

    return line_num;
}

char *get_buffer_as_string(Buffer *buffer)
{
    if (buffer == NULL || buffer->lines == NULL) {
        return NULL;
    }

    size_t bytes = buffer_byte_num(buffer);
    char *str = alloc(bytes + 1);
    char *iter = str;
    Line *line = buffer->lines;

    while (line->next != NULL) {
        if (line->length > 0) {
            memcpy(iter, line->text, line->length);
            iter += line->length;
        }

        *iter++ = '\n';
        line = line->next;
    }

    if (line->length > 0) {
        memcpy(iter, line->text, line->length);
        iter += line->length;
    }

    *iter = '\0';

    return str;
}

int buffer_file_exists(Buffer *buffer)
{
    return file_exists(buffer->file_info);
}

int has_file_path(Buffer *buffer)
{
    return buffer->file_info.rel_path != NULL;
}

int set_buffer_file_path(Buffer *buffer, const char *file_path)
{
    if (file_path == NULL) {
        return 0;
    }

    return set_file_path(&buffer->file_info, file_path);
}

size_t get_pos_line_number(Buffer *buffer)
{
    return get_bufferpos_line_number(buffer->pos);
}

size_t get_bufferpos_line_number(BufferPos pos)
{
    size_t line_num = 1;
    Line *line = pos.line;

    while ((line = line->prev) != NULL) {
        line_num++;
    }

    return line_num;
}

size_t get_pos_col_number(Buffer *buffer)
{
    BufferPos pos = buffer->pos;
    size_t col_no = line_screen_length(pos.line, 0, pos.offset);
    return col_no + 1;
}

Line *get_line_from_offset(Line *line, Direction direction, size_t offset)
{
    if (offset == 0 || line == NULL) {
        return line;
    }

    Line *next;

    if (direction == DIRECTION_DOWN) {
        while ((next = line->next) != NULL && offset-- > 0) {
            line = next;
        }
    } else if (direction == DIRECTION_UP) {
        while ((next = line->prev) != NULL && offset-- > 0) {
            line = next;
        }
    }

    return line;
}

int offset_compare(size_t offset1, size_t offset2)
{
    return (offset1 < offset2 ? -1 : offset1 > offset2);
}

/* TODO This could be optimized. It's pretty heavy going as it is. */
int bufferpos_compare(BufferPos pos1, BufferPos pos2)
{
    if (pos1.line == pos2.line) {
        return offset_compare(pos1.offset, pos2.offset);
    }

    size_t pos1_line_no = get_bufferpos_line_number(pos1);
    size_t pos2_line_no = get_bufferpos_line_number(pos2);

    return (pos1_line_no < pos2_line_no ? -1 : 1);
}

BufferPos bufferpos_min(BufferPos pos1, BufferPos pos2)
{
    if (bufferpos_compare(pos1, pos2) == -1) {
        return pos1;
    }

    return pos2;
}

BufferPos bufferpos_max(BufferPos pos1, BufferPos pos2)
{
    if (bufferpos_compare(pos1, pos2) == 1) {
        return pos1;
    }

    return pos2;
}

int get_selection_range(Buffer *buffer, Range *range)
{
    if (range == NULL) {
        return 0;
    } else if (!selection_started(buffer)) {
        return 0;
    }

    range->start = bufferpos_min(buffer->pos, buffer->select_start);
    range->end = bufferpos_max(buffer->pos, buffer->select_start);

    return 1;
}

/* TODO This could also be optimized */
int bufferpos_in_range(Range range, BufferPos pos)
{
    if (bufferpos_compare(pos, range.start) < 0 || bufferpos_compare(pos, range.end) >= 0) {
        return 0;
    }

    return 1;
}

size_t range_length(Buffer *buffer, Range range)
{
    size_t length = 1;

    while (bufferpos_compare(range.start, range.end) != 0) {
        pos_change_char(buffer, &range.start, DIRECTION_RIGHT, 0);
        length++;
    }

    return length;
}

/* TODO Consider UTF-8 punctuation and whitespace */
CharacterClass character_class(const char *character)
{
    if (char_byte_length(*character) == 1) {
        if (isspace(*character)) {
            return CCLASS_WHITESPACE;
        } else if (ispunct(*character)) {
            return CCLASS_PUNCTUATION;
        }
    }

    return CCLASS_WORD;
}

const char *pos_character(Buffer *buffer)
{
    return pos_offset_character(buffer, DIRECTION_NONE, 0);
}

const char *pos_offset_character(Buffer *buffer, Direction direction, size_t offset)
{
    BufferPos pos = buffer->pos;

    if (!is_success(pos_change_multi_char(buffer, &pos, direction, offset, 0))) {
        return "";
    }

    if (pos.offset == pos.line->length) {
        return " ";
    }

    return pos.line->text + pos.offset;
}

/* start_offset is inclusive, end_offset is exclusive */
char *get_line_segment(Line *line, size_t start_offset, size_t end_offset)
{
    if ((start_offset >= line->length || end_offset <= start_offset) &&
        /* Allow empty lines */
        end_offset - start_offset != 0) {
        return NULL;
    }

    end_offset = (end_offset > line->length ? line->length : end_offset);
    size_t bytes_to_copy = end_offset - start_offset;

    char *text = alloc(bytes_to_copy + 1);

    if (bytes_to_copy > 0) {
        memcpy(text, line->text + start_offset, bytes_to_copy);
    }

    *(text + bytes_to_copy) = '\0';

    return text;
}

/* start_offset is inclusive, end_offset is exclusive */
Line *clone_line_segment(Line *line, size_t start_offset, size_t end_offset)
{
    if ((start_offset >= line->length || end_offset <= start_offset) &&
        /* Allow empty lines */
        end_offset - start_offset != 0) {
        return NULL;
    }

    end_offset = (end_offset > line->length ? line->length : end_offset);
    size_t bytes_to_copy = end_offset - start_offset;

    Line *clone = alloc(sizeof(Line));
    *clone = *line;
    clone->alloc_num = (bytes_to_copy / LINE_ALLOC) + 1;
    clone->text = alloc(clone->alloc_num * LINE_ALLOC);
    clone->length = bytes_to_copy;
    clone->screen_length = line_screen_length(line, start_offset, end_offset);

    if (bytes_to_copy > 0) {
        memcpy(clone->text, line->text + start_offset, bytes_to_copy);
    }

    return clone;
}

int bufferpos_at_line_start(BufferPos pos)
{
    return pos.offset == 0;
}

int bufferpos_at_line_end(BufferPos pos)
{
    return pos.line->length == pos.offset;
}

int bufferpos_at_first_line(BufferPos pos)
{
    return pos.line->prev == NULL;
}

int bufferpos_at_last_line(BufferPos pos)
{
    return pos.line->next == NULL;
}

int bufferpos_at_buffer_start(BufferPos pos)
{
    return bufferpos_at_first_line(pos) && bufferpos_at_line_start(pos);
}

int bufferpos_at_buffer_end(BufferPos pos)
{
    return bufferpos_at_last_line(pos) && bufferpos_at_line_end(pos);
}

int bufferpos_at_buffer_extreme(BufferPos pos)
{
    return bufferpos_at_buffer_start(pos) || bufferpos_at_buffer_end(pos);
}

int move_past_buffer_extremes(BufferPos pos, Direction direction)
{
    return ((direction == DIRECTION_LEFT && bufferpos_at_buffer_start(pos)) ||
            (direction == DIRECTION_RIGHT && bufferpos_at_buffer_end(pos)));
}

static int is_selection(Direction *direction)
{
    if (direction == NULL) {
        return 0;
    }

    int is_select = *direction & DIRECTION_WITH_SELECT;
    *direction &= ~DIRECTION_WITH_SELECT;

    return is_select;
}

int selection_started(Buffer *buffer)
{
    return buffer->select_start.line != NULL;
}

static void default_movement_selection_handler(Buffer *buffer, int is_select, Direction *direction)
{
    if (is_select) {
        if (direction != NULL) {
            *direction |= DIRECTION_WITH_SELECT;
        }

        select_continue(buffer);
        buffer->pos.line->is_dirty |= DRAW_LINE_SELECTION_CHANGE;
    } else if (selection_started(buffer)) {
        Range select_range;

        get_selection_range(buffer, &select_range);

        select_range.start.line->is_dirty |= DRAW_LINE_REFRESH_DOWN;
        select_range.end.line->is_dirty |= DRAW_LINE_END_REFRESH_DOWN;

        select_reset(buffer);
    }
}

/* TODO All cursor functions bellow need to be updated to consider a global cursor offset.
 * This would mean after moving from an (empty|shorter) line to a longer line the cursor 
 * would return to the global offset it was previously on instead of staying in the first column. */

/* Move cursor up or down a line keeping the offset into the line the same 
 * or as close to the original if possible */
Status pos_change_line(Buffer *buffer, BufferPos *pos, Direction direction, int is_cursor)
{
    if (config_bool("linewrap")) {
        return pos_change_screen_line(buffer, pos, direction, is_cursor);
    }

    return pos_change_real_line(buffer, pos, direction, is_cursor);
}

static Status pos_change_real_line(Buffer *buffer, BufferPos *pos, Direction direction, int is_cursor)
{
    int is_select = is_selection(&direction);

    if ((direction == DIRECTION_NONE) ||
        (!(direction == DIRECTION_UP || direction == DIRECTION_DOWN))) {

        return STATUS_SUCCESS;
    }

    if (is_cursor) {
        default_movement_selection_handler(buffer, is_select, NULL);
        
        if (is_select) {
            if (direction == DIRECTION_UP && pos->line->prev != NULL) {
                pos->line->prev->is_dirty |= DRAW_LINE_SELECTION_CHANGE;
            } else if (direction == DIRECTION_DOWN && pos->line->next != NULL) {
                pos->line->next->is_dirty |= DRAW_LINE_SELECTION_CHANGE;
            }
        }
    }

    if ((direction == DIRECTION_DOWN && bufferpos_at_last_line(*pos)) ||
        (direction == DIRECTION_UP && bufferpos_at_first_line(*pos))) {
        
        return STATUS_SUCCESS;
    }

    Line *line = pos->line;
    size_t current_screen_offset = line_screen_length(line, 0, pos->offset);
    size_t new_screen_offset = 0;

    pos->line = line = (direction == DIRECTION_DOWN ? line->next : line->prev);
    pos->offset = 0;

    while (pos->offset < line->length && new_screen_offset < current_screen_offset) {
        new_screen_offset += byte_screen_length(line->text[pos->offset], line, pos->offset); 
        pos->offset++;
    }

    if (is_cursor) {
        return advance_pos_to_line_offset(buffer, pos, is_select);
    }

    return STATUS_SUCCESS;
}

/* Move cursor up or down a screen line keeping the cursor column as close to the
 * starting value as possible. For lines which don't wrap this function behaves the
 * same as pos_change_line. For lines which wrap this allows a user to scroll up or
 * down to a different part of the line displayed as a different line on the screen.
 * Therefore this function is dependent on the width of the screen. */

static Status pos_change_screen_line(Buffer *buffer, BufferPos *pos, Direction direction, int is_cursor)
{
    int is_select = is_selection(&direction);

    if ((direction == DIRECTION_NONE) ||
        (!(direction == DIRECTION_UP || direction == DIRECTION_DOWN))) {

        return STATUS_SUCCESS;
    }

    Direction pos_direction = (direction == DIRECTION_DOWN ? DIRECTION_RIGHT : DIRECTION_LEFT);

    if (is_cursor) {
        default_movement_selection_handler(buffer, is_select, &pos_direction);
        
        if (is_select) {
            if (direction == DIRECTION_UP && pos->line->prev != NULL) {
                pos->line->prev->is_dirty |= DRAW_LINE_SELECTION_CHANGE;
            } else if (direction == DIRECTION_DOWN && pos->line->next != NULL) {
                pos->line->next->is_dirty |= DRAW_LINE_SELECTION_CHANGE;
            }
        }
    }

    Line *start_line = pos->line;
    size_t screen_line = line_pos_screen_height(buffer->win_info, *pos);
    size_t screen_lines = line_screen_height(buffer->win_info, pos->line);
    int break_on_hardline = screen_lines > 1 && (screen_line + DIRECTION_OFFSET(direction)) > 0 && screen_line < screen_lines;
    size_t cols, col_num;
    cols = col_num = buffer->win_info.width;
    Status status;

    while (cols > 0 && cols <= col_num) {
        cols -= byte_screen_length(pos->line->text[pos->offset], pos->line, pos->offset);
        status = pos_change_char(buffer, pos, pos_direction, 0);

        if (!is_success(status)) {
            return status;
        } else if (break_on_hardline && (pos->offset == 0 || pos->offset == pos->line->length)) {
           break; 
        } else if (pos->line != start_line) {
            if (break_on_hardline || pos->line->length == 0) {
                break;
            }

            break_on_hardline = 1;
            start_line = pos->line;

            if (direction == DIRECTION_DOWN) {
                cols -= (col_num - 1 - (pos->line->prev->screen_length % col_num));
            } else {
                cols -= (col_num - 1 - (pos->line->screen_length % col_num));
            }
        }
    }

    if (is_cursor) {
        return advance_pos_to_line_offset(buffer, pos, is_select);
    }

    return STATUS_SUCCESS;
}

static Status advance_pos_to_line_offset(Buffer *buffer, BufferPos *pos, int is_select)
{
    size_t global_col_offset = buffer->line_col_offset;
    size_t current_col_offset = screen_col_no(buffer->win_info, *pos);
    Direction direction = DIRECTION_RIGHT;
    Status status;

    if (is_select) {
        direction |= DIRECTION_WITH_SELECT;
    }

    while (current_col_offset < global_col_offset &&
           pos->offset < pos->line->length) {
        
        status = pos_change_char(buffer, pos, direction, 1);

        RETURN_IF_FAIL(status);

        current_col_offset++;
    }

    buffer->line_col_offset = global_col_offset;

    return STATUS_SUCCESS;
}

Status pos_change_multi_line(Buffer *buffer, BufferPos *pos, Direction direction, size_t offset, int is_cursor)
{
    if (offset == 0 || direction == DIRECTION_NONE) {
        return STATUS_SUCCESS;
    }

    Status status;

    for (size_t k = 0; k < offset; k++) {
        status = pos_change_line(buffer, pos, direction, is_cursor);
        RETURN_IF_FAIL(status);
    }

    return STATUS_SUCCESS;
}

/* Move cursor a character to the left or right */
Status pos_change_char(Buffer *buffer, BufferPos *pos, Direction direction, int is_cursor)
{
    int is_select = is_selection(&direction);

    if ((direction == DIRECTION_NONE) ||
        (!(direction == DIRECTION_LEFT || direction == DIRECTION_RIGHT))) {
    
        return STATUS_SUCCESS;
    }

    if (is_cursor) {
        if (is_select) {
            if (!move_past_buffer_extremes(*pos, direction)) {
                select_continue(buffer);
                pos->line->is_dirty |= DRAW_LINE_SELECTION_CHANGE;
            }
        } else if (selection_started(buffer)) {
            Range select_range;
            BufferPos new_pos;

            get_selection_range(buffer, &select_range);

            if (direction == DIRECTION_LEFT) {
                new_pos = select_range.start;
            } else {
                new_pos = select_range.end;
            }

            select_range.start.line->is_dirty |= DRAW_LINE_REFRESH_DOWN;
            select_range.end.line->is_dirty |= DRAW_LINE_END_REFRESH_DOWN;

            select_reset(buffer);

            return pos_to_bufferpos(buffer, new_pos);
        }
    }

    if (move_past_buffer_extremes(*pos, direction)) {
        return STATUS_SUCCESS;
    }

    Line *line = pos->line;

    if (pos->offset == 0 && direction == DIRECTION_LEFT) {
        pos->line = line = line->prev; 
        pos->offset = line->length == 0 ? 0 : line->length;
    } else if ((pos->offset == line->length || line->length == 0) && direction == DIRECTION_RIGHT) {
        pos->line = line = line->next; 
        pos->offset = 0;
    } else {
        pos->offset += DIRECTION_OFFSET(direction);
    }

    /* Ensure we're not on a continuation byte */
    while (!byte_screen_length(line->text[pos->offset], line, pos->offset) &&
           pos->offset < line->length &&
           (pos->offset += DIRECTION_OFFSET(direction)) > 0) ;

    if (is_cursor) {
        update_line_col_offset(buffer, pos);
    }

    return STATUS_SUCCESS;
}

Status pos_change_multi_char(Buffer *buffer, BufferPos *pos, Direction direction, size_t offset, int is_cursor)
{
    if (offset == 0 || direction == DIRECTION_NONE) {
        return STATUS_SUCCESS;
    }

    Status status;

    for (size_t k = 0; k < offset; k++) {
        status = pos_change_char(buffer, pos, direction, is_cursor);
        RETURN_IF_FAIL(status);
    }

    return STATUS_SUCCESS;
}

static void update_line_col_offset(Buffer *buffer, BufferPos *pos)
{
    buffer->line_col_offset = screen_col_no(buffer->win_info, *pos);
}

Status pos_to_line_start(Buffer *buffer, int is_select)
{
    Direction direction = DIRECTION_LEFT;
    default_movement_selection_handler(buffer, is_select, &direction);

    BufferPos *pos = &buffer->pos;

    if (pos->offset == 0) {
        return STATUS_SUCCESS;
    } else if (!config_bool("linewrap")) {
        pos->offset = 0;
        return STATUS_SUCCESS;
    }

    size_t screen_width = buffer->win_info.width;
    size_t col_index;
    Status status;

    do {
        status = pos_change_char(buffer, pos, direction, 1);

        RETURN_IF_FAIL(status);

        col_index = screen_col_no(buffer->win_info, *pos);
    } while (pos->offset > 0 && (col_index % screen_width) != 0) ;

    return STATUS_SUCCESS;
}

Status pos_to_line_end(Buffer *buffer, int is_select)
{
    Direction direction = DIRECTION_RIGHT;
    default_movement_selection_handler(buffer, is_select, &direction);

    BufferPos *pos = &buffer->pos;

    if (pos->offset == pos->line->length) {
        return STATUS_SUCCESS;
    } else if (!config_bool("linewrap")) {
        pos->offset = pos->line->length;
        return STATUS_SUCCESS;
    }

    size_t screen_width = buffer->win_info.width;
    size_t col_index;
    Status status;

    do {
        status = pos_change_char(buffer, pos, direction, 1);

        RETURN_IF_FAIL(status);

        col_index = screen_col_no(buffer->win_info, *pos);
    } while (pos->offset != pos->line->length && (col_index % screen_width) != (screen_width - 1)) ;

    return STATUS_SUCCESS;
}

Status pos_to_next_word(Buffer *buffer, int is_select)
{
    Direction direction = DIRECTION_RIGHT;
    default_movement_selection_handler(buffer, is_select, &direction);

    BufferPos *pos = &buffer->pos;
    Status status;

    CharacterClass start_class = character_class(pos_character(buffer));

    do {
        status = pos_change_char(buffer, pos, direction, 1);
        RETURN_IF_FAIL(status);
    } while (!bufferpos_at_buffer_end(buffer->pos) &&
             start_class == character_class(pos_character(buffer)));

    while (!bufferpos_at_buffer_extreme(buffer->pos) &&
           character_class(pos_character(buffer)) == CCLASS_WHITESPACE) {

        if (bufferpos_at_line_end(buffer->pos)) {
            break;
        }

        status = pos_change_char(buffer, pos, direction, 1);
        RETURN_IF_FAIL(status);
    }

    return STATUS_SUCCESS;
}

Status pos_to_prev_word(Buffer *buffer, int is_select)
{
    Direction direction = DIRECTION_LEFT;
    default_movement_selection_handler(buffer, is_select, &direction);

    BufferPos *pos = &buffer->pos;
    Status status;

    do {
        status = pos_change_char(buffer, pos, direction, 1);
        RETURN_IF_FAIL(status);
    } while (!bufferpos_at_buffer_start(buffer->pos) &&
             character_class(pos_character(buffer)) == CCLASS_WHITESPACE);

    CharacterClass start_class = character_class(pos_character(buffer));

    while (!bufferpos_at_buffer_start(buffer->pos) &&
           start_class == character_class(pos_offset_character(buffer, DIRECTION_LEFT, 1))) {

        status = pos_change_char(buffer, pos, direction, 1);
        RETURN_IF_FAIL(status);
    }

    return STATUS_SUCCESS;
}

Status pos_to_buffer_start(Buffer *buffer, int is_select)
{
    default_movement_selection_handler(buffer, is_select, NULL);

    BufferPos *pos = &buffer->pos;
    pos->line = buffer->lines;
    pos->offset = 0;

    if (is_select) {
        pos->line->is_dirty |= DRAW_LINE_REFRESH_DOWN;
    }

    return STATUS_SUCCESS;
}

Status pos_to_buffer_end(Buffer *buffer, int is_select)
{
    BufferPos *pos = &buffer->pos;

    default_movement_selection_handler(buffer, is_select, NULL);
    pos->line->is_dirty |= DRAW_LINE_REFRESH_DOWN;

    Line *next;

    while ((next = pos->line->next) != NULL) {
        pos->line = next;
    }

    pos->offset = pos->line->length;

    return STATUS_SUCCESS;
}

Status pos_to_bufferpos(Buffer *buffer, BufferPos pos)
{
    buffer->pos = pos; 
    return STATUS_SUCCESS;
}

Status pos_change_page(Buffer *buffer, Direction direction)
{
    int is_select = is_selection(&direction);

    if (bufferpos_at_first_line(buffer->pos) && direction == DIRECTION_UP) {
        return STATUS_SUCCESS;
    }

    default_movement_selection_handler(buffer, is_select, &direction);

    BufferPos *pos = &buffer->pos;
    Status status = pos_change_multi_line(buffer, pos, direction, buffer->win_info.height - 1, 1);

    RETURN_IF_FAIL(status);

    if (buffer->screen_start.line != buffer->pos.line) {
        buffer->screen_start.line = buffer->pos.line;
        buffer->screen_start.line->is_dirty |= DRAW_LINE_REFRESH_DOWN;
    }

    return STATUS_SUCCESS;
}

Status insert_character(Buffer *buffer, const char *character)
{
    size_t char_len = 0;
    
    if (character != NULL) {
        char_len = strnlen(character, 7);
    }

    if (char_len == 0 || char_len > 6) {
        char *character_copy = strdupe(character);
        Status status = raise_param_error(ERR_INVALID_CHARACTER, STR_VAL(character_copy));
        free(character_copy);
        return status;
    }

    Range range;

    if (get_selection_range(buffer, &range)) {
        Status status = delete_range(buffer, range);
        RETURN_IF_FAIL(status);
    }

    BufferPos *pos = &buffer->pos;

    resize_line_text_if_req(pos->line, pos->line->length + char_len);

    if (pos->line->length > 0 && pos->offset < pos->line->length) {
        memmove(pos->line->text + pos->offset + char_len, pos->line->text + pos->offset, pos->line->length - pos->offset);
    }

    size_t start_screen_height = line_screen_height(buffer->win_info, pos->line);

    while (*character && char_len--) {
        pos->line->screen_length += byte_screen_length(*character, pos->line, pos->offset);
        pos->line->text[pos->offset++] = *character++;
        pos->line->length++;
    }

    size_t end_screen_height = line_screen_height(buffer->win_info, pos->line);

    if (end_screen_height > start_screen_height) {
        pos->line->is_dirty |= DRAW_LINE_REFRESH_DOWN;
    } else {
        pos->line->is_dirty |= DRAW_LINE_EXTENDED;
    }

    return STATUS_SUCCESS;
}

Status insert_string(Buffer *buffer, char *string, size_t string_length, int advance_cursor)
{
    if (string == NULL) {
        return raise_param_error(ERR_INVALID_CHARACTER, STR_VAL(string));     
    } else if (string_length == 0) {
        return STATUS_SUCCESS;
    }

    Range range;

    if (get_selection_range(buffer, &range)) {
        Status status = delete_range(buffer, range);
        RETURN_IF_FAIL(status);
    }

    BufferPos *pos = &buffer->pos;

    resize_line_text_if_req(pos->line, pos->line->length + string_length);

    if (pos->line->length > 0 && pos->offset < pos->line->length) {
        memmove(pos->line->text + pos->offset + string_length, pos->line->text + pos->offset, pos->line->length - pos->offset);
    }

    size_t start_offset = pos->offset;
    size_t start_screen_height = line_screen_height(buffer->win_info, pos->line);

    while (string_length--) {
        pos->line->screen_length += byte_screen_length(*string, pos->line, pos->offset);
        pos->line->text[pos->offset++] = *string++;
        pos->line->length++;
    }

    size_t end_screen_height = line_screen_height(buffer->win_info, pos->line);

    if (!advance_cursor) {
        pos->offset = start_offset;
    }

    if (end_screen_height > start_screen_height) {
        pos->line->is_dirty |= DRAW_LINE_REFRESH_DOWN;
    } else {
        pos->line->is_dirty |= DRAW_LINE_EXTENDED;
    }

    return STATUS_SUCCESS;
} 

Status delete_character(Buffer *buffer)
{
    Range range;

    if (get_selection_range(buffer, &range)) {
        return delete_range(buffer, range);
    }

    BufferPos *pos = &buffer->pos;
    Line *line = pos->line;

    if (pos->offset == line->length) {
        if (line->next == NULL) {
            return STATUS_SUCCESS;
        }

        Status status = insert_string(buffer, line->next->text, line->next->length, 0);

        RETURN_IF_FAIL(status);

        status = delete_line(buffer, line->next);

        RETURN_IF_FAIL(status);

        line->is_dirty |= DRAW_LINE_REFRESH_DOWN;

        return STATUS_SUCCESS;
    }

    size_t char_byte_len = char_byte_length(line->text[pos->offset]);
    size_t screen_length = byte_screen_length(line->text[pos->offset], line, pos->offset);

    if (pos->offset != (line->length - 1)) {
        memmove(line->text + pos->offset, line->text + pos->offset + char_byte_len, line->length - pos->offset);
    }

    line->length -= char_byte_len;
    line->screen_length -= screen_length; 
    pos->line->is_dirty |= DRAW_LINE_SHRUNK;

    resize_line_text_if_req(pos->line, pos->line->length);

    return STATUS_SUCCESS;
}

Status delete_line(Buffer *buffer, Line *line)
{
    if (buffer == NULL || line == NULL) {
        return STATUS_SUCCESS;
    }

    if (line->prev != NULL) {
        line->prev->next = line->next;
    } 

    if (line->next != NULL) {
        line->next->prev = line->prev;
    }

    if (buffer->pos.line == line) {
        if (line->next != NULL) {
            buffer->pos.line = line->next;
        } else {
            buffer->pos.line = line->prev;
        }    
    }

    if (buffer->screen_start.line == line) {
        if (line->next != NULL) {
            buffer->screen_start.line = line->next;
        } else {
            buffer->screen_start.line = line->prev;
        }    
    }

    if (buffer->lines == line) {
        if (line->next != NULL) {
            buffer->lines = line->next;
        } else {
            reset_buffer(buffer);
        }    
    }

    free_line(line);

    return STATUS_SUCCESS;
}

Status insert_line(Buffer *buffer)
{
    Range range;

    if (get_selection_range(buffer, &range)) {
        Status status = delete_range(buffer, range);
        RETURN_IF_FAIL(status);
    }

    BufferPos *pos = &buffer->pos;
    size_t line_length = pos->line->length - pos->offset;

    Line *line = new_sized_line(line_length);

    if (line_length > 0) {
        memcpy(line->text, pos->line->text + pos->offset, line_length); 
        line->length = line_length;
        line->screen_length = line_screen_length(pos->line, pos->offset, pos->line->length);
        pos->line->screen_length -= line->screen_length;
        pos->line->length = pos->offset;
    }

    line->next = pos->line->next;
    line->prev = pos->line;
    pos->line->next = line;

    if (line->next != NULL) {
        line->next->prev = line;
    }

    pos->line->is_dirty |= DRAW_LINE_REFRESH_DOWN;
    pos->line = line;
    pos->offset = 0;

    return STATUS_SUCCESS;
}

Status select_continue(Buffer *buffer)
{
    if (buffer->select_start.line == NULL) {
        buffer->select_start = buffer->pos;
    }

    return STATUS_SUCCESS;
}

Status select_reset(Buffer *buffer)
{
    buffer->select_start.line = NULL;
    buffer->select_start.offset = 0;

    return STATUS_SUCCESS;
}

/* start_offset is inclusive, end_offset is exclusive */
static Status delete_line_segment(Line *line, size_t start_offset, size_t end_offset)
{
    if (line->length == 0 || start_offset >= line->length || end_offset <= start_offset) {
        return STATUS_SUCCESS; 
    }

    end_offset = (end_offset > line->length ? line->length : end_offset);
    size_t bytes_to_move = line->length - end_offset;
    size_t screen_length = line_screen_length(line, start_offset, end_offset);

    if (bytes_to_move > 0) {
        memmove(line->text + start_offset, line->text + end_offset, bytes_to_move);
    }

    line->length -= (end_offset - start_offset);
    line->screen_length -= screen_length; 
    line->is_dirty |= DRAW_LINE_SHRUNK;

    resize_line_text_if_req(line, line->length);

    return STATUS_SUCCESS;
}

Status delete_range(Buffer *buffer, Range range)
{
    select_reset(buffer);
    buffer->pos = range.start;

    int is_single_line = (range.start.line == range.end.line);
    Status status = delete_line_segment(range.start.line, range.start.offset, 
                                        is_single_line ? range.end.offset : range.start.line->length); 

    if (is_single_line || !is_success(status)) {
        if (config_bool("linewrap") && (range.end.offset - range.start.offset) >= buffer->win_info.width) {
            buffer->pos.line->is_dirty |= DRAW_LINE_REFRESH_DOWN;
        }

        return status;
    }

    Line *line = range.start.line->next;
    Line *next;

    while (line != range.end.line) {
        next = line->next;
        status = delete_line(buffer, line);

        RETURN_IF_FAIL(status);

        line = next;
    }

    status = insert_string(buffer, range.end.line->text + range.end.offset, range.end.line->length - range.end.offset, 0);
    RETURN_IF_FAIL(status);

    status = delete_line(buffer, range.end.line);

    buffer->pos.line->is_dirty |= DRAW_LINE_REFRESH_DOWN;

    return status;
}

Status select_all_text(Buffer *buffer)
{
    Line *line = buffer->pos.line;

    while (line->next != NULL) {
        line = line->next;
    }

    buffer->select_start = (BufferPos) { .line = line, .offset = line->length };
    buffer->pos = (BufferPos) { .line = buffer->lines, .offset = 0 };

    buffer->pos.line->is_dirty |= DRAW_LINE_REFRESH_DOWN;

    return STATUS_SUCCESS;
}

Status copy_selected_text(Buffer *buffer, TextSelection **text_selection)
{
    if (buffer == NULL || text_selection == NULL) {
        return STATUS_SUCCESS;
    }

    Range range;

    if (!get_selection_range(buffer, &range)) {
        *text_selection = NULL;
        return STATUS_SUCCESS;
    }

    *text_selection = new_textselection(range); 

    return STATUS_SUCCESS;
}

Status cut_selected_text(Buffer *buffer, TextSelection **text_selection)
{
    if (buffer == NULL || text_selection == NULL) {
        return STATUS_SUCCESS;
    }

    Range range;

    if (!get_selection_range(buffer, &range)) {
        return STATUS_SUCCESS;
    }
    
    Status status = copy_selected_text(buffer, text_selection);

    if (!is_success(status) || text_selection == NULL) {
        return status;
    }

    return delete_range(buffer, range);
}

Status insert_textselection(Buffer *buffer, TextSelection *text_selection)
{
    if (buffer == NULL || text_selection == NULL) {
        return STATUS_SUCCESS;
    }

    Range range;

    if (get_selection_range(buffer, &range)) {
        RETURN_IF_FAIL(delete_range(buffer, range));
    }

    if (text_selection->type == TST_STRING) {
        return insert_string(buffer, text_selection->text.string, strlen(text_selection->text.string), 1);
    }

    Line *line = text_selection->text.lines;
    Line *buf_line = buffer->pos.line;

    RETURN_IF_FAIL(insert_string(buffer, line->text, line->length, 1));
    RETURN_IF_FAIL(insert_line(buffer));

    Line *end_line = buffer->pos.line;
    line = line->next;

    while (line->next != NULL) {
        buf_line->next = clone_line(line); 
        buf_line->next->prev = buf_line;
        buf_line = buf_line->next;
        line = line->next;
    }

    buf_line->next = end_line; 
    end_line->prev = buf_line;

    return insert_string(buffer, line->text, line->length, 1);
}
