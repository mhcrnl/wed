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
#include <ncurses.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <errno.h>
#include <assert.h>
#include "shared.h"
#include "status.h"
#include "command.h"
#include "session.h"
#include "display.h"
#include "buffer.h"
#include "value.h"
#include "hashmap.h"
#include "util.h"
#include "input.h"
#include "config.h"
#include "config_parse_util.h"
#include "search.h"
#include "replace.h"
#include "prompt_completer.h"

typedef enum {
    QR_NONE = 0,
    QR_YES = 1,
    QR_NO = 1 << 1,
    QR_CANCEL = 1 << 2,
    QR_ERROR = 1 << 3,
    QR_ALL = 1 << 4
} QuestionRespose;

static void cm_free_command(void *);

static Status cm_bp_change_line(Session *, Value, const char *, int *);
static Status cm_bp_change_char(Session *, Value, const char *, int *); 
static Status cm_bp_to_line_start(Session *, Value, const char *, int *);
static Status cm_bp_to_line_end(Session *, Value, const char *, int *);
static Status cm_bp_to_next_word(Session *, Value, const char *, int *);
static Status cm_bp_to_prev_word(Session *, Value, const char *, int *);
static Status cm_bp_to_buffer_start(Session *, Value, const char *, int *);
static Status cm_bp_to_buffer_end(Session *, Value, const char *, int *);
static Status cm_bp_change_page(Session *, Value, const char *, int *);
static Status cm_bp_goto_matching_bracket(Session *, Value, const char *, int *);
static Status cm_buffer_insert_char(Session *, Value, const char *, int *);
static Status cm_buffer_delete_char(Session *, Value, const char *, int *);
static Status cm_buffer_backspace(Session *, Value, const char *, int *);
static Status cm_buffer_delete_word(Session *, Value, const char *, int *);
static Status cm_buffer_delete_prev_word(Session *, Value, const char *, int *);
static Status cm_buffer_insert_line(Session *, Value, const char *, int *);
static Status cm_buffer_select_all_text(Session *, Value, const char *, int *);
static Status cm_buffer_copy_selected_text(Session *, Value, const char *, int *);
static Status cm_buffer_cut_selected_text(Session *, Value, const char *, int *);
static Status cm_buffer_paste_text(Session *, Value, const char *, int *);
static Status cm_buffer_undo(Session *, Value, const char *, int *);
static Status cm_buffer_redo(Session *, Value, const char *, int *);
static Status cm_buffer_vert_move_lines(Session *, Value, const char *, int *);
static Status cm_buffer_duplicate_selection(Session *, Value, const char *, int *);
static Status cm_buffer_indent(Session *, Value, const char *, int *);
static Status cm_buffer_save_file(Session *, Value, const char *, int *);
static void cm_generate_find_prompt(const BufferSearch *, char prompt_text[MAX_CMD_PROMPT_LENGTH]);
static Status cm_prerpare_search(Session *, const BufferPos *);
static Status cm_buffer_find(Session *, Value, const char *, int *);
static Status cm_buffer_find_next(Session *, Value, const char *, int *);
static Status cm_buffer_toggle_search_direction(Session *, Value, const char *, int *);
static Status cm_buffer_toggle_search_type(Session *, Value, const char *, int *);
static Status cm_buffer_toggle_search_case(Session *, Value, const char *, int *);
static Status cm_buffer_replace(Session *, Value, const char *, int *);
static Status cm_buffer_goto_line(Session *, Value, const char *, int *);
static Status cm_prepare_replace(Session *, char **, size_t *);
static Status cm_session_open_file(Session *, Value, const char *, int *);
static Status cm_session_add_empty_buffer(Session *, Value, const char *, int *);
static Status cm_session_change_tab(Session *, Value, const char *, int *);
static Status cm_session_save_all(Session *, Value, const char *, int *);
static Status cm_session_close_buffer(Session *, Value, const char *, int *);
static Status cm_session_run_command(Session *, Value, const char *, int *);
static Status cm_previous_cmd_entry(Session *, Value, const char *, int *);
static Status cm_next_cmd_entry(Session *, Value, const char *, int *);
static Status cm_finished_processing_input(Session *, Value, const char *, int *);
static Status cm_session_change_buffer(Session *, Value, const char *, int *);
static Status cm_determine_buffer(Session *, const char *, const Buffer **);
static Status cm_suspend(Session *, Value, const char *, int *);
static Status cm_session_end(Session *, Value, const char *, int *);

static Status cm_cmd_input_prompt(Session *, PromptType, const char *, List *, int);
static QuestionRespose cm_question_prompt(Session *, PromptType, const char *, QuestionRespose, QuestionRespose);
static Status cm_cancel_cmd_input_prompt(Session *, Value, const char *, int *);
static int cm_update_command_function(Session *, const char *, CommandHandler);
static Status cm_run_input_completion(Session *, Value, const char *, int *);

