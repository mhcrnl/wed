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
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include "config.h"
#include "variable.h"
#include "hashmap.h"
#include "util.h"
#include "command.h"

#define CFG_LINE_ALLOC 512
#define CFG_FILE_NAME "wedrc"
#define CFG_SYSTEM_DIR "/etc"

static const Session *curr_sess = NULL;

static int populate_default_config(HashMap *);
static char *get_config_line(FILE *);
static int process_config_line(char *, char **, char **);
static int get_bool_value(char *, Value *);

static int (*conversion_functions[])(char *, Value *) = {
    get_bool_value
};

static const ConfigVariableDescriptor default_config[] = {
    { "linewrap", "lw", BOOL_VAL_STRUCT(1), NULL, NULL }
};

void set_config_session(Session *sess)
{
    curr_sess = sess;
}

Status init_config(Session *sess)
{
    HashMap *config = sess->config;

    if (config == NULL) {
        size_t var_num = sizeof(default_config) / sizeof(ConfigVariableDescriptor);
        config = sess->config = new_sized_hashmap(var_num * 4);
    }

    if (!populate_default_config(config)) {
        /* TODO raise error here */
    }
    
    char *system_config_path = CFG_SYSTEM_DIR "/" CFG_FILE_NAME;

    if (access(system_config_path, F_OK) != -1) {
        RETURN_IF_FAIL(load_config(sess, system_config_path));
    }

    Status status = STATUS_SUCCESS;

    char *home_path = getenv("HOME"); 

    if (home_path != NULL) {
        size_t user_config_path_size = strlen(home_path) + strlen("/." CFG_FILE_NAME) + 1;
        char *user_config_path = alloc(user_config_path_size);
        snprintf(user_config_path, user_config_path_size, "%s/.%s", home_path, CFG_FILE_NAME);
        *(user_config_path + user_config_path_size - 1) = '\0';

        if (access(user_config_path, F_OK) != -1) {
            status = load_config(sess, user_config_path);
        }

        free(user_config_path);
    }

    return status;
}

void free_config(HashMap *config)
{
    if (config == NULL) {
        return;
    }

    size_t var_num = sizeof(default_config) / sizeof(ConfigVariableDescriptor);

    for (size_t k = 0; k < var_num; k++) {
        ConfigVariableDescriptor *cvd = hashmap_get(config, default_config[k].name);

        if (cvd == NULL) {
            continue;
        }

        free_value(cvd->default_value);
        free(cvd);
    }

    free_hashmap(config);
}

static int populate_default_config(HashMap *config)
{
    size_t var_num = sizeof(default_config) / sizeof(ConfigVariableDescriptor);
    ConfigVariableDescriptor *clone = alloc(sizeof(default_config));

    for (size_t k = 0; k < var_num; k++, clone++) {
        memcpy(clone, &default_config[k], sizeof(ConfigVariableDescriptor));
        clone->default_value = deep_copy_value(clone->default_value);

        if (!(hashmap_set(config, clone->name, clone) && 
              hashmap_set(config, clone->short_name, clone))) {
            return 0;
        }
    }

    return 1;
}

Status load_config(Session *sess, char *config_file_path)
{
    FILE *config_file = fopen(config_file_path, "rb");

    if (config_file == NULL) {
        return raise_param_error(ERR_UNABLE_TO_OPEN_FILE, STR_VAL(config_file_path));
    } 

    Status status = STATUS_SUCCESS;
    size_t line_no = 0;
    char *line, *var, *val;

    while (!feof(config_file)) {
        line = get_config_line(config_file);

        if (ferror(config_file)) {
            free(line);
            status = raise_param_error(ERR_UNABLE_TO_READ_FILE, STR_VAL(config_file_path));
            break;
        } 

        line_no++;

        if (!process_config_line(line, &var, &val)) {
            free(line);
            continue;
        }

        status = set_session_var(sess, var, val);

        free(line);

        if (!is_success(status)) {
            char *error_msg = get_error_msg(status.error);

            if (error_msg != NULL) {
                char *new_error_msg = alloc(MAX_ERROR_MSG_SIZE);
                snprintf(new_error_msg, MAX_ERROR_MSG_SIZE, "%s on line %zu: %s", config_file_path, line_no, error_msg);
                free_error(status.error);
                status = raise_param_error(ERR_INVALID_CONFIG_ENTRY, STR_VAL(new_error_msg));
                free(new_error_msg);
                free(error_msg);
            }
             
            break;
        }

    }

    fclose(config_file);

    return status;
}

