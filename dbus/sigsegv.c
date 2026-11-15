/**
 * This source file is used to print out a stack-trace when your program
 * segfaults.  It is relatively reliable and spot-on accurate.
 *
 * This code is in the public domain.  Use it as you see fit, some credit
 * would be appreciated, but is not a prerequisite for usage.  Feedback
 * on it's use would encourage further development and maintenance.
 *
 * Author:  Jaco Kroon <jaco@kroon.co.za>
 *
 * Copyright (C) 2005 - 2008 Jaco Kroon
 *
 * JackBus: rewritten to be async-signal-safe. The previous handler called
 * jack_error() (vsnprintf, stat/fopen/fprintf on the log file),
 * __cxa_demangle() (malloc), walked frame pointers and finished with exit()
 * while the server's realtime threads were still running. A crash inside
 * malloc or stdio (including glibc heap-corruption aborts, as SIGABRT is
 * handled too) could deadlock in the handler, leaving a hung jackdbus that
 * still held the sound card and its D-Bus name. Now the handler only uses
 * write(2) and backtrace_symbols_fd(3), then re-raises the signal with the
 * default action so a core dump (e.g. systemd-coredump) is produced.
 */

#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_EXECINFO_H
# include <execinfo.h>
#endif

#include "jack/control.h"
#include "sigsegv.h"

/* File descriptors the handler writes to: stderr, plus the jackdbus log
 * file if sigsegv_set_log_fd() was called. */
static volatile int g_log_fd = -1;

static void write_all(int fd, const char *s, size_t len)
{
    while (len > 0) {
        ssize_t res = write(fd, s, len);
        if (res <= 0) {
            if (res < 0 && errno == EINTR)
                continue;
            return;
        }
        s += res;
        len -= (size_t)res;
    }
}

static void write_both(const char *s)
{
    size_t len = strlen(s);
    int log_fd = g_log_fd;
    write_all(STDERR_FILENO, s, len);
    if (log_fd >= 0)
        write_all(log_fd, s, len);
}

#ifdef __APPLE__
// TODO : does not compile yet on OSX
static void signal_segv(int signum, siginfo_t* info, void*ptr)
{}
#else
static void signal_segv(int signum, siginfo_t* info, void* ptr)
{
    const char *what;
    (void)info;
    (void)ptr;

    switch (signum) {
    case SIGSEGV: what = "Segmentation fault"; break;
    case SIGABRT: what = "Abort"; break;
    case SIGILL:  what = "Illegal instruction"; break;
    case SIGFPE:  what = "Floating point exception"; break;
    default:      what = "Fatal signal"; break;
    }

    write_both("jackdbus: ");
    write_both(what);
    write_both(", stack trace follows\n");
#ifdef HAVE_EXECINFO_H
    {
        void *bt[64];
        int n = backtrace(bt, 64);
        int log_fd = g_log_fd;
        backtrace_symbols_fd(bt, n, STDERR_FILENO);
        if (log_fd >= 0)
            backtrace_symbols_fd(bt, n, log_fd);
    }
#endif
    write_both("jackdbus: end of stack trace\n");

    /* SA_RESETHAND restored the default action; the signal is blocked while
     * we run and is delivered (terminating with a core) when we return. */
    raise(signum);
}
#endif

void sigsegv_set_log_fd(int fd)
{
    g_log_fd = fd;
}

int setup_sigsegv()
{
    static const int signals[] = {SIGSEGV, SIGILL, SIGABRT, SIGFPE};
    struct sigaction action;
    size_t i;

#ifdef HAVE_EXECINFO_H
    {
        /* The first backtrace() call loads libgcc_s (malloc, dlopen): do it now,
         * not in the handler. */
        void *bt[2];
        backtrace(bt, 2);
    }
#endif

    memset(&action, 0, sizeof(action));
    action.sa_sigaction = signal_segv;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&action.sa_mask);

    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        if (sigaction(signals[i], &action, NULL) < 0) {
            jack_error("sigaction failed. errno is %d (%s)", errno, strerror(errno));
            return 0;
        }
    }
    return 1;
}