static const Command commands[] = {
    { "<Up>"         , cm_bp_change_line                , INT_VAL_STRUCT(DIRECTION_UP)                           , CMDT_BUFFER_MOVE },
    { "<Down>"       , cm_bp_change_line                , INT_VAL_STRUCT(DIRECTION_DOWN)                         , CMDT_BUFFER_MOVE },
    { "<Right>"      , cm_bp_change_char                , INT_VAL_STRUCT(DIRECTION_RIGHT)                        , CMDT_BUFFER_MOVE },
    { "<Left>"       , cm_bp_change_char                , INT_VAL_STRUCT(DIRECTION_LEFT)                         , CMDT_BUFFER_MOVE },
    { "<Home>"       , cm_bp_to_line_start              , INT_VAL_STRUCT(0)                                      , CMDT_BUFFER_MOVE },
    { "<End>"        , cm_bp_to_line_end                , INT_VAL_STRUCT(0)                                      , CMDT_BUFFER_MOVE },
    { "<C-Right>"    , cm_bp_to_next_word               , INT_VAL_STRUCT(0)                                      , CMDT_BUFFER_MOVE },
    { "<C-Left>"     , cm_bp_to_prev_word               , INT_VAL_STRUCT(0)                                      , CMDT_BUFFER_MOVE },
    { "<C-Home>"     , cm_bp_to_buffer_start            , INT_VAL_STRUCT(0)                                      , CMDT_BUFFER_MOVE },
    { "<C-End>"      , cm_bp_to_buffer_end              , INT_VAL_STRUCT(0)                                      , CMDT_BUFFER_MOVE },
    { "<PageUp>"     , cm_bp_change_page                , INT_VAL_STRUCT(DIRECTION_UP)                           , CMDT_BUFFER_MOVE },
    { "<PageDown>"   , cm_bp_change_page                , INT_VAL_STRUCT(DIRECTION_DOWN)                         , CMDT_BUFFER_MOVE },
    { "<S-Up>"       , cm_bp_change_line                , INT_VAL_STRUCT(DIRECTION_UP    | DIRECTION_WITH_SELECT), CMDT_BUFFER_MOVE },
    { "<S-Down>"     , cm_bp_change_line                , INT_VAL_STRUCT(DIRECTION_DOWN  | DIRECTION_WITH_SELECT), CMDT_BUFFER_MOVE },
    { "<S-Right>"    , cm_bp_change_char                , INT_VAL_STRUCT(DIRECTION_RIGHT | DIRECTION_WITH_SELECT), CMDT_BUFFER_MOVE },
    { "<S-Left>"     , cm_bp_change_char                , INT_VAL_STRUCT(DIRECTION_LEFT  | DIRECTION_WITH_SELECT), CMDT_BUFFER_MOVE },
    { "<S-Home>"     , cm_bp_to_line_start              , INT_VAL_STRUCT(DIRECTION_WITH_SELECT)                  , CMDT_BUFFER_MOVE },
    { "<S-End>"      , cm_bp_to_line_end                , INT_VAL_STRUCT(DIRECTION_WITH_SELECT)                  , CMDT_BUFFER_MOVE },
    { "<C-S-Right>"  , cm_bp_to_next_word               , INT_VAL_STRUCT(DIRECTION_WITH_SELECT)                  , CMDT_BUFFER_MOVE },
    { "<C-S-Left>"   , cm_bp_to_prev_word               , INT_VAL_STRUCT(DIRECTION_WITH_SELECT)                  , CMDT_BUFFER_MOVE },
    { "<C-S-Home>"   , cm_bp_to_buffer_start            , INT_VAL_STRUCT(DIRECTION_WITH_SELECT)                  , CMDT_BUFFER_MOVE },
    { "<C-S-End>"    , cm_bp_to_buffer_end              , INT_VAL_STRUCT(DIRECTION_WITH_SELECT)                  , CMDT_BUFFER_MOVE },
    { "<S-PageUp>"   , cm_bp_change_page                , INT_VAL_STRUCT(DIRECTION_UP   | DIRECTION_WITH_SELECT) , CMDT_BUFFER_MOVE },
    { "<S-PageDown>" , cm_bp_change_page                , INT_VAL_STRUCT(DIRECTION_DOWN | DIRECTION_WITH_SELECT) , CMDT_BUFFER_MOVE },
    { "<C-b>"        , cm_bp_goto_matching_bracket      , INT_VAL_STRUCT(0)                                      , CMDT_BUFFER_MOVE },
    { "<Space>"      , cm_buffer_insert_char            , STR_VAL_STRUCT(" ")                                    , CMDT_BUFFER_MOD  }, 
    { "<Tab>"        , cm_buffer_indent                 , INT_VAL_STRUCT(DIRECTION_RIGHT)                        , CMDT_BUFFER_MOD  }, 
    { "<S-Tab>"      , cm_buffer_indent                 , INT_VAL_STRUCT(DIRECTION_LEFT)                         , CMDT_BUFFER_MOD  },
    { "<KPDiv>"      , cm_buffer_insert_char            , STR_VAL_STRUCT("/")                                    , CMDT_BUFFER_MOD  }, 
    { "<KPMult>"     , cm_buffer_insert_char            , STR_VAL_STRUCT("*")                                    , CMDT_BUFFER_MOD  }, 
    { "<KPMinus>"    , cm_buffer_insert_char            , STR_VAL_STRUCT("-")                                    , CMDT_BUFFER_MOD  }, 
    { "<KPPlus>"     , cm_buffer_insert_char            , STR_VAL_STRUCT("+")                                    , CMDT_BUFFER_MOD  }, 
    { "<Delete>"     , cm_buffer_delete_char            , INT_VAL_STRUCT(0)                                      , CMDT_BUFFER_MOD  }, 
    { "<Backspace>"  , cm_buffer_backspace              , INT_VAL_STRUCT(0)                                      , CMDT_BUFFER_MOD  }, 
    { "<C-Delete>"   , cm_buffer_delete_word            , INT_VAL_STRUCT(0)                                      , CMDT_BUFFER_MOD  }, 
    { "<M-Backspace>", cm_buffer_delete_prev_word       , INT_VAL_STRUCT(0)                                      , CMDT_BUFFER_MOD  }, 
    { "<Enter>"      , cm_buffer_insert_line            , INT_VAL_STRUCT(0)                                      , CMDT_BUFFER_MOD  }, 
    { "<C-a>"        , cm_buffer_select_all_text        , INT_VAL_STRUCT(0)                                      , CMDT_BUFFER_MOD  }, 
    { "<C-c>"        , cm_buffer_copy_selected_text     , INT_VAL_STRUCT(0)                                      , CMDT_BUFFER_MOD  }, 
    { "<C-x>"        , cm_buffer_cut_selected_text      , INT_VAL_STRUCT(0)                                      , CMDT_BUFFER_MOD  }, 
    { "<C-v>"        , cm_buffer_paste_text             , INT_VAL_STRUCT(0)                                      , CMDT_BUFFER_MOD  }, 
    { "<C-z>"        , cm_buffer_undo                   , INT_VAL_STRUCT(0)                                      , CMDT_BUFFER_MOD  },
    { "<C-y>"        , cm_buffer_redo                   , INT_VAL_STRUCT(0)                                      , CMDT_BUFFER_MOD  },
    { "<C-S-Up>"     , cm_buffer_vert_move_lines        , INT_VAL_STRUCT(DIRECTION_UP)                           , CMDT_BUFFER_MOD  },
    { "<C-S-Down>"   , cm_buffer_vert_move_lines        , INT_VAL_STRUCT(DIRECTION_DOWN)                         , CMDT_BUFFER_MOD  },
    { "<C-d>"        , cm_buffer_duplicate_selection    , INT_VAL_STRUCT(0)                                      , CMDT_BUFFER_MOD  },
    { "<C-s>"        , cm_buffer_save_file              , INT_VAL_STRUCT(0)                                      , CMDT_CMD_INPUT   },
    { "<C-f>"        , cm_buffer_find                   , INT_VAL_STRUCT(0)                                      , CMDT_CMD_INPUT   },
    { "<F3>"         , cm_buffer_find_next              , INT_VAL_STRUCT(0)                                      , CMDT_CMD_INPUT   },
    { "<F15>"        , cm_buffer_find_next              , INT_VAL_STRUCT(1)                                      , CMDT_CMD_INPUT   },
    { "<C-r>"        , cm_buffer_toggle_search_type     , INT_VAL_STRUCT(0)                                      , CMDT_CMD_MOD     },
    { "<M-i>"        , cm_buffer_toggle_search_case     , INT_VAL_STRUCT(0)                                      , CMDT_CMD_MOD     },
    { "<C-h>"        , cm_buffer_replace                , INT_VAL_STRUCT(0)                                      , CMDT_CMD_INPUT   },
    { "<C-g>"        , cm_buffer_goto_line              , INT_VAL_STRUCT(0)                                      , CMDT_CMD_INPUT   },
    { "<C-o>"        , cm_session_open_file             , INT_VAL_STRUCT(0)                                      , CMDT_CMD_INPUT   },
    { "<C-n>"        , cm_session_add_empty_buffer      , INT_VAL_STRUCT(0)                                      , CMDT_SESS_MOD    },
    { "<M-C-Right>"  , cm_session_change_tab            , INT_VAL_STRUCT(DIRECTION_RIGHT)                        , CMDT_SESS_MOD    },
    { "<M-Right>"    , cm_session_change_tab            , INT_VAL_STRUCT(DIRECTION_RIGHT)                        , CMDT_SESS_MOD    },
    { "<M-C-Left>"   , cm_session_change_tab            , INT_VAL_STRUCT(DIRECTION_LEFT)                         , CMDT_SESS_MOD    },
    { "<M-Left>"     , cm_session_change_tab            , INT_VAL_STRUCT(DIRECTION_LEFT)                         , CMDT_SESS_MOD    },
    { "<C-^>"        , cm_session_save_all              , INT_VAL_STRUCT(0)                                      , CMDT_SESS_MOD    },
    { "<C-w>"        , cm_session_close_buffer          , INT_VAL_STRUCT(0)                                      , CMDT_CMD_INPUT   },
    { "<C-\\>"       , cm_session_run_command           , INT_VAL_STRUCT(0)                                      , CMDT_CMD_INPUT   },
    { "<C-_>"        , cm_session_change_buffer         , INT_VAL_STRUCT(0)                                      , CMDT_CMD_INPUT   },
    { "<M-z>"        , cm_suspend                       , INT_VAL_STRUCT(0)                                      , CMDT_SUSPEND     },
    { "<M-c>"        , cm_session_end                   , INT_VAL_STRUCT(0)                                      , CMDT_EXIT        },
    { "<Escape>"     , cm_session_end                   , INT_VAL_STRUCT(0)                                      , CMDT_EXIT        }
};

