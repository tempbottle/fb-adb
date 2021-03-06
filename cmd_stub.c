/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in
 *  the LICENSE file in the root directory of this source tree. An
 *  additional grant of patent rights can be found in the PATENTS file
 *  in the same directory.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <limits.h>
#include "util.h"
#include "child.h"
#include "xmkraw.h"
#include "ringbuf.h"
#include "ringbuf.h"
#include "proto.h"
#include "core.h"
#include "channel.h"
#include "adbenc.h"
#include "termbits.h"
#include "constants.h"
#include "timestamp.h"
#include "cmd_stub.h"

static void
send_exit_message(int status, struct fb_adb_sh* sh)
{
    struct msg_child_exit m;
    memset(&m, 0, sizeof (m));
    m.msg.type = MSG_CHILD_EXIT;
    m.msg.size = sizeof (m);
    if (WIFEXITED(status))
        m.exit_status = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        m.exit_status = 128 + WTERMSIG(status);

    queue_message_synch(sh, &m.msg);
}

static void
set_window_size(int fd, const struct window_size* ws)
{
    int ret;
    struct winsize wz = {
        .ws_row = ws->row,
        .ws_col = ws->col,
        .ws_xpixel = ws->xpixel,
        .ws_ypixel = ws->ypixel,
    };

    do {
        ret = ioctl(fd, TIOCSWINSZ, &wz);
    } while (ret == -1 && errno == EINTR);

    dbg("TIOCSWINSZ(%ux%u): %d", wz.ws_row, wz.ws_col, ret);
}

struct stub {
    struct fb_adb_sh sh;
    struct child* child;
};

static void
stub_process_msg(struct fb_adb_sh* sh, struct msg mhdr)
{
    if (mhdr.type == MSG_WINDOW_SIZE) {
        struct msg_window_size m;
        read_cmdmsg(sh, mhdr, &m, sizeof (m));
        dbgmsg(&m.msg, "recv");
        struct stub* stub = (struct stub*) sh;
        if (stub->child->pty_master)
            set_window_size(stub->child->pty_master->fd, &m.ws);

        return;
    }

    fb_adb_sh_process_msg(sh, mhdr);
}

static void
setup_pty(int master, int slave, void* arg)
{
    struct msg_shex_hello* shex_hello = arg;
    char* hello_end = (char*) shex_hello + shex_hello->msg.size;
    struct termios attr = { 0 };
    xtcgetattr(slave, &attr);
    if (shex_hello->ispeed)
        cfsetispeed(&attr, shex_hello->ispeed);
    if (shex_hello->ospeed)
        cfsetospeed(&attr, shex_hello->ospeed);

    const struct termbit* tb = &termbits[0];
    const struct termbit* tb_end = tb + nr_termbits;
    struct term_control* tc = &shex_hello->tctl[0];

    while ((char*)(tc+1) <= hello_end && tb < tb_end) {
        int cmp = strncmp(tc->name, tb->name, sizeof (tc->name));
        if (cmp < 0) {
            dbg("tc not present: %.*s", (int) sizeof (tc->name), tc->name);
            tc++;
            continue;
        }

        if (cmp > 0) {
            dbg("tc not sent: %s", tb->name);
            tb++;
            continue;
        }

        tcflag_t* flg = NULL;
        if (tb->thing == TERM_IFLAG) {
            flg = &attr.c_iflag;
        } else if (tb->thing == TERM_OFLAG) {
            flg = &attr.c_oflag;
        } else if (tb->thing == TERM_LFLAG) {
            flg = &attr.c_lflag;
        } else if (tb->thing == TERM_C_CC) {
            if (tc->value == shex_hello->posix_vdisable_value)
                attr.c_cc[tb->value] = _POSIX_VDISABLE;
            else
                attr.c_cc[tb->value] = tc->value;

            dbg("c_cc[%s] = %d", tb->name, tc->value);
        }

        if (flg) {
            if (tc->value) {
                dbg("pty %s: set", tb->name);
                *flg |= tb->value;
            } else {
                dbg("pty %s: reset", tb->name);
                *flg &= ~tb->value;
            }
        }

        tc++;
        tb++;
    }

    xtcsetattr(slave, &attr);
    if (shex_hello->have_ws) {
        dbg("ws %ux%u (%ux%u)",
            shex_hello->ws.row,
            shex_hello->ws.col,
            shex_hello->ws.xpixel,
            shex_hello->ws.ypixel);
        set_window_size(master, &shex_hello->ws);
    }
}