static char *get_config_line(FILE *file)
{
    size_t allocated = CFG_LINE_ALLOC;
    size_t line_size = 0;
    char *line = alloc(allocated);
    char *iter = line;
    int c;

    while ((c = fgetc(file)) != EOF) {
        if (line_size++ == allocated) {
            line = ralloc(line, allocated *= 2); 
            iter = line + line_size - 1;
        }
        if (c == '\n') {
            break;
        }

        *iter++ = c;
    }

    *iter = '\0';

    return line;
}

static int process_config_line(char *line, char **var, char **val)
{
    char *c = line;

    while (*c) {
        if (*c == '#' || *c == ';') {
            return 0;
        } else if (!isspace(*c)) {
            break;
        }

        c++;
    }

    if (*c) {
        *var = c;
    } else {
        return 0;
    }

    while (*c) {
        if (isspace(*c)) {
            *c++ = '\0';
            continue;
        } else if (*c == '=') {
            break; 
        }

        c++;
    }

    if (!(*c && *c == '=')) {
        return 0;
    } else {
        *c++ = '\0';
    }

    while (*c && isspace(*c)) {
        c++;
    }

    if (*c) {
        *val = c;
    } else {
        return 0;
    }

    char *last_space = NULL;
   
    while (*(++c)) {
        if (isspace(*c)) {
            if (last_space == NULL) {
                last_space = c;
            }
        } else {
            last_space = NULL;
        }
    } 

    if (last_space != NULL) {
        *last_space = '\0';
    }

    return 1;
}

static int get_bool_value(char *svalue, Value *value)
{
    if (svalue == NULL || value == NULL) {
        return 0;
    }

    value->type = VAL_TYPE_BOOL;

    if (strncmp(svalue, "true", 5) == 0 || strncmp(svalue, "1", 2) == 0) {
        value->val.ival = 1;
    } else if (strncmp(svalue, "false", 6) == 0 || strncmp(svalue, "0", 2) == 0) {
        value->val.ival = 0;
    } else {
        return 0;
    }

    return 1;
}

Status set_session_var(Session *sess, char *var_name, char *val)
{
    if (sess == NULL || var_name == NULL || val == NULL) {
        return raise_param_error(ERR_INVALID_VAR, STR_VAL(var_name));
    }

    ConfigVariableDescriptor *var = hashmap_get(sess->config, var_name);

    if (var == NULL) {
        return raise_param_error(ERR_INVALID_VAR, STR_VAL(var_name));
    }

    Value value;
    
    if (!conversion_functions[var->default_value.type](val, &value)) {
        return raise_param_error(ERR_INVALID_VAL, STR_VAL(val));
    }

    if (var->custom_validator != NULL) {
        if (!var->custom_validator(value)) {
            /* TODO It would be useful to know why the value isn't valid */
            return raise_param_error(ERR_INVALID_VAL, STR_VAL(val));
        }
    }

    Value old_value = var->default_value;
    var->default_value = value; 

    if (var->on_change_event != NULL) {
        return var->on_change_event(sess, old_value, value);
    }

    return STATUS_SUCCESS;
}

int config_bool(char *var_name)
{
    ConfigVariableDescriptor *var = hashmap_get(curr_sess->config, var_name);

    if (var == NULL || var->default_value.type != VAL_TYPE_BOOL) {
        /* TODO Add error to session error queue */
        return 0;
    }

    return var->default_value.val.ival;
}
