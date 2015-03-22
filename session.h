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

#ifndef WED_SESSION_H
#define WED_SESSION_H

#include "shared.h"
#include "buffer.h"
#include "status.h"
#include "hashmap.h"

/* Top level structure containing all state.
 * A new session is created when wed is invoked. */
typedef struct {
    Buffer *buffers; /* Linked list buffers */
    Buffer *active_buffer; /* The buffer currently being edited */
    Buffer *error_buffer;
    Buffer *msg_buffer;
    HashMap *keymap; /* Maps keyboard inputs to commands */
    TextSelection *clipboard; /* Stores copied and cut text */
    HashMap *config; /* Stores config variables */
    struct {
        Buffer *cmd_buffer; /* Used for command input e.g. Find & Replace, file name input, etc... */
        char *cmd_text; /* Command instruction/description */
        int cancelled; /* Did the user quit the last prompt */
    } cmd_prompt; 
    CommandType exclude_cmd_types; /* Types of commands that shouldn't run */
    size_t buffer_num;
    size_t active_buffer_index;
    size_t menu_first_buffer_index;
    size_t empty_buffer_num;
    int msgs_enabled;
} Session;

Session *new_session(void);
int init_session(Session *, char **, int);
void free_session(Session *);
int add_buffer(Session *, Buffer *);
int set_active_buffer(Session *, size_t);
Buffer *get_buffer(Session *, size_t);
int remove_buffer(Session *, Buffer *);
Status make_cmd_buffer_active(Session *, const char *);
int end_cmd_buffer_active(Session *);
int cmd_buffer_active(Session *);
char *get_cmd_buffer_text(Session *);
int add_error(Session *, Status);
void set_clipboard(Session *, TextSelection *);
void exclude_command_type(Session *, CommandType);
void enable_command_type(Session *, CommandType);
int command_type_excluded(Session *, CommandType);
int has_errors(Session *);
void clear_errors(Session *);
int add_msg(Session *, const char *);
int has_msgs(Session *);
void clear_msgs(Session *);
Status add_new_buffer(Session *, const char *);
Status add_new_empty_buffer(Session *);
Status get_buffer_index(Session *, const char *, int *);

#endif