static void
read_child_arglist(size_t expected,
                   char*** out_argv,
                   const char** out_cwd)
{
    char** argv;
    const char* cwd = "/";

    if (expected >= SIZE_MAX / sizeof (*argv))
        die(EFBIG, "too many arguments");

    argv = xalloc(sizeof (*argv) * (1+expected));
    for (size_t argno = 0; argno < expected; ++argno) {
        SCOPED_RESLIST(rl_read_arg);
        struct msg_cmdline_argument* m;
        struct msg* mhdr = read_msg(0, read_all_adb_encoded);

        const char* argval;
        size_t arglen;

        if (mhdr->type == MSG_CMDLINE_ARGUMENT) {
            m = (struct msg_cmdline_argument*) mhdr;
            if (mhdr->size < sizeof (*m))
                die(ECOMM,
                    "bad handshake: MSG_CMDLINE_ARGUMENT size %u < %u",
                    (unsigned) mhdr->size,
                    (unsigned) sizeof (*m));

            argval = m->value;
            arglen = m->msg.size - sizeof (*m);
        } else if (mhdr->type == MSG_CMDLINE_DEFAULT_SH ||
                   mhdr->type == MSG_CMDLINE_DEFAULT_SH_LOGIN)
        {
            argval = getenv("SHELL");
            if (argval == NULL)
                argval = DEFAULT_SHELL;

            if (mhdr->type == MSG_CMDLINE_DEFAULT_SH_LOGIN)
                argval = xaprintf("-%s", argval);

            arglen = strlen(argval);
        } else if (mhdr->type == MSG_CMDLINE_ARGUMENT_JUMBO) {
            struct msg_cmdline_argument_jumbo* mj =
                (struct msg_cmdline_argument_jumbo*) mhdr;

            if (mhdr->size != sizeof (*mj))
                die(ECOMM,
                    "bad handshake: MSG_CMDLINE_ARGUMENT_JUMBO size %u != %u",
                    (unsigned) mhdr->size,
                    (unsigned) sizeof (*mj));

            arglen = mj->actual_size;
            void* buf = xalloc(arglen);
            size_t nr_read = read_all_adb_encoded(0, buf, arglen);
            if (nr_read != arglen)
                die(ECOMM, "peer disconnected");
            argval = buf;
        } else if (mhdr->type == MSG_CHDIR) {
            struct msg_chdir* mchd = (struct msg_chdir*) mhdr;
            reslist_pop_nodestroy(rl_read_arg);
            cwd = xaprintf("%.*s",
                           (int) (mhdr->size - sizeof (*mchd)),
                           mchd->dir);
            --argno;
            continue;
        } else {
            die(ECOMM,
                "bad handshake: unknown init msg s=%u t=%u",
                (unsigned) mhdr->size, (unsigned) mhdr->type);
        }

        reslist_pop_nodestroy(rl_read_arg);
        size_t allocsz;
        if (SATADD(&allocsz, arglen, 1))
            die(ECOMM, "bad handshake: argument length overflow");

        argv[argno] = xalloc(arglen + 1);
        memcpy(argv[argno], argval, arglen);
        argv[argno][arglen] = '\0';
    }

    argv[expected] = NULL;
    *out_argv = argv;
    *out_cwd = cwd;
}

static struct child*
start_child(struct msg_shex_hello* shex_hello)
{
    if (shex_hello->nr_argv < 2)
        die(ECOMM, "insufficient arguments given");

    SCOPED_RESLIST(rl_args);
    char** child_args;
    const char* child_chdir = NULL;
    read_child_arglist(shex_hello->nr_argv,
                       &child_args, &child_chdir);

    reslist_pop_nodestroy(rl_args);
    struct child_start_info csi = {
        .flags = CHILD_SETSID,
        .exename = child_args[0],
        .argv = (const char* const *) child_args + 1,
        .pty_setup = setup_pty,
        .pty_setup_data = shex_hello,
        .deathsig = -SIGHUP,
        .child_chdir = child_chdir,
    };

    if (shex_hello->si[0].pty_p)
        csi.flags |= CHILD_PTY_STDIN;
    if (shex_hello->si[1].pty_p)
        csi.flags |= CHILD_PTY_STDOUT;
    if (shex_hello->si[2].pty_p)
        csi.flags |= CHILD_PTY_STDERR;

    if (shex_hello->stdio_socket_p)
        csi.flags |= CHILD_SOCKETPAIR_STDIO;

    if (shex_hello->ctty_p)
        csi.flags |= CHILD_CTTY;

    return child_start(&csi);
}

static void __attribute__((noreturn))
re_exec_as_root()
{
    execlp("su", "su", "-c", orig_argv0, "stub", NULL);
    die_errno("execlp of su");
}

static void __attribute__((noreturn))
re_exec_as_user(const char* username)
{
    execlp("run-as", "run-as", username, orig_argv0, "stub", NULL);
    die_errno("execlp of run-as");
}

