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
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "value.h"
#include "util.h"
#include "status.h"

#define VALUE_STR_CONVERT_SIZE 100

const char *va_get_value_type(Value value)
{
    return va_value_type_string(value.type);
}

const char *va_value_type_string(ValueType value_type)
{
    switch (value_type) {
        case VAL_TYPE_BOOL:
            return "Boolean";
        case VAL_TYPE_INT:
            return "Integer";
        case VAL_TYPE_FLOAT:
            return "Float";
        case VAL_TYPE_STR:
            return "String";
        case VAL_TYPE_REGEX:
            return "Regex";
        case VAL_TYPE_SHELL_COMMAND:
            return "Shell Command";
        default:
            break;
    }

    assert(!"Invalid value type");

    return "";
}

const char *va_multi_value_type_string(ValueType value_types)
{
    static char value_type_str[1024] = { 0 };
    size_t offset = 0;

    if (value_types & VAL_TYPE_BOOL) {
        offset += snprintf(value_type_str + offset,
                           sizeof(value_type_str) - offset,
                           "%s%s", (offset > 0 ? " or " : ""), "Boolean");
    }
    if (value_types & VAL_TYPE_INT) {
        offset += snprintf(value_type_str + offset,
                           sizeof(value_type_str) - offset,
                           "%s%s", (offset > 0 ? " or " : ""), "Integer");
    }
    if (value_types & VAL_TYPE_FLOAT) {
        offset += snprintf(value_type_str + offset,
                           sizeof(value_type_str) - offset,
                           "%s%s", (offset > 0 ? " or " : ""), "Float");
    }
    if (value_types & VAL_TYPE_STR) {
        offset += snprintf(value_type_str + offset,
                           sizeof(value_type_str) - offset,
                           "%s%s", (offset > 0 ? " or " : ""), "String");
    }
    if (value_types & VAL_TYPE_REGEX) {
        offset += snprintf(value_type_str + offset,
                           sizeof(value_type_str) - offset,
                           "%s%s", (offset > 0 ? " or " : ""), "Regex");
    }
    if (value_types & VAL_TYPE_SHELL_COMMAND) {
        offset += snprintf(value_type_str + offset,
                           sizeof(value_type_str) - offset,
                           "%s%s", (offset > 0 ? " or " : ""),
                           "Shell Command");
    }

    assert(offset > 0);

    return value_type_str;
}

Status va_deep_copy_value(Value value, Value *new_val)
{
    if (!STR_BASED_VAL(value)) {
        *new_val = value;
        return STATUS_SUCCESS;
    }

    const char *curr_val = va_str_val(value);

    if (curr_val == NULL) {
        *new_val = value;
        return STATUS_SUCCESS;
    }

    char *str_val = strdup(curr_val);

    if (str_val == NULL) {
        return OUT_OF_MEMORY("Unable to copy value");
    }

    if (value.type == VAL_TYPE_STR) {
        *new_val = STR_VAL(str_val);
    } else if (value.type == VAL_TYPE_REGEX) {
        *new_val = REGEX_VAL(str_val, RVAL(value).modifiers);
    } else if (value.type == VAL_TYPE_SHELL_COMMAND) {
        *new_val = CMD_VAL(str_val);
    }

    return STATUS_SUCCESS;
}

char *va_to_string(Value value)
{
    switch (value.type) {
        case VAL_TYPE_STR:
            return strdup(SVAL(value));
        case VAL_TYPE_BOOL:
            return strdup(IVAL(value) ? "true" : "false");
        case VAL_TYPE_INT:
            {
                char *num_str = malloc(VALUE_STR_CONVERT_SIZE);

                if (num_str == NULL) {
                    return NULL;
                }

                snprintf(num_str, VALUE_STR_CONVERT_SIZE, "%ld", IVAL(value));

                return num_str;
            }
        case VAL_TYPE_FLOAT:
            {
                char *num_str = malloc(VALUE_STR_CONVERT_SIZE);

                if (num_str == NULL) {
                    return NULL;
                }

                snprintf(num_str, VALUE_STR_CONVERT_SIZE, "%f", FVAL(value));

                return num_str;
            }
        case VAL_TYPE_REGEX:
            return strdup(RVAL(value).regex_pattern);
        case VAL_TYPE_SHELL_COMMAND:
            return strdup(CVAL(value));
        default:
            assert(!"Invalid value type");
            break;
    }

    return NULL;
}

const char *va_str_val(Value value)
{
    if (value.type == VAL_TYPE_STR) {
        return SVAL(value);
    } else if (value.type == VAL_TYPE_REGEX) {
        return RVAL(value).regex_pattern;
    } else if (value.type == VAL_TYPE_SHELL_COMMAND) {
        return CVAL(value);
    }

    assert(!"Invalid value type");

    return NULL;
}

void va_free_value(Value value)
{
    if (!STR_BASED_VAL(value)) {
        return;
    }

    if (value.type == VAL_TYPE_STR) {
        free(SVAL(value));
    } else if (value.type == VAL_TYPE_REGEX) {
        free(RVAL(value).regex_pattern);
    } else if (value.type == VAL_TYPE_SHELL_COMMAND) {
        free(CVAL(value));
    }
}
