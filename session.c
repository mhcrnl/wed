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

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "session.h"
#include "status.h"
#include "util.h"
#include "buffer.h"
#include "command.h"
#include "config.h"

#define MAX_EMPTY_BUFFER_NAME_SIZE 20

static Status se_add_to_history(List *, char *);
static void se_determine_filetype(Session *, Buffer *);
static void se_determine_fileformat(Session *, Buffer *);
static int se_is_valid_config_def(Session *, HashMap *, ConfigType, const char *);

Session *se_new(void)
{
    Session *sess = malloc(sizeof(Session));
    RETURN_IF_NULL(sess);
    memset(sess, 0, sizeof(Session));

    return sess;
}

int se_init(Session *sess, char *buffer_paths[], int buffer_num)
{
    if ((sess->error_buffer = bf_new_empty("errors", sess->config)) == NULL) {
        return 0;
    }

    if ((sess->cmd_prompt.cmd_buffer = bf_new_empty("commands", sess->config)) == NULL) {
        return 0;
    }

    if ((sess->msg_buffer = bf_new_empty("messages", sess->config)) == NULL) {
        return 0;
    }

    if ((sess->search_history = list_new()) == NULL) {
        return 0;
    }

    if ((sess->replace_history = list_new()) == NULL) {
        return 0;
    }

    if ((sess->command_history = list_new()) == NULL) {
        return 0;
    }

    if (!cm_init_keymap(sess)) {
        return 0;
    }

    if ((sess->filetypes = new_hashmap()) == NULL) {
        return 0;
    }

    if ((sess->syn_defs = new_hashmap()) == NULL) {
        return 0;
    }

    if ((sess->themes = new_hashmap()) == NULL) {
        return 0;
    }

    Theme *default_theme = th_get_default_theme();

    if (default_theme == NULL) {
        return 0;
    }

    if (!hashmap_set(sess->themes, "default", default_theme)) {
        return 0;
    }

    if ((sess->cfg_buffer_stack = list_new()) == NULL) {
        return 0;
    }

    se_add_error(sess, cf_init_session_config(sess));

    for (int k = 1; k < buffer_num; k++) {
        se_add_error(sess, se_add_new_buffer(sess, buffer_paths[k]));
    }

    if (sess->buffer_num == 0) {
        se_add_new_empty_buffer(sess);
    }

    if (!se_set_active_buffer(sess, 0)) {
        return 0;
    }

    Buffer *cmd_buffer = sess->cmd_prompt.cmd_buffer;
    cf_set_var(CE_VAL(sess, cmd_buffer), CL_BUFFER, CV_LINEWRAP, INT_VAL(0));

    se_enable_msgs(sess);

    sess->initialised = 1;

    return 1;
}

void se_free(Session *sess)
{
    if (sess == NULL) {
        return;
    }

    Buffer *buffer = sess->buffers;
    Buffer *tmp;

    while (buffer != NULL) {
        tmp = buffer->next;
        bf_free(buffer);
        buffer = tmp;
    }

    cm_free_keymap(sess);
    bf_free_textselection(&sess->clipboard);
    cf_free_config(sess->config);
    bf_free(sess->cmd_prompt.cmd_buffer);
    free(sess->cmd_prompt.cmd_text);
    bf_free(sess->error_buffer);
    bf_free(sess->msg_buffer);
    list_free_all(sess->search_history, NULL);
    list_free_all(sess->replace_history, NULL);
    list_free_all(sess->command_history, NULL);
    free_hashmap_values(sess->filetypes, (void (*)(void *))ft_free);
    free_hashmap(sess->filetypes);
    free_hashmap_values(sess->syn_defs, (void (*)(void *))sy_free_def);
    free_hashmap(sess->syn_defs);
    free_hashmap_values(sess->themes, NULL);
    free_hashmap(sess->themes);
    list_free(sess->cfg_buffer_stack);

    free(sess);
}

int se_add_buffer(Session *sess, Buffer *buffer)
{
    assert(buffer != NULL);

    if (buffer == NULL) {
        return 0;
    }

    int re_enable_msgs = se_disable_msgs(sess);

    se_determine_filetype(sess, buffer);
    se_determine_syntaxtype(sess, buffer);
    se_determine_fileformat(sess, buffer);

    if (re_enable_msgs) {
        se_enable_msgs(sess);
    }

    sess->buffer_num++;

    if (sess->buffers == NULL) {
        sess->buffers = buffer;
        return 1;
    }

    Buffer *buff = sess->buffers;

    do {
        if (buff->next == NULL) {
            buff->next = buffer;
            break;
        }

        buff = buff->next;
    } while (1);
    
    return 1;
}

