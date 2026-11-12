 /*
Copyright (C) 2004-2008 Grame

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

#include "JackPosixProcessSync.h"
#include "JackError.h"
#include <time.h>

namespace Jack
{

// Absolute CLOCK_MONOTONIC deadline usec from now (the condition variable uses that clock).
static void MonotonicDeadline(long usec, struct timespec* deadline)
{
    clock_gettime(CLOCK_MONOTONIC, deadline);
    long long nsec = (long long)deadline->tv_nsec + (long long)usec * 1000;
    deadline->tv_sec += (time_t)(nsec / 1000000000LL);
    deadline->tv_nsec = (long)(nsec % 1000000000LL);
}

void JackPosixProcessSync::Signal()
{
    int res = pthread_cond_signal(&fCond);
    if (res != 0) {
        jack_error("JackPosixProcessSync::Signal error err = %s", strerror(res));
    }
}

// TO DO : check thread consistency?
void JackPosixProcessSync::LockedSignal()
{
    int res = pthread_mutex_lock(&fMutex);
    if (res != 0) {
        jack_error("JackPosixProcessSync::LockedSignal error err = %s", strerror(res));
    }
    res = pthread_cond_signal(&fCond);
    if (res != 0) {
        jack_error("JackPosixProcessSync::LockedSignal error err = %s", strerror(res));
    }
    res = pthread_mutex_unlock(&fMutex);
    if (res != 0) {
        jack_error("JackPosixProcessSync::LockedSignal error err = %s", strerror(res));
    }
}

void JackPosixProcessSync::SignalAll()
{
    int res = pthread_cond_broadcast(&fCond);
    if (res != 0) {
        jack_error("JackPosixProcessSync::SignalAll error err = %s", strerror(res));
    }
}

// TO DO : check thread consistency?
void JackPosixProcessSync::LockedSignalAll()
{
    int res = pthread_mutex_lock(&fMutex);
    if (res != 0) {
        jack_error("JackPosixProcessSync::LockedSignalAll error err = %s", strerror(res));
    }
    res = pthread_cond_broadcast(&fCond);
    if (res != 0) {
        jack_error("JackPosixProcessSync::LockedSignalAll error err = %s", strerror(res));
    }
    res = pthread_mutex_unlock(&fMutex);
    if (res != 0) {
        jack_error("JackPosixProcessSync::LockedSignalAll error err = %s", strerror(res));
    }
}

void JackPosixProcessSync::Wait()
{
    ThrowIf(!pthread_equal(pthread_self(), fOwner), JackException("JackPosixProcessSync::Wait: a thread has to have locked a mutex before it can wait"));
    fOwner = 0;

    int res = pthread_cond_wait(&fCond, &fMutex);
    if (res != 0) {
        jack_error("JackPosixProcessSync::Wait error err = %s", strerror(res));
    } else {
        fOwner = pthread_self();
    }
}

// TO DO : check thread consistency?
void JackPosixProcessSync::LockedWait()
{
    int res;
    res = pthread_mutex_lock(&fMutex);
    if (res != 0) {
        jack_error("JackPosixProcessSync::LockedWait error err = %s", strerror(res));
    }
    if ((res = pthread_cond_wait(&fCond, &fMutex)) != 0) {
        jack_error("JackPosixProcessSync::LockedWait error err = %s", strerror(res));
    }
    res = pthread_mutex_unlock(&fMutex);
    if (res != 0) {
        jack_error("JackPosixProcessSync::LockedWait error err = %s", strerror(res));
    }
}

bool JackPosixProcessSync::TimedWait(long usec)
{
    ThrowIf(!pthread_equal(pthread_self(), fOwner), JackException("JackPosixProcessSync::TimedWait: a thread has to have locked a mutex before it can wait"));
    fOwner = 0;

    timespec time;
    int res;

    MonotonicDeadline(usec, &time);

    res = pthread_cond_timedwait(&fCond, &fMutex, &time);
    if (res != 0 && res != ETIMEDOUT) {
        jack_error("JackPosixProcessSync::TimedWait error usec = %ld err = %s", usec, strerror(res));
    } else {
        fOwner = pthread_self();
    }

    return (res == 0);
}

// TO DO : check thread consistency?
bool JackPosixProcessSync::LockedTimedWait(long usec)
{
    struct timeval T0, T1;
    timespec time;
    int res1, res2;

    res1 = pthread_mutex_lock(&fMutex);
    if (res1 != 0) {
        jack_error("JackPosixProcessSync::LockedTimedWait error err = %s", strerror(res1));
    }

    jack_log("JackPosixProcessSync::TimedWait time out = %ld", usec);
    gettimeofday(&T0, 0);

    MonotonicDeadline(usec, &time);
    res2 = pthread_cond_timedwait(&fCond, &fMutex, &time);
    if (res2 != 0) {
        jack_error("JackPosixProcessSync::LockedTimedWait error usec = %ld err = %s", usec, strerror(res2));
    }

    gettimeofday(&T1, 0);
    res1 = pthread_mutex_unlock(&fMutex);
    if (res1 != 0) {
        jack_error("JackPosixProcessSync::LockedTimedWait error err = %s", strerror(res1));
    }

    jack_log("JackPosixProcessSync::TimedWait finished delta = %5.1lf",
             (1e6 * T1.tv_sec - 1e6 * T0.tv_sec + T1.tv_usec - T0.tv_usec));

    return (res2 == 0);
}


} // end of namespace

