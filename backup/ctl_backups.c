/* ctl_backups.c -- tool for managing replication-based backup files
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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lib/cyrusdb.h"
#include "lib/exitcodes.h"

#include "imap/global.h"
#include "imap/imap_err.h"

#include "backup/backup.h"

EXPORTED void fatal(const char *error, int code)
{
    fprintf(stderr, "fatal error: %s\n", error);
    cyrus_done();
    exit(code);
}

static const char *argv0 = NULL;
static void usage(void)
{
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "    %s [options] compact [mode] backup...\n", argv0);
    fprintf(stderr, "    %s [options] list [list_opts] [[mode] backup...]\n", argv0);
    fprintf(stderr, "    %s [options] lock [lock_opts] [mode] backup\n", argv0);
    fprintf(stderr, "    %s [options] reindex [mode] backup...\n", argv0);
    fprintf(stderr, "    %s [options] verify [mode] backup...\n", argv0);

    fprintf(stderr, "\n%s\n",
            "Commands:\n"
            "    compact             # compact specified backups\n"
            "    list [list_opts]    # list backups (all if none specified)\n"
            "    lock [lock_opts]    # lock specified backup\n"
            "    reindex             # reindex specified backups\n"
            "    verify              # verify specified backups\n"
    );

    fprintf(stderr, "%s\n",
            "Options:\n"
            "    -C alt_config       # alternate config file\n"
            "    -F                  # force (run command even if not needed)\n"
            "    -S                  # stop on error\n"
            "    -v                  # verbose (repeat for more verbosity)\n"
            "    -w                  # wait for locks (don't skip locked backups)\n"
    );

    fprintf(stderr, "%s\n",
            "List options:\n"
            "    -t [hours]          # stale (no update in hours) backups only (default: 24)\n"
    );

    fprintf(stderr, "%s\n",
            "Lock options:\n"
            "    -c                  # exclusively create backup\n"
            "    -p                  # lock backup and wait for eof on stdin (default)\n"
            "    -s                  # lock backup and open index in sqlite3\n"
            "    -x command          # lock backup and execute command\n"
    );

    fprintf(stderr, "%s\n",
            "Modes:\n"
            "    -A                  # all known backups\n"
            "    -D                  # specified backups interpreted as domains\n"
            "    -P                  # specified backups interpreted as userid prefixes\n"
            "    -f                  # specified backups interpreted as filenames\n"
            "    -m                  # specified backups interpreted as mboxnames\n"
            "    -u                  # specified backups interpreted as userids (default)\n"
            "\n"
            "    Modes -A, -D, -P not available for all commands\n" /* FIXME which */
    );

    exit(EC_USAGE);
}

enum ctlbu_mode {
    CTLBU_MODE_UNSPECIFIED = 0,
    CTLBU_MODE_FILENAME,
    CTLBU_MODE_MBOXNAME,
    CTLBU_MODE_USERNAME,
    CTLBU_MODE_DOMAIN,
    CTLBU_MODE_PREFIX,
    CTLBU_MODE_ALL,
};

enum ctlbu_lock_mode {
    CTLBU_LOCK_MODE_UNSPECIFIED = 0,
    CTLBU_LOCK_MODE_PIPE,
    CTLBU_LOCK_MODE_SQL,
    CTLBU_LOCK_MODE_EXEC,
};

struct ctlbu_cmd_options {
    enum ctlbu_mode mode;
    enum ctlbu_lock_mode lock_mode;
    enum backup_open_nonblock wait;
    enum backup_open_create create;
    int verbose;
    int stop_on_error;
    int list_stale;
    int force;
    const char *lock_exec_cmd;
    const char *domain;
};

enum ctlbu_cmd {
    CTLBU_CMD_UNSPECIFIED = 0,
    CTLBU_CMD_COMPACT,
    CTLBU_CMD_DELETE,
    CTLBU_CMD_LIST,
    CTLBU_CMD_LOCK,
    CTLBU_CMD_MOVE,
    CTLBU_CMD_RECONSTRUCT,
    CTLBU_CMD_REINDEX,
    CTLBU_CMD_VERIFY,
};

static int ctlbu_skips_fails = 0;

/* same signature as foreach_cb */
static int cmd_compact_one(void *rock,
                           const char *userid, size_t userid_len,
                           const char *fname, size_t fname_len);