int se_set_active_buffer(Session *sess, size_t buffer_index)
{
    assert(sess->buffers != NULL);
    assert(buffer_index < sess->buffer_num);

    if (sess->buffers == NULL || buffer_index >= sess->buffer_num) {
        return 0;
    }

    Buffer *buffer = sess->buffers;
    size_t iter = 0;

    while (iter < buffer_index) {
         buffer = buffer->next;
         iter++;
    }

    sess->active_buffer = buffer;
    sess->active_buffer_index = buffer_index;

    return 1;
}

Buffer *se_get_buffer(const Session *sess, size_t buffer_index)
{
    assert(sess->buffers != NULL);
    assert(buffer_index < sess->buffer_num);

    if (sess->buffers == NULL || buffer_index >= sess->buffer_num) {
        return NULL;
    }

    Buffer *buffer = sess->buffers;

    while (buffer_index-- != 0) {
        buffer = buffer->next;    
    }

    return buffer;
}

int se_remove_buffer(Session *sess, Buffer *to_remove)
{
    assert(sess->buffers != NULL);
    assert(to_remove != NULL);

    if (sess->buffers == NULL || to_remove == NULL) {
        return 0;
    }

    Buffer *buffer = sess->buffers;
    Buffer *prev = NULL;
    size_t buffer_index = 0;

    while (buffer != NULL && to_remove != buffer) {
        prev = buffer;    
        buffer = buffer->next;
        buffer_index++;
    }

    if (buffer == NULL) {
        return 0;
    }

    if (prev != NULL) {
        if (buffer->next != NULL) {
            prev->next = buffer->next; 

            if (sess->active_buffer_index == buffer_index) {
                sess->active_buffer = buffer->next;
            }
        } else {
            prev->next = NULL;
            sess->active_buffer = prev;

            if (sess->active_buffer_index == buffer_index) {
                sess->active_buffer_index--;
            }
        }
    } else if (sess->active_buffer == buffer) {
        if (buffer->next != NULL) {
            sess->buffers = buffer->next;
            sess->active_buffer = sess->buffers;
        } else {
            sess->buffers = NULL;
            sess->active_buffer = NULL;
        } 
    }

    sess->buffer_num--;

    bf_free(buffer);

    return 1;
}

Status se_make_cmd_buffer_active(Session *sess, const char *prompt_text, List *history, int show_last_cmd)
{
    RETURN_IF_FAIL(se_update_cmd_prompt_text(sess, prompt_text));

    sess->cmd_prompt.cmd_buffer->next = sess->active_buffer;
    sess->active_buffer = sess->cmd_prompt.cmd_buffer;

    sess->cmd_prompt.cancelled = 0;
    sess->cmd_prompt.history = history;
    
    const char *cmd_text = NULL;
    
    if (history != NULL) {
        sess->cmd_prompt.history_index = list_size(history);

        if (show_last_cmd && sess->cmd_prompt.history_index > 0) {
            cmd_text = list_get(history, --sess->cmd_prompt.history_index); 
        }
    }

    RETURN_IF_FAIL(bf_set_text(sess->cmd_prompt.cmd_buffer, cmd_text));

    return bf_select_all_text(sess->cmd_prompt.cmd_buffer);
}

Status se_update_cmd_prompt_text(Session *sess, const char *text)
{
    assert(!is_null_or_empty(text));

    if (sess->cmd_prompt.cmd_text != NULL) {
        free(sess->cmd_prompt.cmd_text);
    }

    sess->cmd_prompt.cmd_text = strdupe(text);
    
    if (text != NULL && sess->cmd_prompt.cmd_text == NULL) {
        return st_get_error(ERR_OUT_OF_MEMORY, "Out of memory - Unable to set prompt text");
    }

    return STATUS_SUCCESS;
}

int se_end_cmd_buffer_active(Session *sess)
{
    assert(sess->active_buffer != NULL);

    if (sess->active_buffer == NULL) {
        return 0;
    }

    sess->active_buffer = sess->cmd_prompt.cmd_buffer->next;

    return 1;
}