int cm_init_keymap(Session *sess)
{
    size_t command_num = sizeof(commands) / sizeof(Command);

    sess->keymap = new_sized_hashmap(command_num * 2);

    if (sess->keymap == NULL) {
        return 0;
    }

    Command *command;

    for (size_t k = 0; k < command_num; k++) {
        command = malloc(sizeof(Command));

        if (command == NULL) {
            return 0;
        }

        memcpy(command, &commands[k], sizeof(Command));

        if (!hashmap_set(sess->keymap, command->keystr, command)) {
            return 0;
        }
    }

    return 1;
}

static void cm_free_command(void *command)
{
    if (command != NULL) {
        free(command);
    }
}

void cm_free_keymap(Session *sess)
{
    if (sess == NULL || sess->keymap == NULL) {
        return;
    } 

    free_hashmap_values(sess->keymap, cm_free_command);
    free_hashmap(sess->keymap);
}

Status cm_do_command(Session *sess, const char *command_str, int *finished)
{
    assert(!is_null_or_empty(command_str));
    assert(finished != NULL);

    Command *command = hashmap_get(sess->keymap, command_str);

    if (command != NULL && !se_command_type_excluded(sess, command->cmd_type)) {
        return command->command_handler(sess, command->param, command_str, finished);
    }

    if (!(command_str[0] == '<' && command_str[1] != '\0') &&
        !se_command_type_excluded(sess, CMDT_BUFFER_MOD)) {
        return bf_insert_character(sess->active_buffer, command_str, 1);
    }

    return STATUS_SUCCESS;
}

static Status cm_bp_change_line(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)keystr;
    (void)finished;
    return bf_change_line(sess->active_buffer, &sess->active_buffer->pos, IVAL(param), 1);
}

static Status cm_bp_change_char(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)keystr;
    (void)finished;
    return bf_change_char(sess->active_buffer, &sess->active_buffer->pos, IVAL(param), 1);
}

static Status cm_bp_to_line_start(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)keystr;
    (void)finished;
    return bf_to_line_start(sess->active_buffer, &sess->active_buffer->pos, IVAL(param) & DIRECTION_WITH_SELECT, 1);
}

static Status cm_bp_to_line_end(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)keystr;
    (void)finished;
    return bf_to_line_end(sess->active_buffer, IVAL(param) & DIRECTION_WITH_SELECT);
}

static Status cm_bp_to_next_word(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)keystr;
    (void)finished;
    return bf_to_next_word(sess->active_buffer, IVAL(param) & DIRECTION_WITH_SELECT);
}

static Status cm_bp_to_prev_word(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)keystr;
    (void)finished;
    return bf_to_prev_word(sess->active_buffer, IVAL(param) & DIRECTION_WITH_SELECT);
}