static int cmd_delete_one(void *rock,
                          const char *userid, size_t userid_len,
                          const char *fname, size_t fname_len);
static int cmd_list_one(void *rock,
                        const char *userid, size_t userid_len,
                        const char *fname, size_t fname_len);
static int cmd_lock_one(void *rock,
                        const char *userid, size_t userid_len,
                        const char *fname, size_t fname_len);
static int cmd_move_one(void *rock,
                        const char *userid, size_t userid_len,
                        const char *fname, size_t fname_len);
static int cmd_reindex_one(void *rock,
                           const char *userid, size_t userid_len,
                           const char *fname, size_t fname_len);
static int cmd_verify_one(void *rock,
                          const char *userid, size_t userid_len,
                          const char *fname, size_t fname_len);

static foreach_cb *const cmd_func[] = {
    NULL,
    cmd_compact_one,
    cmd_delete_one,
    cmd_list_one,
    cmd_lock_one,
    cmd_move_one,
    NULL, /* reconstruct one doesn't make sense */
    cmd_reindex_one,
    cmd_verify_one,
};

static int lock_run_pipe(const char *userid, const char *fname,
                         enum backup_open_nonblock nonblock,
                         enum backup_open_create create);
static int lock_run_sqlite(const char *userid, const char *fname,
                           enum backup_open_nonblock nonblock,
                           enum backup_open_create create);
static int lock_run_exec(const char *userid, const char *fname,
                         const char *cmd,
                         enum backup_open_nonblock nonblock,
                         enum backup_open_create create);

static enum ctlbu_cmd parse_cmd_string(const char *cmd)
{
    assert(cmd != NULL);

    switch(cmd[0]) {
    case 'c':
        if (strcmp(cmd, "compact") == 0) return CTLBU_CMD_COMPACT;
        break;
    case 'd':
        if (strcmp(cmd, "delete") == 0) return CTLBU_CMD_DELETE;
        break;
    case 'l':
        if (strcmp(cmd, "list") == 0) return CTLBU_CMD_LIST;
        if (strcmp(cmd, "lock") == 0) return CTLBU_CMD_LOCK;
        break;
    case 'm':
        if (strcmp(cmd, "move") == 0) return CTLBU_CMD_MOVE;
        break;
    case 'r':
        if (strcmp(cmd, "reconstruct") == 0) return CTLBU_CMD_RECONSTRUCT;
        if (strcmp(cmd, "reindex") == 0) return CTLBU_CMD_REINDEX;
        break;
    case 'v':
        if (strcmp(cmd, "verify") == 0) return CTLBU_CMD_VERIFY;
        break;
    };

    return CTLBU_CMD_UNSPECIFIED;
}

static void print_status(const char *cmd,
                         const char *userid, const char *fname,
                         int r)
{
    printf("%s %s: ", cmd, userid ? userid : fname);
    switch(r) {
        case 1:
            puts("skipped");
            break;
        case 0:
            puts("ok");
            break;
        case IMAP_MAILBOX_LOCKED:
            puts("locked");
            break;
        default:
            printf("failed (%s)\n", error_message(r));
            break;
    }
}

static int domain_filter(void *rock,
                         const char *key, size_t key_len,
                         const char *data __attribute__((unused)),
                         size_t data_len __attribute__((unused)))
{
    struct ctlbu_cmd_options *options = (struct ctlbu_cmd_options *) rock;
    char *userid = NULL;
    mbname_t *mbname = NULL;
    const char *domain = NULL;
    int doit = 0;

    /* input args might not be 0-terminated, so make a safe copy */
    if (!key_len) return 0;
    userid = xstrndup(key, key_len);

    mbname = mbname_from_userid(userid);
    domain = mbname_domain(mbname);
    if (!domain)
        domain = config_defdomain;

    if (domain)
        doit = !strcmp(domain, options->domain);

    mbname_free(&mbname);
    free(userid);

    return doit;
}

static void save_argv0(const char *s)
{
    const char *slash = strrchr(s, '/');
    if (slash)
        argv0 = slash + 1;
    else
        argv0 = s;
}