int
stub_main(int argc, const char** argv)
{
    if (argc != 1)
        die(EINVAL, "this command is internal");

    /* XMKRAW_SKIP_CLEANUP so we never change from raw back to cooked
     * mode on exit.  The connection dies on exit anyway, and
     * resetting the pty can send some extra bytes that can confuse
     * our peer. */

    if (isatty(0))
        xmkraw(0, XMKRAW_SKIP_CLEANUP);

    if (isatty(1))
        xmkraw(1, XMKRAW_SKIP_CLEANUP);

    printf(FB_ADB_PROTO_START_LINE "\n", build_time, (int) getuid());
    fflush(stdout);

    struct msg_shex_hello* shex_hello;
    struct msg* mhdr = read_msg(0, read_all_adb_encoded);

    if (mhdr->type == MSG_EXEC_AS_ROOT)
        re_exec_as_root(); // Never returns

    if (mhdr->type == MSG_EXEC_AS_USER) {
        struct msg_exec_as_user* umsg =
            (struct msg_exec_as_user*) mhdr;
        size_t username_length = umsg->msg.size - sizeof (*umsg);
        if (username_length > INT_MAX)
            die(ECOMM, "name length overflow");

        const char* username =
            xaprintf("%.*s",
                     (int) username_length,
                     umsg->username);
        re_exec_as_user(username); // Never returns
    }

    if (mhdr->type != MSG_SHEX_HELLO ||
        mhdr->size < sizeof (struct msg_shex_hello))
    {
        die(ECOMM, "bad hello");
    }

    shex_hello = (struct msg_shex_hello*) mhdr;

    struct child* child = start_child(shex_hello);
    struct stub stub;
    memset(&stub, 0, sizeof (stub));
    stub.child = child;
    struct fb_adb_sh* sh = &stub.sh;

    sh->process_msg = stub_process_msg;
    sh->max_outgoing_msg = shex_hello->maxmsg;
    sh->nrch = 5;
    struct channel** ch = xalloc(sh->nrch * sizeof (*ch));

    ch[FROM_PEER] = channel_new(fdh_dup(0),
                                shex_hello->stub_recv_bufsz,
                                CHANNEL_FROM_FD);

    ch[FROM_PEER]->window = UINT32_MAX;
    ch[FROM_PEER]->adb_encoding_hack = true;
    replace_with_dev_null(0);

    ch[TO_PEER] = channel_new(fdh_dup(1),
                              shex_hello->stub_send_bufsz,
                              CHANNEL_TO_FD);
    replace_with_dev_null(1);

    ch[CHILD_STDIN] = channel_new(child->fd[0],
                                  shex_hello->si[0].bufsz,
                                  CHANNEL_TO_FD);

    ch[CHILD_STDIN]->track_bytes_written = true;
    ch[CHILD_STDIN]->bytes_written =
        ringbuf_room(ch[CHILD_STDIN]->rb);

    ch[CHILD_STDOUT] = channel_new(child->fd[1],
                                   shex_hello->si[1].bufsz,
                                   CHANNEL_FROM_FD);
    ch[CHILD_STDOUT]->track_window = true;

    ch[CHILD_STDERR] = channel_new(child->fd[2],
                                   shex_hello->si[2].bufsz,
                                   CHANNEL_FROM_FD);
    ch[CHILD_STDERR]->track_window = true;

    sh->ch = ch;
    io_loop_init(sh);

    PUMP_WHILE(sh, (!channel_dead_p(ch[FROM_PEER]) &&
                    !channel_dead_p(ch[TO_PEER]) &&
                    (!channel_dead_p(ch[CHILD_STDOUT]) ||
                     !channel_dead_p(ch[CHILD_STDERR]))));

    if (channel_dead_p(ch[FROM_PEER]) || channel_dead_p(ch[TO_PEER])) {
        //
        // If we lost our peer connection, make sure the child sees
        // SIGHUP instead of seeing its stdin close: just drain any
        // internally-buffered IO and exit.  When we lose the pty, the
        // child gets SIGHUP.
        //

        // Make sure we won't be getting any more commands.

        channel_close(ch[FROM_PEER]);
        channel_close(ch[TO_PEER]);

        PUMP_WHILE(sh, (!channel_dead_p(ch[FROM_PEER]) ||
                        !channel_dead_p(ch[TO_PEER])));

        // Drain output buffers
        channel_close(ch[CHILD_STDIN]);
        PUMP_WHILE(sh, !channel_dead_p(ch[CHILD_STDIN]));
        return 128 + SIGHUP;
    }

    //
    // Clean exit: close standard handles and drain IO.  Peer still
    // has no idea that we're exiting.  Get exit status, send that to
    // peer, then cleanly shut down the peer connection.
    //

    dbg("clean exit");

    channel_close(ch[CHILD_STDIN]);
    channel_close(ch[CHILD_STDOUT]);
    channel_close(ch[CHILD_STDERR]);

    PUMP_WHILE (sh, (!channel_dead_p(ch[CHILD_STDIN]) ||
                     !channel_dead_p(ch[CHILD_STDOUT]) ||
                     !channel_dead_p(ch[CHILD_STDERR])));

    send_exit_message(child_wait(child), sh);
    channel_close(ch[TO_PEER]);

    PUMP_WHILE(sh, !channel_dead_p(ch[TO_PEER]));
    channel_close(ch[FROM_PEER]);
    PUMP_WHILE(sh, !channel_dead_p(ch[FROM_PEER]));
    return 0;
}