static Status cm_bp_to_buffer_start(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)keystr;
    (void)finished;
    return bf_to_buffer_start(sess->active_buffer, IVAL(param) & DIRECTION_WITH_SELECT);
}

static Status cm_bp_to_buffer_end(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)keystr;
    (void)finished;
    return bf_to_buffer_end(sess->active_buffer, IVAL(param) & DIRECTION_WITH_SELECT);
}

static Status cm_bp_change_page(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)keystr;
    (void)finished;
    return bf_change_page(sess->active_buffer, IVAL(param));
}

static Status cm_bp_goto_matching_bracket(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;
    return bf_jump_to_matching_bracket(sess->active_buffer);
}

static Status cm_buffer_insert_char(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)keystr;
    (void)finished;
    return bf_insert_character(sess->active_buffer, SVAL(param), 1);
}

static Status cm_buffer_delete_char(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;
    return bf_delete_character(sess->active_buffer);
}

static Status cm_buffer_backspace(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    if (!bf_selection_started(sess->active_buffer)) {
        if (bp_at_buffer_start(&sess->active_buffer->pos)) {
            return STATUS_SUCCESS;
        }

        Status status = bf_change_char(sess->active_buffer, &sess->active_buffer->pos, DIRECTION_LEFT, 1);
        RETURN_IF_FAIL(status);
    }

    return bf_delete_character(sess->active_buffer);
}

static Status cm_buffer_delete_word(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;
    return bf_delete_word(sess->active_buffer);
}

static Status cm_buffer_delete_prev_word(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;
    return bf_delete_prev_word(sess->active_buffer);
}

static Status cm_buffer_insert_line(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;
    return bf_insert_character(sess->active_buffer, "\n", 1);
}

static Status cm_buffer_select_all_text(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;
    return bf_select_all_text(sess->active_buffer);
}

static Status cm_buffer_copy_selected_text(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    TextSelection text_selection;

    Status status = bf_copy_selected_text(sess->active_buffer, &text_selection);

    RETURN_IF_FAIL(status);

    if (text_selection.str_len == 0) {
        return STATUS_SUCCESS; 
    }

    se_set_clipboard(sess, text_selection);

    return status;
}

static Status cm_buffer_cut_selected_text(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    TextSelection text_selection;

    Status status = bf_cut_selected_text(sess->active_buffer, &text_selection);

    RETURN_IF_FAIL(status);

    if (text_selection.str_len == 0) {
        return STATUS_SUCCESS; 
    }

    se_set_clipboard(sess, text_selection);

    return status;
}

static Status cm_buffer_paste_text(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    if (sess->clipboard.str == NULL) {
        return STATUS_SUCCESS;
    }

    return bf_insert_textselection(sess->active_buffer, &sess->clipboard, 1);
}

static Status cm_buffer_undo(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    Buffer *buffer = sess->active_buffer;
    return bc_undo(&buffer->changes, buffer);
}

static Status cm_buffer_redo(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    Buffer *buffer = sess->active_buffer;
    return bc_redo(&buffer->changes, buffer);
}

static Status cm_buffer_vert_move_lines(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)keystr;
    (void)finished;

    Buffer *buffer = sess->active_buffer;
    return bf_vert_move_lines(buffer, IVAL(param));
}

static Status cm_buffer_duplicate_selection(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    if (se_prompt_active(sess) &&
        pr_get_prompt_type(sess->prompt) == PT_FIND) {
        return cm_buffer_toggle_search_direction(sess, param, keystr, finished);
    }

    Buffer *buffer = sess->active_buffer;
    return bf_duplicate_selection(buffer);
}

static Status cm_buffer_indent(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)keystr;
    (void)finished;

    Buffer *buffer = sess->active_buffer;
    Range range;

    if (bf_get_range(buffer, &range) &&
        range.end.line_no != range.start.line_no) {
        return bf_indent(buffer, IVAL(param));
    }

    return bf_insert_character(sess->active_buffer, "\t", 1);
}

static Status cm_buffer_save_file(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    Buffer *buffer = sess->active_buffer;
    Status status = STATUS_SUCCESS;
    int file_path_exists = fi_has_file_path(&buffer->file_info);
    int file_exists_on_disk = fi_file_exists(&buffer->file_info);
    char *file_path;

    if (!file_path_exists) {
        cm_cmd_input_prompt(sess, PT_SAVE_FILE, "Save As:", NULL, 0);

        if (pr_prompt_cancelled(sess->prompt)) {
            return STATUS_SUCCESS;
        }

        file_path = pr_get_prompt_content(sess->prompt);

        if (file_path == NULL) {
            return st_get_error(ERR_OUT_OF_MEMORY, "Out of memory - Unable to process input");
        } else if (*file_path == '\0') {
            status = st_get_error(ERR_INVALID_FILE_PATH, "Invalid file path \"%s\"", file_path);
            free(file_path);
            return status;
        } 

        char *processed_path = fi_process_path(file_path);
        free(file_path);

        if (processed_path == NULL) {
            return st_get_error(ERR_OUT_OF_MEMORY, 
                                "Out of memory - "
                                "Unable to process input");
        }

        file_path = processed_path; 
    } else if (file_exists_on_disk) {
        file_path = buffer->file_info.abs_path;
    } else {
        file_path = buffer->file_info.rel_path;
    }

    status = bf_write_file(buffer, file_path);
    
    if (!STATUS_IS_SUCCESS(status)) {
        if (!file_path_exists) {
            free(file_path);
        }

        return status;
    }

    if (!file_path_exists || !file_exists_on_disk) {
        FileInfo tmp = buffer->file_info;
        status = fi_init(&buffer->file_info, file_path);
        fi_free(&tmp);

        if (!file_path_exists) {
            free(file_path);
        }

        RETURN_IF_FAIL(status);
    } else {
        fi_refresh_file_attributes(&buffer->file_info);
    }

    char msg[MAX_MSG_SIZE];
    snprintf(msg, MAX_MSG_SIZE, "Save successful: %zu lines, %zu bytes written", bf_lines(buffer), bf_length(buffer));
    se_add_msg(sess, msg);

    return status;
}

