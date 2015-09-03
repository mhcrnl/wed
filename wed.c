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

#include <unistd.h>
#include <locale.h>
#include "display.h"
#include "util.h"
#include "session.h"
#include "buffer.h"
#include "display.h"
#include "input.h"
#include "file.h"
#include "config.h"

int parse_args(int argc, char *argv[], Session *sess)
{
    /* TODO Write this function. Probably using getopt */
    
    /*To stop compiler warnings */
    argc++;
    (void)argv;
    (void)sess;

    return 1;
}

/* TODO If a fatal error occurs when initializing then
 * print an error message to stderr. */
int main(int argc, char *argv[])
{
    Session *sess = se_new();

    if (sess == NULL) {
        fatal("Out of memory - Unable to create Session");
    }

    if (!parse_args(argc, argv, sess)) {
        return 1;
    }

    setlocale(LC_ALL, "");

    if (!se_init(sess, argv, argc)) {
        fatal("Unable to initialise session");
    }

    ip_edit(sess);

    se_free(sess);
    
    return 0;
}