int main (int argc, char **argv)
{
    save_argv0(argv[0]);

    int opt, r = 0;
    const char *alt_config = NULL;
    enum ctlbu_cmd cmd = CTLBU_CMD_UNSPECIFIED;
    struct ctlbu_cmd_options options = {0};
    options.wait = BACKUP_OPEN_NONBLOCK;

    while ((opt = getopt(argc, argv, ":AC:DFPScfmpst:x:uvw")) != EOF) {
        switch (opt) {
        case 'A':
            if (options.mode != CTLBU_MODE_UNSPECIFIED) usage();
            options.mode = CTLBU_MODE_ALL;
            break;
        case 'C':
            alt_config = optarg;
            break;
        case 'D':
            if (options.mode != CTLBU_MODE_UNSPECIFIED) usage();
            options.mode = CTLBU_MODE_DOMAIN;
            break;
        case 'F':
            options.force = 1;
            break;
        case 'P':
            if (options.mode != CTLBU_MODE_UNSPECIFIED) usage();
            options.mode = CTLBU_MODE_PREFIX;
            break;
        case 'S':
            options.stop_on_error = 1;
            break;
        case 'c':
            options.create = BACKUP_OPEN_CREATE_EXCL;
            break;
        case 'f':
            if (options.mode != CTLBU_MODE_UNSPECIFIED) usage();
            options.mode = CTLBU_MODE_FILENAME;
            break;
        case 'm':
            if (options.mode != CTLBU_MODE_UNSPECIFIED) usage();
            options.mode = CTLBU_MODE_MBOXNAME;
            break;
        case 'p':
            if (options.lock_mode != CTLBU_LOCK_MODE_UNSPECIFIED) usage();
            options.lock_mode = CTLBU_LOCK_MODE_PIPE;
            break;
        case 's':
            if (options.lock_mode != CTLBU_LOCK_MODE_UNSPECIFIED) usage();
            options.lock_mode = CTLBU_LOCK_MODE_SQL;
            break;
        case 't':
            options.list_stale = atoi(optarg);
            if (!options.list_stale) usage();
            break;
        case 'u':
            if (options.mode != CTLBU_MODE_UNSPECIFIED) usage();
            options.mode = CTLBU_MODE_USERNAME;
            break;
        case 'v':
            options.verbose ++;
            break;
        case 'x':
            if (options.lock_mode != CTLBU_LOCK_MODE_UNSPECIFIED) usage();
            options.lock_mode = CTLBU_LOCK_MODE_EXEC;
            options.lock_exec_cmd = optarg;
            break;
        case 'w':
            options.wait = BACKUP_OPEN_BLOCK;
            break;
        case ':':
            if (optopt == 't') options.list_stale = 24;
            else usage();
            break;
        default:
            usage();
            break;
        }
    }

    /* get the command */
    if (optind == argc) usage();
    cmd = parse_cmd_string(argv[optind++]);
    if (cmd == CTLBU_CMD_UNSPECIFIED) usage();

    if (options.lock_mode != CTLBU_LOCK_MODE_UNSPECIFIED
        && cmd != CTLBU_CMD_LOCK)
        usage();

    switch (cmd) {
    /* list defaults to all */
    case CTLBU_CMD_LIST:
        if (options.mode == CTLBU_MODE_UNSPECIFIED && argc - optind == 0)
            options.mode = CTLBU_MODE_ALL;
        break;

    /* some commands only accept one backup at a time */
    case CTLBU_CMD_LOCK:
    case CTLBU_CMD_MOVE:
    case CTLBU_CMD_DELETE:
        if (options.mode == CTLBU_MODE_ALL) usage();
        if (options.mode == CTLBU_MODE_DOMAIN) usage();
        if (options.mode == CTLBU_MODE_PREFIX) usage();
        if (argc - optind > 1) usage();
        break;

    /* reconstruct doesn't accept named backups */
    case CTLBU_CMD_RECONSTRUCT:
        if (options.mode != CTLBU_MODE_UNSPECIFIED) usage();
        if (optind != argc) usage();
        break;

    default:
        break;
    }

    /* default mode is username */
    if (options.mode == CTLBU_MODE_UNSPECIFIED)
        options.mode = CTLBU_MODE_USERNAME;

    /* mode all doesn't want any named backups */
    if (options.mode == CTLBU_MODE_ALL && optind != argc) usage();

    cyrus_init(alt_config, "ctl_backups", 0, 0);

    if (cmd == CTLBU_CMD_RECONSTRUCT) {
        /* special handling for reconstruct */
        // FIXME
    }
    else if (options.mode == CTLBU_MODE_ALL) {
        /* loop over entire backups.db */
        struct db *backups_db = NULL;
        struct txn *tid = NULL;

        r = backupdb_open(&backups_db, &tid);

        if (!r)
            r = cyrusdb_foreach(backups_db, NULL, 0, NULL,
                                cmd_func[cmd], &options,
                                &tid);

        if (backups_db) {
            if (tid) cyrusdb_abort(backups_db, tid);
            cyrusdb_close(backups_db);
        }
    }
    else if (options.mode == CTLBU_MODE_DOMAIN) {
        /* loop over domains named on command line */
        struct db *backups_db = NULL;
        struct txn *tid = NULL;
        int i;

        r = backupdb_open(&backups_db, &tid);

        for (i = optind; i < argc && !r; i++) {
            options.domain = argv[i];

            r = cyrusdb_foreach(backups_db, NULL, 0,
                                domain_filter,
                                cmd_func[cmd], &options,
                                &tid);
        }

        if (backups_db) {
            if (tid) cyrusdb_abort(backups_db, tid);
            cyrusdb_close(backups_db);
        }

    }
    else if (options.mode == CTLBU_MODE_PREFIX) {
        /* loop over prefixes named on command line */
        struct db *backups_db = NULL;
        struct txn *tid = NULL;
        int i;

        r = backupdb_open(&backups_db, &tid);

        for (i = optind; i < argc && !r; i++) {
            r = cyrusdb_foreach(backups_db,
                                argv[i], strlen(argv[i]),
                                NULL,
                                cmd_func[cmd], &options,
                                &tid);
        }

        if (backups_db) {
            if (tid) cyrusdb_abort(backups_db, tid);
            cyrusdb_close(backups_db);
        }
    }
    else {
        /* loop over backups named on command line */
        struct buf userid = BUF_INITIALIZER;
        struct buf fname = BUF_INITIALIZER;
        int i;

        for (i = optind; i < argc; i++) {
            buf_reset(&userid);
            buf_reset(&fname);
            mbname_t *mbname = NULL;

            // FIXME error checking in here

            if (options.mode == CTLBU_MODE_USERNAME)
                mbname = mbname_from_userid(argv[i]);
            else if (options.mode == CTLBU_MODE_MBOXNAME)
                mbname = mbname_from_intname(argv[i]);

            if (mbname) {
                backup_get_paths(mbname, &fname, NULL, BACKUP_OPEN_NOCREATE);
                buf_setcstr(&userid, mbname_userid(mbname));
            }
            else
                buf_setcstr(&fname, argv[i]);

            if (cmd_func[cmd])
                r = cmd_func[cmd](&options,
                                  buf_cstring(&userid),
                                  buf_len(&userid),
                                  buf_cstring(&fname),
                                  buf_len(&fname));

            if (mbname) mbname_free(&mbname);

            if (r) break;
        }

        buf_free(&userid);
        buf_free(&fname);
    }

    backup_cleanup_staging_path();
    cyrus_done();
    exit(r || ctlbu_skips_fails ? EC_TEMPFAIL : EC_OK);
}