static void cm_generate_find_prompt(const BufferSearch *search, char prompt_text[MAX_CMD_PROMPT_LENGTH])
{
    const char *type = "";

    if (search->search_type == BST_REGEX) {
        type = " (regex)";
    }

    const char *direction = "";

    if (!search->opt.forward) {
        direction = " (backwards)";
    }

    const char *case_sensitive = "";

    if (!search->opt.case_insensitive) {
        case_sensitive = " (case sensitive)";
    }

    snprintf(prompt_text, MAX_CMD_PROMPT_LENGTH, "Find%s%s%s:", type, direction, case_sensitive);
}

static Status cm_prerpare_search(Session *sess, const BufferPos *start_pos)
{
    Buffer *buffer = sess->active_buffer;

    char prompt_text[MAX_CMD_PROMPT_LENGTH];
    cm_generate_find_prompt(&buffer->search, prompt_text);

    cm_cmd_input_prompt(sess, PT_FIND, prompt_text, sess->search_history, 1);

    if (pr_prompt_cancelled(sess->prompt)) {
        return STATUS_SUCCESS;
    }

    char *pattern = pr_get_prompt_content(sess->prompt);

    if (pattern == NULL) {
        return st_get_error(ERR_OUT_OF_MEMORY, "Out of memory - Unable to process input");
    } else if (*pattern == '\0') {
        free(pattern);
        return STATUS_SUCCESS;
    } 

    Status status = se_add_search_to_history(sess, pattern);

    if (!STATUS_IS_SUCCESS(status)) {
        free(pattern);    
        return status;
    }

    return bs_reinit(&buffer->search, start_pos, pattern, strlen(pattern));
}

static Status cm_buffer_find(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    RETURN_IF_FAIL(cm_prerpare_search(sess, NULL));

    if (pr_prompt_cancelled(sess->prompt)) {
        return STATUS_SUCCESS;
    }

    return cm_buffer_find_next(sess, param, keystr, finished);
}

static Status cm_buffer_find_next(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    Buffer *buffer = sess->active_buffer;

    if (buffer->search.opt.pattern == NULL) {
        return STATUS_SUCCESS;
    }

    int find_prev = IVAL(param);

    if (find_prev) {
        buffer->search.opt.forward ^= 1;
    }

    int found_match;

    Status status = bs_find_next(&buffer->search, &buffer->pos, &found_match);

    if (STATUS_IS_SUCCESS(status)) {
        if (found_match) {
            if ((buffer->search.opt.forward && 
                 bp_compare(&buffer->search.last_match_pos, &buffer->pos) == -1) ||
                (!buffer->search.opt.forward &&
                 bp_compare(&buffer->search.last_match_pos, &buffer->pos) == 1)) {
                se_add_msg(sess, "Search wrapped");
            }

            status = bf_set_bp(buffer, &buffer->search.last_match_pos);
        } else {
            char msg[MAX_MSG_SIZE];
            snprintf(msg, MAX_MSG_SIZE, "Unable to find pattern: \"%s\"", buffer->search.opt.pattern);
            se_add_msg(sess, msg);
        }
    }

    if (find_prev) {
        buffer->search.opt.forward ^= 1;
    }

    return status;
}

static Status cm_buffer_toggle_search_direction(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    if (!se_prompt_active(sess) || 
        pr_get_prompt_type(sess->prompt) != PT_FIND) {
        return STATUS_SUCCESS;
    }

    Buffer *buffer = sess->active_buffer->next;
    buffer->search.opt.forward ^= 1;

    char prompt_text[MAX_CMD_PROMPT_LENGTH];
    cm_generate_find_prompt(&buffer->search, prompt_text);

    return pr_set_prompt_text(sess->prompt, prompt_text);
}

static Status cm_buffer_toggle_search_type(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    if (!se_prompt_active(sess) || 
        pr_get_prompt_type(sess->prompt) != PT_FIND) {
        return STATUS_SUCCESS;
    }

    Buffer *buffer = sess->active_buffer->next;

    if (buffer->search.search_type == BST_TEXT) {
        buffer->search.search_type = BST_REGEX;
    } else {
        buffer->search.search_type = BST_TEXT;
    }

    char prompt_text[MAX_CMD_PROMPT_LENGTH];
    cm_generate_find_prompt(&buffer->search, prompt_text);

    return pr_set_prompt_text(sess->prompt, prompt_text);
}

static Status cm_buffer_toggle_search_case(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    if (!se_prompt_active(sess) || 
        pr_get_prompt_type(sess->prompt) != PT_FIND) {
        return STATUS_SUCCESS;
    }

    Buffer *buffer = sess->active_buffer->next;
    buffer->search.opt.case_insensitive ^= 1;

    char prompt_text[MAX_CMD_PROMPT_LENGTH];
    cm_generate_find_prompt(&buffer->search, prompt_text);

    return pr_set_prompt_text(sess->prompt, prompt_text);
}

static Status cm_prepare_replace(Session *sess, char **rep_text_ptr, size_t *rep_length)
{
    cm_cmd_input_prompt(sess, PT_REPLACE, "Replace With:", sess->replace_history, 1);

    if (pr_prompt_cancelled(sess->prompt)) {
        return STATUS_SUCCESS;
    }

    char *rep_text = pr_get_prompt_content(sess->prompt);

    if (rep_text == NULL) {
        return st_get_error(ERR_OUT_OF_MEMORY, "Out of memory - Unable to process input");
    } 

    *rep_length = strlen(rep_text);

    Buffer *buffer = sess->active_buffer;
    Status status = rp_replace_init(&buffer->search, rep_text, *rep_length);

    if (!STATUS_IS_SUCCESS(status)) {
        free(rep_text);    
        return status;
    }

    if (*rep_text != '\0') {
        status = se_add_replace_to_history(sess, rep_text);

        if (!STATUS_IS_SUCCESS(status)) {
            free(rep_text);    
            return status;
        }
    }

    *rep_text_ptr = rep_text;

    return status;
}