int se_cmd_buffer_active(const Session *sess)
{
    assert(sess->active_buffer != NULL);

    if (sess->active_buffer == NULL) {
        return 0;
    }

    return sess->active_buffer == sess->cmd_prompt.cmd_buffer;
}

char *se_get_cmd_buffer_text(const Session *sess)
{
    return bf_to_string(sess->cmd_prompt.cmd_buffer);
}

void se_set_clipboard(Session *sess, TextSelection clipboard)
{
    if (sess->clipboard.str != NULL) {
        bf_free_textselection(&sess->clipboard);
    }

    sess->clipboard = clipboard;
}

void se_exclude_command_type(Session *sess, CommandType cmd_type)
{
    sess->exclude_cmd_types |= cmd_type;
}

void se_enable_command_type(Session *sess, CommandType cmd_type)
{
    sess->exclude_cmd_types &= ~cmd_type;
}

int se_command_type_excluded(const Session *sess, CommandType cmd_type)
{
    return sess->exclude_cmd_types & cmd_type;
}

int se_add_error(Session *sess, Status error)
{
    if (STATUS_IS_SUCCESS(error)) {
        return 0;
    }

    Buffer *error_buffer = sess->error_buffer;
    char error_msg[MAX_ERROR_MSG_SIZE];

    snprintf(error_msg, MAX_ERROR_MSG_SIZE, "Error %d: %s", error.error_code, error.msg);    
    st_free_status(error);

    if (!bp_at_buffer_start(&error_buffer->pos)) {
        bf_insert_character(error_buffer, "\n", 1);
    }

    bf_insert_string(error_buffer, error_msg, strnlen(error_msg, MAX_ERROR_MSG_SIZE), 1);

    return 1;
}

int se_has_errors(const Session *sess)
{
    return !bf_is_empty(sess->error_buffer);
}

void se_clear_errors(Session *sess)
{
    bf_clear(sess->error_buffer);
}

int se_add_msg(Session *sess, const char *msg)
{
    assert(!is_null_or_empty(msg));

    if (msg == NULL) {
        return 0; 
    } else if (!se_msgs_enabled(sess)) {
        return 1;
    }

    Buffer *msg_buffer = sess->msg_buffer;

    if (!bp_at_buffer_start(&msg_buffer->pos)) {
        bf_insert_character(msg_buffer, "\n", 1);
    }

    bf_insert_string(msg_buffer, msg, strnlen(msg, MAX_MSG_SIZE), 1);

    return 1;
}

int se_has_msgs(const Session *sess)
{
    return !bf_is_empty(sess->msg_buffer);
}

void se_clear_msgs(Session *sess)
{
    bf_clear(sess->msg_buffer);
}

Status se_add_new_buffer(Session *sess, const char *file_path)
{
    if (file_path == NULL || strnlen(file_path, 1) == 0) {
        return st_get_error(ERR_INVALID_FILE_PATH, "Invalid file path - \"%s\"", file_path);
    }

    FileInfo file_info;
    Buffer *buffer = NULL;
    Status status;

    RETURN_IF_FAIL(fi_init(&file_info, file_path));

    if (fi_is_directory(&file_info)) {
        status = st_get_error(ERR_FILE_IS_DIRECTORY, "%s is a directory", file_info.file_name);
        goto cleanup;
    } else if (fi_is_special(&file_info)) {
        status = st_get_error(ERR_FILE_IS_SPECIAL, "%s is not a regular file", file_info.file_name);
        goto cleanup;
    }

    buffer = bf_new(&file_info, sess->config);

    if (buffer == NULL) {
        status = st_get_error(ERR_OUT_OF_MEMORY, 
                           "Out of memory - Unable to "
                           "create buffer for file %s", 
                           file_info.file_name);
        goto cleanup;
    }

    status = bf_load_file(buffer);

    if (!STATUS_IS_SUCCESS(status)) {
        goto cleanup;
    }

    se_add_buffer(sess, buffer);

    return STATUS_SUCCESS;

cleanup:
    fi_free(&file_info);
    bf_free(buffer);

    return status;
}