static int cmd_compact_one(void *rock,
                           const char *key, size_t key_len,
                           const char *data, size_t data_len)
{
    struct ctlbu_cmd_options *options = (struct ctlbu_cmd_options *) rock;
    char *userid = NULL;
    char *fname = NULL;
    int r = 0;

    /* input args might not be 0-terminated, so make a safe copy */
    if (key_len)
        userid = xstrndup(key, key_len);
    if (data_len)
        fname = xstrndup(data, data_len);

    r = backup_compact(fname, options->wait, options->force,
                       options->verbose, stdout);

    print_status("compact", userid, fname, r);

    if (userid) free(userid);
    if (fname) free(fname);

    if (r) ++ctlbu_skips_fails;
    if (r == IMAP_MAILBOX_LOCKED) r = 0;
    return options->stop_on_error ? r : 0;
}

static int cmd_delete_one(void *rock,
                          const char *userid, size_t userid_len,
                          const char *fname, size_t fname_len)
{
    struct ctlbu_cmd_options *options = (struct ctlbu_cmd_options *) rock;
    (void) options;
    fprintf(stderr, "unimplemented: %s %s[%zu] %s[%zu]\n", __func__,
            userid, userid_len, fname, fname_len);
    return -1;
}

static int cmd_list_one(void *rock,
                        const char *key, size_t key_len,
                        const char *data, size_t data_len)
{
    struct ctlbu_cmd_options *options = (struct ctlbu_cmd_options *) rock;
    struct backup *backup = NULL;
    struct backup_chunk *chunk = NULL;
    char *userid = NULL;
    char *fname = NULL;
    int r = 0;

    /* input args might not be 0-terminated, so make a safe copy */
    if (key_len)
        userid = xstrndup(key, key_len);
    if (data_len)
        fname = xstrndup(data, data_len);

    r = backup_open_paths(&backup, fname, NULL,
                          options->wait, BACKUP_OPEN_NOCREATE);
    if (!r)
        r = backup_verify(backup, BACKUP_VERIFY_QUICK, 0, NULL);

    if (r) {
        fprintf(stderr, "%s: %s\n", userid ? userid : fname, error_message(r));
        goto done;
    }

    if (options->list_stale) {
        chunk = backup_get_latest_chunk(backup);

        /* skip out early if it's not stale */
        if (chunk && time(NULL) - chunk->ts_end < 3600 * options->list_stale)
            goto done;
    }

    backup_printinfo(backup, userid, stdout, options->verbose);

done:
    if (chunk) backup_chunk_free(&chunk);
    if (backup) backup_close(&backup);
    if (userid) free(userid);
    if (fname) free(fname);

    if (r) ++ctlbu_skips_fails;
    if (r == IMAP_MAILBOX_LOCKED) r = 0;
    return options->stop_on_error ? r : 0;
}