static Status cm_buffer_replace(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    Buffer *buffer = sess->active_buffer;
    RETURN_IF_FAIL(cm_prerpare_search(sess, &buffer->pos));

    if (pr_prompt_cancelled(sess->prompt)) {
        return STATUS_SUCCESS;
    }

    char *rep_text = NULL;
    size_t rep_length = 0;

    Status status = cm_prepare_replace(sess, &rep_text, &rep_length);

    if (pr_prompt_cancelled(sess->prompt) || !STATUS_IS_SUCCESS(status)) {
        return status;
    }
    
    int find_prev = IVAL(param);

    if (find_prev) {
        buffer->search.opt.forward ^= 1;
    }

    int found_match;
    QuestionRespose response = QR_NONE;
    BufferSearch *search = &buffer->search;
    size_t match_num = 0;
    size_t replace_num = 0;
   
    do {
        found_match = 0;
        status = bs_find_next(search, &buffer->pos, &found_match);

        if (found_match) {
            match_num++;
            status = bf_set_bp(buffer, &search->last_match_pos);

            if (!STATUS_IS_SUCCESS(status)) {
                break;
            }

            if (response != QR_ALL) {
                buffer->select_start = buffer->pos;
                bp_advance_to_offset(&buffer->select_start, buffer->pos.offset + bs_match_length(search));
                update_display(sess);

                response = cm_question_prompt(sess, PT_REPLACE, "Replace (Yes|no|all):", 
                                              QR_YES | QR_NO | QR_ALL, QR_YES);

                if (response == QR_ALL) {
                    status = bc_start_grouped_changes(&buffer->changes);

                    if (!STATUS_IS_SUCCESS(status)) {
                        break;
                    }
                }
            }

            if (response == QR_ERROR) {
                status = st_get_error(ERR_OUT_OF_MEMORY, "Out of memory - Unable to process input");
                break;
            } else if (response == QR_CANCEL) {
                break;
            } else if (response == QR_YES || response == QR_ALL) {
                status = rp_replace_current_match(buffer, rep_text, rep_length);
                replace_num++;
            }

            if (search->opt.forward) {
                if (search->last_match_pos.offset < search->start_pos.offset &&
                    buffer->pos.offset >= search->start_pos.offset) {
                    break;
                }
            } else {
                status = bf_set_bp(buffer, &buffer->search.last_match_pos);
            }
        }
    } while (STATUS_IS_SUCCESS(status) && found_match);

    if (find_prev) {
        buffer->search.opt.forward ^= 1;
    }

    bf_select_reset(buffer);

    if (bc_grouped_changes_started(&buffer->changes)) {
        bc_end_grouped_changes(&buffer->changes);
    }

    if (!STATUS_IS_SUCCESS(status)) {
        return status;
    }

    char msg[MAX_MSG_SIZE];

    if (match_num == 0) {
        snprintf(msg, MAX_MSG_SIZE, "Unable to find pattern \"%s\"", search->opt.pattern);
    } else if (replace_num == 0) {
        snprintf(msg, MAX_MSG_SIZE, "No occurrences replaced");
    } else {
        snprintf(msg, MAX_MSG_SIZE, "%zu occurrences replaced", replace_num);
    }

    se_add_msg(sess, msg);

    return status;
}

static Status cm_buffer_goto_line(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    Regex line_no_regex = { .regex_pattern = "[0-9]+", .modifiers = 0 };
    Buffer *prompt_buffer = pr_get_prompt_buffer(sess->prompt);
    bf_set_mask(prompt_buffer, &line_no_regex);

    cm_cmd_input_prompt(sess, PT_GOTO, "Line:", sess->lineno_history, 0);

    bf_remove_mask(prompt_buffer);

    if (pr_prompt_cancelled(sess->prompt)) {
        return STATUS_SUCCESS;
    }

    char *input = pr_get_prompt_content(sess->prompt);
    Status status = STATUS_SUCCESS;

    if (input == NULL) {
        return st_get_error(ERR_OUT_OF_MEMORY, "Out of memory - "
                            "Unable to process input");
    } else if (*input != '\0') {
        status = se_add_lineno_to_history(sess, input);

        if (!STATUS_IS_SUCCESS(status)) {
            free(input);
            return status;
        }

        errno = 0;

        size_t line_no = strtoull(input, NULL, 10);

        if (errno) {
            status = st_get_error(ERR_INVALID_LINE_NO,
                                  "Invalid line number \"%s\"",
                                  input);
        } else {
            status = bf_goto_line(sess->active_buffer, line_no);
        }
    } else {
        free(input);
    }

    return status;
}

static Status cm_session_open_file(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    cm_cmd_input_prompt(sess, PT_OPEN_FILE, "Open:", NULL, 0);

    if (pr_prompt_cancelled(sess->prompt)) {
        return STATUS_SUCCESS;
    }

    Status status;
    int buffer_index;

    char *input = pr_get_prompt_content(sess->prompt);

    if (input == NULL) {
        return st_get_error(ERR_OUT_OF_MEMORY, "Out of memory - Unable to process input");
    } else if (*input == '\0') {
        status = st_get_error(ERR_INVALID_FILE_PATH, "Invalid file path \"%s\"", input);
    } else {
        status = se_get_buffer_index_by_path(sess, input, &buffer_index);

        if (STATUS_IS_SUCCESS(status) && buffer_index == -1) {
            status = se_add_new_buffer(sess, input); 

            if (STATUS_IS_SUCCESS(status)) {
                buffer_index = sess->buffer_num - 1;
            }
        }
    }

    free(input);
    RETURN_IF_FAIL(status);

    se_set_active_buffer(sess, buffer_index);

    return STATUS_SUCCESS;
}

static Status cm_session_add_empty_buffer(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    RETURN_IF_FAIL(se_add_new_empty_buffer(sess));
    se_set_active_buffer(sess, sess->buffer_num - 1);

    return STATUS_SUCCESS;
}

static Status cm_session_change_tab(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)keystr;
    (void)finished;

    if (sess->buffer_num < 2) {
        return STATUS_SUCCESS;
    }

    size_t new_active_buffer_index;

    if (IVAL(param) == DIRECTION_RIGHT) {
        new_active_buffer_index = (sess->active_buffer_index + 1) % sess->buffer_num;
    } else {
        if (sess->active_buffer_index == 0) {
            new_active_buffer_index = sess->buffer_num - 1; 
        } else {
            new_active_buffer_index = sess->active_buffer_index - 1;
        }
    }

    se_set_active_buffer(sess, new_active_buffer_index);

    return STATUS_SUCCESS;
}

