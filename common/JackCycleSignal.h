/*
Copyright (C) 2026 jack2-nspa

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

*/

#ifndef __JackCycleSignal__
#define __JackCycleSignal__

#include "JackPlatformPlug.h"

#ifdef __linux__
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <linux/time_types.h>
#if !defined(SYS_futex) && defined(SYS_futex_time64)
#define SYS_futex SYS_futex_time64
#endif
/* the timeout is a libc struct timespec: 32-bit builds with a 64-bit time_t
   (_TIME_BITS=64) must use the time64 system call */
#if defined(SYS_futex_time64) && !defined(__LP64__)
#define JACK_SYS_FUTEX_TIMED (sizeof(time_t) == 8 ? SYS_futex_time64 : SYS_futex)
#else
#define JACK_SYS_FUTEX_TIMED SYS_futex
#endif
#endif

namespace Jack
{

/*!
\brief "A cycle has passed" event, signalled by the realtime driver thread.

 Non-realtime threads (client activate/deactivate/close) wait for the next
 cycle so that a graph change is known to be in effect. The realtime side
 used to signal a pthread condition variable every cycle; glibc condvars
 serialize signallers and timed-out waiters on an internal lock that is not
 priority-inheriting. Here the realtime side is one atomic increment, plus a
 FUTEX_WAKE only while some thread is actually waiting.
*/
#ifdef __linux__

class JackCycleSignal
{
    private:

        alignas(4) uint32_t fSequence;  // futex word
        uint32_t fWaiters;

    public:

        JackCycleSignal(): fSequence(0), fWaiters(0)
        {}

        // Realtime thread, once per cycle.
        void Signal()
        {
            __atomic_fetch_add(&fSequence, 1, __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&fWaiters, __ATOMIC_SEQ_CST) != 0) {
                ::syscall(SYS_futex, &fSequence, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
            }
        }

        // Waits for the next Signal() (at most usec). Returns false on timeout.
        bool TimedWait(long usec)
        {
            uint32_t seq = __atomic_load_n(&fSequence, __ATOMIC_SEQ_CST);
            __atomic_fetch_add(&fWaiters, 1, __ATOMIC_SEQ_CST);

            struct timespec deadline;
            clock_gettime(CLOCK_MONOTONIC, &deadline);
            long long nsec = (long long)deadline.tv_nsec + (long long)usec * 1000;
            deadline.tv_sec += (time_t)(nsec / 1000000000LL);
            deadline.tv_nsec = (long)(nsec % 1000000000LL);

            bool signalled = false;
            for (;;) {
                if (__atomic_load_n(&fSequence, __ATOMIC_SEQ_CST) != seq) {
                    signalled = true;
                    break;
                }
                // absolute CLOCK_MONOTONIC deadline: no recomputation on EINTR
                long res = ::syscall(JACK_SYS_FUTEX_TIMED, &fSequence, FUTEX_WAIT_BITSET_PRIVATE, seq,
                                     &deadline, NULL, FUTEX_BITSET_MATCH_ANY);
                if (res == -1 && errno == ETIMEDOUT) {
                    signalled = (__atomic_load_n(&fSequence, __ATOMIC_SEQ_CST) != seq);
                    break;
                }
            }

            __atomic_fetch_sub(&fWaiters, 1, __ATOMIC_SEQ_CST);
            return signalled;
        }

    #if defined(SYS_futex_waitv) && defined(FUTEX_WAITV_MAX)
        /*
         TimedWait that also returns as soon as one of the caller's futexes no longer
         holds its expected value, or is woken (futex_waitv, Linux 5.16). waiters[0]
         is this signal's own word (filled in here), waiters[1..count] the caller's.
         Returns 1 if signalled, woken or a value changed, 0 on timeout, -1 if
         futex_waitv is not available (the caller then uses TimedWait).
         */
        int TimedWaitV(long usec, struct futex_waitv* waiters, unsigned int count)
        {
            if (count + 1 > FUTEX_WAITV_MAX) {
                count = FUTEX_WAITV_MAX - 1;
            }
            uint32_t seq = __atomic_load_n(&fSequence, __ATOMIC_SEQ_CST);
            __atomic_fetch_add(&fWaiters, 1, __ATOMIC_SEQ_CST);

            waiters[0].val = seq;
            waiters[0].uaddr = (uintptr_t)&fSequence;
            waiters[0].flags = FUTEX_32 | FUTEX_PRIVATE_FLAG;   // Signal() wakes it private
            waiters[0].__reserved = 0;

            // absolute CLOCK_MONOTONIC deadline, 64-bit on every ABI
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long long nsec = (long long)now.tv_nsec + (long long)usec * 1000;
            struct __kernel_timespec deadline;
            deadline.tv_sec = (long long)now.tv_sec + nsec / 1000000000LL;
            deadline.tv_nsec = nsec % 1000000000LL;

            int result;
            for (;;) {
                if (__atomic_load_n(&fSequence, __ATOMIC_SEQ_CST) != seq) {
                    result = 1;
                    break;
                }
                long res = ::syscall(SYS_futex_waitv, waiters, count + 1, 0, &deadline, CLOCK_MONOTONIC);
                if (res >= 0 || errno == EAGAIN) {
                    result = 1;     // woken, or a value already differed
                    break;
                }
                if (errno == ETIMEDOUT) {
                    result = (__atomic_load_n(&fSequence, __ATOMIC_SEQ_CST) != seq) ? 1 : 0;
                    break;
                }
                if (errno != EINTR) {
                    result = -1;    // ENOSYS (older kernel), or anything unexpected
                    break;
                }
            }

            __atomic_fetch_sub(&fWaiters, 1, __ATOMIC_SEQ_CST);
            return result;
        }
    #endif
};

#else

class JackCycleSignal
{
    private:

        JackProcessSync fSync;

    public:

        void Signal() { fSync.Signal(); }
        bool TimedWait(long usec) { return fSync.LockedTimedWait(usec); }
};

#endif

} // end of namespace

#endif