Status se_add_new_empty_buffer(Session *sess)
{
    char empty_buf_name[MAX_EMPTY_BUFFER_NAME_SIZE];
    snprintf(empty_buf_name, MAX_EMPTY_BUFFER_NAME_SIZE, "[new %zu]", ++sess->empty_buffer_num);

    Buffer *buffer = bf_new_empty(empty_buf_name, sess->config);

    if (buffer == NULL) {
        return st_get_error(ERR_OUT_OF_MEMORY, 
                         "Out of memory - Unable to "
                         "create empty buffer");
    }   

    se_add_buffer(sess, buffer);

    return STATUS_SUCCESS;
}

Status se_get_buffer_index(const Session *sess, const char *file_path, int *buffer_index_ptr)
{
    assert(!is_null_or_empty(file_path));
    assert(buffer_index_ptr != NULL);

    FileInfo file_info;
    RETURN_IF_FAIL(fi_init(&file_info, file_path));

    Buffer *buffer = sess->buffers;
    *buffer_index_ptr = -1;
    int buffer_index = 0;

    while (buffer != NULL) {
        if (fi_equal(&buffer->file_info, &file_info)) {
            *buffer_index_ptr = buffer_index; 
            break;
        } 

        buffer = buffer->next;
        buffer_index++;
    }

    fi_free(&file_info);

    return STATUS_SUCCESS;
}

static Status se_add_to_history(List *history, char *text)
{
    assert(!is_null_or_empty(text));

    size_t size = list_size(history);

    if (size > 0 && strcmp(list_get(history, size - 1), text) == 0) {
        return STATUS_SUCCESS;
    }

    if (!list_add(history, text)) {
        return st_get_error(ERR_OUT_OF_MEMORY, "Out of memory - Unable save search history");
    }

    return STATUS_SUCCESS;
}

Status se_add_search_to_history(Session *sess, char *search_text)
{
    return se_add_to_history(sess->search_history, search_text);
}

Status se_add_replace_to_history(Session *sess, char *replace_text)
{
    return se_add_to_history(sess->replace_history, replace_text);
}

Status se_add_cmd_to_history(Session *sess, char *cmd_text)
{
    return se_add_to_history(sess->command_history, cmd_text);
}

Status se_add_filetype_def(Session *sess, FileType *file_type)
{
    assert(file_type != NULL);    

    FileType *existing = hashmap_get(sess->filetypes, file_type->name);

    if (!hashmap_set(sess->filetypes, file_type->name, file_type)) {
        return st_get_error(ERR_OUT_OF_MEMORY, "Out Of Memory - Unable to save filetype");
    }

    if (existing != NULL) {
        ft_free(existing);
    }

    Buffer *buffer = sess->buffers;
    int re_enable_msgs = se_disable_msgs(sess);
    int matches;

    while (buffer != NULL) {
        if (is_null_or_empty(cf_string(buffer->config, CV_FILETYPE))) {
            se_add_error(sess, ft_matches(file_type, &buffer->file_info, &matches));

            if (matches) {
                se_add_error(sess, cf_set_var(CE_VAL(sess, buffer), CL_BUFFER, 
                                              CV_FILETYPE, STR_VAL(file_type->name)));
            }
        }

        buffer = buffer->next;
    }

    if (re_enable_msgs) {
        se_enable_msgs(sess);
    }

    return STATUS_SUCCESS;
}

static void se_determine_filetype(Session *sess, Buffer *buffer)
{
    HashMap *filetypes = sess->filetypes;
    size_t key_num = hashmap_size(filetypes);
    const char **keys = hashmap_get_keys(filetypes);

    if (key_num == 0) {
        return;
    } else if (keys == NULL) {
        se_add_error(sess, st_get_error(ERR_OUT_OF_MEMORY, "Out Of Memory - "
                                        "Unable to generate filetypes set"));
    }

    FileType *file_type;
    int matches;

    for (size_t k = 0; k < key_num; k++) {
        file_type = hashmap_get(filetypes, keys[k]);

        if (file_type != NULL) {
            se_add_error(sess, ft_matches(file_type, &buffer->file_info, &matches));

            if (matches) {
                se_add_error(sess, cf_set_var(CE_VAL(sess, buffer), CL_BUFFER, 
                                              CV_FILETYPE, STR_VAL(file_type->name)));
                break;
            }
        }
    }

    free(keys);
}

int se_msgs_enabled(const Session *sess)
{
    return sess->msgs_enabled;
}