static int cmd_lock_one(void *rock,
                        const char *key, size_t key_len,
                        const char *data, size_t data_len)
{
    struct ctlbu_cmd_options *options = (struct ctlbu_cmd_options *) rock;
    char *userid = NULL;
    char *fname = NULL;
    int r;

    /* input args might not be 0-terminated, so make a safe copy */
    if (key_len)
        userid = xstrndup(key, key_len);
    if (data_len)
        fname = xstrndup(data, data_len);

    switch (options->lock_mode) {
    case CTLBU_LOCK_MODE_UNSPECIFIED:
    case CTLBU_LOCK_MODE_PIPE:
        r = lock_run_pipe(userid, fname, options->wait, options->create);
        break;
    case CTLBU_LOCK_MODE_SQL:
        r = lock_run_sqlite(userid, fname, options->wait, options->create);
        break;
    case CTLBU_LOCK_MODE_EXEC:
        r = lock_run_exec(userid, fname, options->lock_exec_cmd,
                          options->wait, options->create);
        break;
    }

    if (userid) free(userid);
    if (fname) free(fname);

    /* don't care about stop_on_error: lock command only accepts one backup */
    if (r) ++ctlbu_skips_fails;
    return r;
}

static int cmd_move_one(void *rock,
                        const char *userid, size_t userid_len,
                        const char *fname, size_t fname_len)
{
    struct ctlbu_cmd_options *options = (struct ctlbu_cmd_options *) rock;
    (void) options;
    fprintf(stderr, "unimplemented: %s %s[%zu] %s[%zu]\n", __func__,
            userid, userid_len, fname, fname_len);
    return -1;
}

static int cmd_reindex_one(void *rock,
                           const char *key, size_t key_len,
                           const char *data, size_t data_len)
{
    struct ctlbu_cmd_options *options = (struct ctlbu_cmd_options *) rock;
    char *userid = NULL;
    char *fname = NULL;
    int r;

    /* input args might not be 0-terminated, so make a safe copy */
    if (key_len)
        userid = xstrndup(key, key_len);
    if (data_len)
        fname = xstrndup(data, data_len);

    r = backup_reindex(fname, options->wait, options->verbose, stdout);

    print_status("reindex", userid, fname, r);

    if (userid) free(userid);
    if (fname) free(fname);

    if (r) ++ctlbu_skips_fails;
    if (r == IMAP_MAILBOX_LOCKED) r = 0;
    return options->stop_on_error ? r : 0;
}

