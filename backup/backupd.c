/* backupd.c -- replication-based backup daemon
 *
 * Copyright (c) 1994-2015 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <config.h>

#include <stdlib.h>
#include <syslog.h>

/* for config.c */
const int config_need_data = 0;

static void shut_down(int code)
{
    exit(code);
}

EXPORTED void fatal(const char* s, int code)
{
    static int recurse_code = 0;

    if (recurse_code) {
        /* We were called recursively. Just give up */
//        proc_cleanup();
        exit(recurse_code);
    }
    recurse_code = code;
//    if (sync_out) {
//        prot_printf(sync_out, "* Fatal error: %s\r\n", s);
//        prot_flush(sync_out);
//    }
    syslog(LOG_ERR, "Fatal error: %s", s);
    shut_down(code);
}

/*
 * run once when process is forked;
 * MUST NOT exit directly; must return with non-zero error code
 */
EXPORTED int service_init(int argc __attribute__((unused)),
                 char **argv __attribute__((unused)),
                 char **envp __attribute__((unused)))
{
    return -1;
}

/* Called by service API to shut down the service */
EXPORTED void service_abort(int error)
{
    shut_down(error);
}

/*
 * run for each accepted connection
 */
EXPORTED int service_main(int argc __attribute__((unused)),
                 char **argv __attribute__((unused)),
                 char **envp __attribute__((unused)))
{
    return -1;
}