int se_enable_msgs(Session *sess)
{
    int currently_enabled = sess->msgs_enabled;
    sess->msgs_enabled = 1;    
    return currently_enabled;
}

int se_disable_msgs(Session *sess)
{
    int currently_enabled = sess->msgs_enabled;
    sess->msgs_enabled = 0;
    return currently_enabled;
}

Status se_add_syn_def(Session *sess, SyntaxDefinition *syn_def, const char *syn_name)
{
    assert(syn_def != NULL);
    assert(!is_null_or_empty(syn_name));

    SyntaxDefinition *existing = hashmap_get(sess->syn_defs, syn_name);

    if (!hashmap_set(sess->syn_defs, syn_name, syn_def)) {
        return st_get_error(ERR_OUT_OF_MEMORY, "Out Of Memory - "
                            "Unable to save syntax definition");
    }

    if (existing != NULL) {
        sy_free_def(existing);
    }

    return STATUS_SUCCESS;
}

void se_determine_syntaxtype(Session *sess, Buffer *buffer)
{
    if (!cf_bool(sess->config, CV_SYNTAX)) {
        return;
    }

    const char *syn_type = cf_string(buffer->config, CV_SYNTAXTYPE);

    if (!is_null_or_empty(syn_type)) {
        return;
    }

    const char *file_type = cf_string(buffer->config, CV_FILETYPE);

    if (is_null_or_empty(file_type)) {
        return;
    }

    if (!se_is_valid_syntaxtype(sess, file_type)) {
        return;
    }

    se_add_error(sess, cf_set_var(CE_VAL(sess, buffer), CL_BUFFER, 
                                  CV_SYNTAXTYPE, STR_VAL((char *)file_type)));
}

static void se_determine_fileformat(Session *sess, Buffer *buffer)
{
    FileFormat file_format = bf_detect_fileformat(buffer);
    cf_set_var(CE_VAL(sess, buffer), CL_BUFFER, CV_FILEFORMAT, 
               STR_VAL((char *)bf_get_fileformat_str(file_format)));
}

int se_is_valid_syntaxtype(Session *sess, const char *syn_type)
{
    if (is_null_or_empty(syn_type)) {
        return 1;
    }

    return se_is_valid_config_def(sess, sess->syn_defs, CT_SYNTAX, syn_type);
}

static int se_is_valid_config_def(Session *sess, HashMap *defs, 
                                  ConfigType config_type, const char *def_name)
{
    const void *def = hashmap_get(defs, def_name);

    if (def != NULL) {
        return 1;
    }

    cf_load_config_def(sess, config_type, def_name);

    def = hashmap_get(defs, def_name);

    return def != NULL;
}

const SyntaxDefinition *se_get_syntax_def(const Session *sess, const Buffer *buffer)
{
    if (!cf_bool(sess->config, CV_SYNTAX)) {
        return NULL;
    }

    const char *syn_type = cf_string(buffer->config, CV_SYNTAXTYPE);

    return hashmap_get(sess->syn_defs, syn_type);
}

int se_is_valid_theme(Session *sess, const char *theme)
{
    return se_is_valid_config_def(sess, sess->themes, CT_THEME, theme);
}

Status se_add_theme(Session *sess, Theme *theme, const char *theme_name)
{
    assert(theme != NULL);    
    assert(!is_null_or_empty(theme_name));

    if (strncmp(theme_name, "default", 8) == 0) {
        return st_get_error(ERR_OVERRIDE_DEFAULT_THEME, 
                            "Cannot override default theme");
    }

    Theme *existing = hashmap_get(sess->themes, theme_name);

    if (!hashmap_set(sess->themes, theme_name, theme)) {
        return st_get_error(ERR_OUT_OF_MEMORY, "Out Of Memory - "
                            "Unable to save theme definition");
    }

    if (existing != NULL) {
        free(existing);
    }

    return STATUS_SUCCESS;
}

const Theme *se_get_active_theme(const Session *sess)
{
    const char *theme_name = cf_string(sess->config, CV_THEME);

    assert(!is_null_or_empty(theme_name));
    
    const Theme *theme = hashmap_get(sess->themes, theme_name);

    assert(theme != NULL);

    return theme;
}

int se_initialised(const Session *sess)
{
    return sess->initialised;
}