static int cmd_verify_one(void *rock,
                          const char *key, size_t key_len,
                          const char *data, size_t data_len)
{
    struct ctlbu_cmd_options *options = (struct ctlbu_cmd_options *) rock;
    struct backup *backup = NULL;
    char *userid = NULL;
    char *fname = NULL;
    int r;

    /* input args might not be 0-terminated, so make a safe copy */
    if (key_len)
        userid = xstrndup(key, key_len);
    if (data_len)
        fname = xstrndup(data, data_len);

    r = backup_open_paths(&backup, fname, NULL,
                          options->wait, BACKUP_OPEN_NOCREATE);

    if (!r) r = backup_verify(backup, BACKUP_VERIFY_FULL, options->verbose, stdout);

    print_status("verify", userid, fname, r);

    if (backup) backup_close(&backup);
    if (userid) free(userid);
    if (fname) free(fname);

    if (r) ++ctlbu_skips_fails;
    if (r == IMAP_MAILBOX_LOCKED) r = 0;
    return options->stop_on_error ? r : 0;
}

static int lock_run_pipe(const char *userid, const char *fname,
                         enum backup_open_nonblock nonblock,
                         enum backup_open_create create)
{
    printf("* Trying to obtain lock on %s...\n", userid ? userid : fname);

    struct backup *backup = NULL;
    int r;

    r = backup_open_paths(&backup, fname, NULL, nonblock, create);

    if (!r)
        r = backup_verify(backup, BACKUP_VERIFY_QUICK, 0, NULL);

    if (r) {
        printf("NO failed (%s)\n", error_message(r));
        return EC_SOFTWARE; // FIXME would something else be more appropriate?
    }

    printf("OK locked\n");

    /* wait until stdin closes */
    char buf[PROT_BUFSIZE] = {0};
    while (!feof(stdin))
        fgets(buf, sizeof(buf), stdin);

    r = backup_close(&backup);
    if (r) fprintf(stderr, "warning: backup_close() returned %i\n", r);

    return 0;
}

static int lock_run_sqlite(const char *userid, const char *fname,
                           enum backup_open_nonblock nonblock,
                           enum backup_open_create create)
{
    fprintf(stderr, "trying to obtain lock on %s...\n", userid ? userid : fname);

    struct backup *backup = NULL;
    const char *index_fname = NULL;
    int r, status;
    pid_t pid;

    r = backup_open_paths(&backup, fname, NULL, nonblock, create);

    if (!r)
        r = backup_verify(backup, BACKUP_VERIFY_QUICK, 0, NULL);

    if (r) {
        fprintf(stderr, "unable to lock %s: %s\n",
                userid ? userid : fname,
                error_message(r));
        return EC_SOFTWARE;
    }

    index_fname = backup_get_index_fname(backup);

    /* FIXME probably need to do something with signals here */

    pid = fork();

    switch (pid) {
    case -1:
        perror("fork");
        r = EC_SOFTWARE;
        break;

    case 0:
        /* child */
        fprintf(stderr, "execlp: %s %s\n", "sqlite3", index_fname);
        execlp("sqlite3", "sqlite3", index_fname, NULL);
        /* execlp never returns */
        perror("execlp sqlite3");
        _exit(EC_SOFTWARE);
        break;

    default:
        /* parent */
        waitpid(pid, &status, 0);
        if (WIFEXITED(status))
            r = WEXITSTATUS(status);
        else
            r = EC_SOFTWARE;
        break;
    }

    backup_close(&backup);
    return r;
}

static const char data_fname_env[] = "ctl_backups_lock_data_fname";
static const char index_fname_env[] = "ctl_backups_lock_index_fname";

static int lock_run_exec(const char *userid, const char *fname,
                         const char *cmd,
                         enum backup_open_nonblock nonblock,
                         enum backup_open_create create)
{
    fprintf(stderr, "trying to obtain lock on %s...\n", userid ? userid : fname);

    struct backup *backup = NULL;
    int r;

    r = backup_open_paths(&backup, fname, NULL, nonblock, create);

    if (!r)
        r = backup_verify(backup, BACKUP_VERIFY_QUICK, 0, NULL);

    if (r) {
        fprintf(stderr, "unable to lock %s: %s\n",
                userid ? userid : fname,
                error_message(r));
        return EC_SOFTWARE;
    }

    setenv(data_fname_env, fname, 1);
    setenv(index_fname_env, backup_get_index_fname(backup), 1);

    r = system(cmd);

    unsetenv(data_fname_env);
    unsetenv(index_fname_env);

    if (r == -1)
        r = EC_SOFTWARE;
    else if (WIFEXITED(r))
        r = WEXITSTATUS(r);
    else
        r = EC_SOFTWARE;

    backup_close(&backup);
    return r;
}
