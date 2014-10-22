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

#include <ncurses.h>
#include "display.h"
#include "session.h"
#include "buffer.h"
#include "util.h"

#define WINDOW_NUM 3
#define WED_TAB_SIZE 8

static WINDOW *menu;
static WINDOW *status;
static WINDOW *text;
static WINDOW *windows[WINDOW_NUM];
static size_t text_y;
static size_t text_x;

static void handle_draw_status(Line *, LineDrawStatus *);
static size_t draw_line(Line *, size_t, int, LineDrawStatus *, int, Range);
static void convert_pos_to_point(Point *, BufferPos);
static void vertical_scroll(Buffer *, Point *, Point);

/* ncurses setup */
void init_display(void)
{
    initscr();

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(MENU_COLOR_PAIR, COLOR_BLUE, COLOR_WHITE);
        init_pair(TAB_COLOR_PAIR, COLOR_BLUE, COLOR_WHITE);
        init_pair(STATUS_COLOR_PAIR, COLOR_YELLOW, COLOR_BLUE);
    }

    raw();
    noecho();
    nl();
    keypad(stdscr, TRUE);
    curs_set(1);
    set_tabsize(WED_TAB_SIZE);

    text_y = LINES - 2;
    text_x = COLS;

    windows[0] = menu = newwin(1, COLS, 0, 0); 
    windows[1] = text = newwin(text_y, text_x, 1, 0);
    windows[2] = status = newwin(1, COLS, LINES - 1, 0);

    refresh();
}

void end_display(void)
{
    endwin();
}

void refresh_display(Session *sess)
{
    draw_menu(sess);
    draw_status(sess);
    draw_text(sess, DRAW_LINE_FULL_REFRESH);
    update_display(sess);
}

/* Draw top menu */
/* TODO Show other buffers with numbers and colors */
void draw_menu(Session *sess)
{
    Buffer *buffer = sess->active_buffer;
    wclrtoeol(menu);
    wbkgd(menu, COLOR_PAIR(MENU_COLOR_PAIR));
    wattron(menu, COLOR_PAIR(MENU_COLOR_PAIR));
    mvwprintw(menu, 0, 0, " %s", buffer->file_info.file_name); 
    wattroff(menu, COLOR_PAIR(MENU_COLOR_PAIR));
    wnoutrefresh(menu); 
}

/* TODO There's a lot more this function could show.
 * The layout needs to be considered. */
void draw_status(Session *sess)
{
    Buffer *buffer = sess->active_buffer;
    size_t line_no = get_pos_line_number(buffer);
    size_t col_no = get_pos_col_number(buffer);

    wmove(status, 0, 0);
    wbkgd(status, COLOR_PAIR(STATUS_COLOR_PAIR));
    wattron(status, COLOR_PAIR(STATUS_COLOR_PAIR));
    wprintw(status, "Line %zu Column %zu", line_no, col_no); 
    wclrtoeol(status);
    wattroff(status, COLOR_PAIR(STATUS_COLOR_PAIR));
    wnoutrefresh(status); 
}

static void handle_draw_status(Line *line, LineDrawStatus *draw_status)
{
    if (line->is_dirty) {
        if (line->is_dirty & DRAW_LINE_REFRESH_DOWN) {
            *draw_status |= DRAW_LINE_REFRESH_DOWN;
        } else if (line->is_dirty & DRAW_LINE_END_REFRESH_DOWN) {
            *draw_status &= ~DRAW_LINE_REFRESH_DOWN;
        } else if (line->is_dirty & DRAW_LINE_SCROLL_REFRESH_DOWN) {
            *draw_status |= DRAW_LINE_SCROLL_REFRESH_DOWN;
        } else if (line->is_dirty & DRAW_LINE_SCROLL_END_REFRESH_DOWN) {
            *draw_status &= ~DRAW_LINE_SCROLL_REFRESH_DOWN;
        }    

        line->is_dirty = DRAW_LINE_NO_CHANGE;
    }
}

/* Refresh the active buffer on the screen. Only draw
 * necessary buffer parts. */