static Status cm_session_save_all(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    
    size_t start_buffer_index;
    
    se_get_buffer_index(sess, sess->active_buffer, &start_buffer_index);
    
    Status status = STATUS_SUCCESS;
    Buffer *buffer = sess->buffers;
    size_t buffer_save_num = 0;
    size_t buffer_index = 0;
    int re_enable_msgs = se_disable_msgs(sess);

    while (buffer != NULL) {
        if (buffer->is_dirty) {
            se_set_active_buffer(sess, buffer_index);
            
            status = cm_buffer_save_file(sess, INT_VAL(0), keystr, finished);
            
            if (!STATUS_IS_SUCCESS(status)) {
                break;
            }
            
            buffer_save_num++;
        }
        
        buffer = buffer->next;
        buffer_index++;
    }
    
    se_set_active_buffer(sess, start_buffer_index);
    
    if (re_enable_msgs) {
        se_enable_msgs(sess);
    }
    
    if (STATUS_IS_SUCCESS(status) &&
        buffer_save_num > 0) {
        char msg[MAX_MSG_SIZE];
        snprintf(msg, MAX_MSG_SIZE, "Save successful: "
                 "%zu buffers saved", buffer_save_num);
        se_add_msg(sess, msg);
    }

    return status;
}

static Status cm_session_close_buffer(Session *sess, Value param, const char *keystr, int *finished)
{
    int allow_no_buffers = IVAL(param);
    Buffer *buffer = sess->active_buffer;

    if (buffer->is_dirty) {
        char prompt_text[50];
        char *fmt = "Save changes to %.*s (Y/n)?";
        snprintf(prompt_text, sizeof(prompt_text), fmt, 
                 sizeof(prompt_text) - strlen(fmt) + 3, buffer->file_info.file_name);

        QuestionRespose response = cm_question_prompt(sess, PT_SAVE_FILE, prompt_text,
                                                      QR_YES | QR_NO, QR_YES);

        if (response == QR_ERROR) {
            return st_get_error(ERR_OUT_OF_MEMORY, "Out of memory - "
                                "Unable to process input");
        } else if (response == QR_CANCEL) {
            return STATUS_SUCCESS;
        } else if (response == QR_YES) {
            RETURN_IF_FAIL(cm_buffer_save_file(sess, INT_VAL(0), keystr, finished));
        }

        if (pr_prompt_cancelled(sess->prompt)) {
            return STATUS_SUCCESS;
        }
    }

    se_remove_buffer(sess, buffer);

    if (sess->buffer_num == 0 && !allow_no_buffers) {
        return cm_session_add_empty_buffer(sess, INT_VAL(0), keystr, finished); 
    }

    return STATUS_SUCCESS;
}

static Status cm_session_run_command(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    cm_cmd_input_prompt(sess, PT_COMMAND, "Command:", sess->command_history, 0);

    if (pr_prompt_cancelled(sess->prompt)) {
        return STATUS_SUCCESS;
    }

    char *input = pr_get_prompt_content(sess->prompt);
    Status status = STATUS_SUCCESS;

    if (input == NULL) {
        return st_get_error(ERR_OUT_OF_MEMORY, "Out of memory - Unable to process input");
    } else if (*input != '\0') {
        status = se_add_cmd_to_history(sess, input);

        if (!STATUS_IS_SUCCESS(status)) {
            free(input);
            return status;
        }

        status = cp_parse_config_string(sess, CL_BUFFER, input);
    } else {
        free(input);
    }

    return status;
}

static Status cm_previous_cmd_entry(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    return pr_previous_entry(sess->prompt);
}

static Status cm_next_cmd_entry(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    return pr_next_entry(sess->prompt);
}

static Status cm_finished_processing_input(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)sess;
    (void)param;
    (void)keystr;

    *finished = 1;

    return STATUS_SUCCESS;
}

static Status cm_session_change_buffer(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    cm_cmd_input_prompt(sess, PT_BUFFER, "Buffer:", sess->buffer_history, 0);

    if (pr_prompt_cancelled(sess->prompt)) {
        return STATUS_SUCCESS;
    }

    char *input = pr_get_prompt_content(sess->prompt);
    Status status = STATUS_SUCCESS;

    if (input == NULL) {
        return st_get_error(ERR_OUT_OF_MEMORY, "Out of memory - "
                            "Unable to process input");
    } else if (*input != '\0') {
        status = se_add_buffer_to_history(sess, input);

        if (!STATUS_IS_SUCCESS(status)) {
            free(input);
            return status;
        }

        const Buffer *buffer;
        status = cm_determine_buffer(sess, input, &buffer);

        if (!STATUS_IS_SUCCESS(status)) {
            return status;
        }

        size_t buffer_index;

        if (!se_get_buffer_index(sess, buffer, &buffer_index)) {
            assert(!"Buffer has no valid buffer index");
        }

        se_set_active_buffer(sess, buffer_index);
    } else {
        free(input);
    }
    
    return status;
}

static Status cm_determine_buffer(Session *sess, const char *input, 
                                  const Buffer **buffer_ptr)
{
    Prompt *prompt = sess->prompt;
    size_t input_len = strlen(input);

    RegexInstance regex;
    RegexResult regex_result; 
    Regex numeric_regex = { 
        .regex_pattern = "^\\s*([0-9]+)\\s*$",
        .modifiers = 0
    };

    RETURN_IF_FAIL(re_compile(&regex, &numeric_regex));
    Status status = re_exec(&regex_result, &regex, input, input_len, 0);
    re_free_instance(&regex);
    RETURN_IF_FAIL(status);

    int is_numeric = (regex_result.return_code == 2);

    if (is_numeric) {
        char *group_str;
        RETURN_IF_FAIL(re_get_group(&regex_result, input, input_len, 1, &group_str));
        errno = 0;
        size_t buffer_index = strtoull(input, NULL, 10);
        free(group_str);

        if (errno == 0 && 
            buffer_index-- > 0 &&
            se_is_valid_buffer_index(sess, buffer_index)) {
            *buffer_ptr = se_get_buffer(sess, buffer_index);
            return STATUS_SUCCESS;  
        }
    }

    RETURN_IF_FAIL(pc_run_prompt_completer(sess, prompt, 0));
    size_t suggestion_num = list_size(prompt->suggestions);

    if (suggestion_num < 2) {
        return st_get_error(ERR_NO_BUFFERS_MATCH,
                            "No buffers match \"%s\"",
                            input);
    }

    const PromptSuggestion *suggestion = list_get(prompt->suggestions, 0);

    if (!(suggestion->rank == SR_EXACT_MATCH ||
          suggestion_num == 2)) {
        return st_get_error(ERR_MULTIPLE_BUFFERS_MATCH, 
                            "Multiple (%zu) buffers match \"%s\"",
                            suggestion_num - 1, input);
    }

    *buffer_ptr = suggestion->data;
    return STATUS_SUCCESS;
}