void draw_text(Session *sess, LineDrawStatus draw_status)
{
    if (draw_status & DRAW_LINE_FULL_REFRESH) {
        wclear(text);
    }

    Buffer *buffer = sess->active_buffer;
    Range select_range;
    int is_selection = get_selection_range(buffer, &select_range);
    size_t line_num = text_y;
    size_t line_count = 0;
    BufferPos screen_start = buffer->screen_start;
    Line *line = buffer->lines;

    while (line != screen_start.line) {
        handle_draw_status(line, &draw_status);
        line = line->next;
    }

    line_count += draw_line(line, screen_start.offset, line_count, &draw_status, is_selection, select_range);
    handle_draw_status(line, &draw_status);
    line = line->next;

    while (line_count < line_num && line != NULL) {
        line_count += draw_line(line, 0, line_count, &draw_status, is_selection, select_range);
        handle_draw_status(line, &draw_status);
        line = line->next;
    }

    if (draw_status) {
        wstandend(text);

        while (line_count < text_y) {
            mvwaddch(text, line_count++, 0, '~');
            wclrtoeol(text);
        }
    }

    while (line != NULL) {
        if (line->is_dirty) {
            line->is_dirty = DRAW_LINE_NO_CHANGE;
        }

        line = line->next;
    }
}

static size_t draw_line(Line *line, size_t char_index, int y, LineDrawStatus *draw_status, int is_selection, Range select_range)
{
    if (!(*draw_status || line->is_dirty)) {
        if (char_index > 0) {
            return line_offset_screen_height(line, char_index, line->length);
        } 

        return line_screen_height(line);
    }

    if (line->length == 0) {
        if (*draw_status || (line->is_dirty & (DRAW_LINE_SHRUNK | DRAW_LINE_REFRESH_DOWN))) {
            wmove(text, y, 0);
            wclrtoeol(text);
        }

        return 1;
    }

    BufferPos draw_pos = { .line = line, .offset = char_index };
    size_t scr_line_num = 0;
    size_t start_index = char_index;
    size_t char_byte_len;

    while (draw_pos.offset < line->length && scr_line_num < text_y) {
        wmove(text, y++, 0);
        scr_line_num++;

        for (size_t k = 0; k < text_x && draw_pos.offset < line->length; draw_pos.offset += char_byte_len, k++) {
            if (is_selection && bufferpos_in_range(select_range, draw_pos)) {
                wattron(text, A_REVERSE);
            } else {
                wattroff(text, A_REVERSE);
            }

            char_byte_len = char_byte_length(line->text[draw_pos.offset]);
            waddnstr(text, line->text + draw_pos.offset, char_byte_len);
        }
    }

    if (*draw_status || (line->is_dirty & (DRAW_LINE_SHRUNK | DRAW_LINE_REFRESH_DOWN))) {
        wclrtoeol(text);  
    }

    if (scr_line_num < line_offset_screen_height(line, start_index, line->length)) {
        wmove(text, y, 0);
        wclrtoeol(text);
        scr_line_num++;
    }

    return scr_line_num;
}

/* Update the menu, status and active buffer views.
 * This is called after a change has been made that
 * needs to be reflected in the UI. */
void update_display(Session *sess)
{
    draw_status(sess);

    Buffer *buffer = sess->active_buffer;

    Point screen_start, cursor;
    convert_pos_to_point(&screen_start, buffer->screen_start);
    convert_pos_to_point(&cursor, buffer->pos);

    vertical_scroll(buffer, &screen_start, cursor);
    draw_text(sess, 0);

    wmove(text, cursor.line_no - screen_start.line_no, cursor.col_no);
    wnoutrefresh(text);

    doupdate();
}

static void convert_pos_to_point(Point *point, BufferPos pos)
{
    point->line_no = screen_line_no(pos);
    point->col_no = screen_col_no(pos);
}

/* Calculate the line number pos represents counting 
 * wrapped lines as whole lines */
size_t screen_line_no(BufferPos pos)
{
    size_t line_no = 0;
    Line *line = pos.line;

    line_no += line_pos_screen_height(pos);

    while ((line = line->prev) != NULL) {
        line_no += line_screen_height(line);
    }

    return line_no;
}

/* TODO This isn't generic, it assumes line wrapping is enabled. This 
 * may not be the case in future when horizontal scrolling is added. */
size_t screen_col_no(BufferPos pos)
{
    size_t screen_length = line_screen_length(pos.line, 0, pos.offset);
    return screen_length % text_x; 
}

/* The number of screen columns taken up by this line segment */
size_t line_screen_length(Line *line, size_t start_offset, size_t limit_offset)
{
    size_t screen_length = 0;

    if (line == NULL || (limit_offset <= start_offset)) {
        return screen_length;
    }

    for (size_t k = start_offset; k < line->length && k < limit_offset; k++) {
        screen_length += byte_screen_length(line->text[k], line, k);
    }

    return screen_length;
}

size_t line_screen_height(Line *line)
{
    return screen_height_from_screen_length(line->screen_length);
}

size_t line_pos_screen_height(BufferPos pos)
{
    size_t screen_length = line_screen_length(pos.line, 0, pos.offset);
    return screen_height_from_screen_length(screen_length);
}

size_t line_offset_screen_height(Line *line, size_t start_offset, size_t limit_offset)
{
    size_t screen_length = line_screen_length(line, start_offset, limit_offset);
    return screen_height_from_screen_length(screen_length);
}

/* This calculates the number of lines that text, when displayed on the screen
 * takes up screen_length columns, takes up */
size_t screen_height_from_screen_length(size_t screen_length)
{
    if (screen_length == 0) {
        return 1;
    } else if ((screen_length % text_x) == 0) {
        screen_length++;
    }

    return roundup_div(screen_length, text_x);
}

/* The number of columns a byte takes up on the screen.
 * Continuation bytes take up no screen space for example. */
size_t byte_screen_length(char c, Line *line, size_t offset)
{
    if (line->length == offset) {
        return 1;
    } else if (c == '\t') {
        if (offset == 0) {
            return WED_TAB_SIZE;
        }

        size_t col_index = line_screen_length(line, 0, offset);
        return WED_TAB_SIZE - (col_index % WED_TAB_SIZE);
    }

    return ((c & 0xc0) != 0x80);
}

/* The length in bytes of a character 
 * (assumes we're on the first byte of the character) */
size_t char_byte_length(char c)
{
    if (!(c & 0x80)) {
        return 1;
    }

    if ((c & 0xFC) == 0xFC) {
        return 6;
    }

    if ((c & 0xF8) == 0xF8) {
        return 5;
    }

    if ((c & 0xF0) == 0xF0) {
        return 4;
    }

    if ((c & 0xE0) == 0xE0) {
        return 3;
    }

    if ((c & 0xC0) == 0xC0) {
        return 2;
    }

    return 0;
}

size_t editor_screen_width(void)
{
    return text_x;
}

size_t editor_screen_height(void)
{
    return text_y;
}

/* Determine if the screen needs to be scrolled and what parts need to be updated if so */
static void vertical_scroll(Buffer *buffer, Point *screen_start, Point cursor)
{
    size_t diff;
    Direction direction;

    if (cursor.line_no > screen_start->line_no) {
        diff = cursor.line_no - screen_start->line_no;
        direction = DIRECTION_DOWN;
    } else {
        diff = screen_start->line_no - cursor.line_no;
        direction = DIRECTION_UP;
    }

    if (diff == 0) {
        return;
    }

    if (direction == DIRECTION_DOWN) {
        if (diff < text_y) {
            return;
        }

        diff -= (text_y - 1);

        size_t draw_start = diff % text_y;

        if (draw_start == 0) {
            draw_start = text_y;
        }

        Line *line = get_line_from_offset(buffer->pos.line, DIRECTION_UP, draw_start - 1);
        line->is_dirty |= DRAW_LINE_SCROLL_REFRESH_DOWN;
    }

    pos_change_multi_screen_line(buffer, &buffer->screen_start, direction, diff, 0);
    convert_pos_to_point(screen_start, buffer->screen_start);

    if (direction == DIRECTION_UP) {
        buffer->screen_start.line->is_dirty |= DRAW_LINE_SCROLL_REFRESH_DOWN;
        Line *line = get_line_from_offset(buffer->screen_start.line, DIRECTION_DOWN, diff);
        line->is_dirty |= DRAW_LINE_SCROLL_END_REFRESH_DOWN;
    }

    scrollok(text, TRUE);
    wscrl(text, diff * DIRECTION_OFFSET(direction));
    scrollok(text, FALSE);
}