static Status cm_suspend(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    suspend_display();
    raise(SIGTSTP);
    resize_display(sess);

    return STATUS_SUCCESS;
}

static Status cm_session_end(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    pr_prompt_set_cancelled(sess->prompt, 0);

    while (sess->buffer_num > 0) {
        RETURN_IF_FAIL(cm_session_close_buffer(sess, INT_VAL(1), keystr, finished));

        if (pr_prompt_cancelled(sess->prompt)) {
            return STATUS_SUCCESS;
        }
    }  

    *finished = 1;

    return STATUS_SUCCESS;
}

static QuestionRespose cm_question_prompt(Session *sess, PromptType prompt_type, const char *question, 
                                          QuestionRespose allowed_answers, QuestionRespose default_answer)
{
    QuestionRespose response = QR_NONE;

    do {
        cm_cmd_input_prompt(sess, prompt_type, question, NULL, 0);

        if (pr_prompt_cancelled(sess->prompt)) {
            return QR_CANCEL;
        }

        char *input = pr_get_prompt_content(sess->prompt);

        if (input == NULL) {
            return QR_ERROR;
        } else if ((allowed_answers & default_answer) && *input == '\0') {
            response = default_answer;
        } else if ((allowed_answers & QR_YES) && (*input == 'y' || *input == 'Y')) {
            response = QR_YES;
        } else if ((allowed_answers & QR_NO) && (*input == 'n' || *input == 'N')) {
            response = QR_NO;
        } else if ((allowed_answers & QR_ALL) && (*input == 'a' || *input == 'A')) {
            response = QR_ALL;
        }

        free(input);
    } while (response == QR_NONE);

    return response;
}

static Status cm_cmd_input_prompt(Session *sess, PromptType prompt_type, 
                                  const char *prompt_text, List *history,
                                  int show_last_cmd)
{
    RETURN_IF_FAIL(se_make_prompt_active(sess, prompt_type, prompt_text, 
                                         history, show_last_cmd));
    cm_update_command_function(sess, "<Up>", cm_previous_cmd_entry);
    cm_update_command_function(sess, "<Down>", cm_next_cmd_entry);
    cm_update_command_function(sess, "<Enter>", cm_finished_processing_input);
    cm_update_command_function(sess, "<Escape>", cm_cancel_cmd_input_prompt);

    if (pc_has_prompt_completer(prompt_type)) {
        cm_update_command_function(sess, "<Tab>", cm_run_input_completion);
        cm_update_command_function(sess, "<S-Tab>", cm_run_input_completion);
    }

    se_exclude_command_type(sess, CMDT_CMD_INPUT);

    update_display(sess);
    ip_process_input(sess);

    se_enable_command_type(sess, CMDT_CMD_INPUT);
    cm_update_command_function(sess, "<Up>", cm_bp_change_line);
    cm_update_command_function(sess, "<Down>", cm_bp_change_line);
    cm_update_command_function(sess, "<Enter>", cm_buffer_insert_line);
    cm_update_command_function(sess, "<Escape>", cm_session_end);

    if (pc_has_prompt_completer(prompt_type)) {
        cm_update_command_function(sess, "<Tab>", cm_buffer_insert_char);
        cm_update_command_function(sess, "<S-Tab>", cm_buffer_indent);
    }

    se_end_prompt(sess);

    return STATUS_SUCCESS;
}

static Status cm_cancel_cmd_input_prompt(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;

    pr_prompt_set_cancelled(sess->prompt, 1);
    *finished = 1;

    return STATUS_SUCCESS;
}

static int cm_update_command_function(Session *sess, const char *keystr, CommandHandler new_command_handler)
{
    Command *command = hashmap_get(sess->keymap, keystr);

    if (command == NULL) {
        return 0;
    }

    command->command_handler = new_command_handler;

    return 1;
}

static Status cm_run_input_completion(Session *sess, Value param, const char *keystr, int *finished)
{
    (void)param;
    (void)keystr;
    (void)finished;

    if (!se_prompt_active(sess)) {
        return STATUS_SUCCESS;
    }

    Prompt *prompt = sess->prompt;
    Status status = STATUS_SUCCESS;
    const char *prev_key = se_get_prev_key(sess);

    int prev_key_is_completer = 
        strncmp(prev_key, "<Tab>", MAX_KEY_STR_SIZE) == 0 ||
        strncmp(prev_key, "<S-Tab>", MAX_KEY_STR_SIZE) == 0;

    if (prev_key_is_completer) {
        if (strncmp(keystr, "<Tab>", MAX_KEY_STR_SIZE) == 0) {
            status = pr_show_next_suggestion(prompt);
        } else if (strncmp(keystr, "<S-Tab>", MAX_KEY_STR_SIZE) == 0) {
            status = pr_show_previous_suggestion(prompt);
        }
    } else {
        int reverse = (strncmp(keystr, "<S-Tab>", MAX_KEY_STR_SIZE) == 0);
        status = pc_run_prompt_completer(sess, prompt, reverse);
    }

    if (STATUS_IS_SUCCESS(status) &&
        pc_show_suggestion_prompt(prompt->prompt_type)) {
        pr_show_suggestion_prompt(prompt);
    }

    return status;
}

