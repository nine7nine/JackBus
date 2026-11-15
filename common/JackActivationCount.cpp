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

#include "JackAtomic.h"
#include "JackActivationCount.h"
#include "JackConstants.h"
#include "JackClientControl.h"
#include "JackError.h"

/*
 ThreadSanitizer builds: a client that decrements a successor's counter without
 being the one that signals it still orders its cycle before the successor's
 (another client, or the server, signals later): a release here, paired with
 the acquire after a Wait() returns (JackLinuxFutex.cpp). No-op otherwise.
*/
#if defined(__SANITIZE_THREAD__) && defined(__linux__)
namespace Jack { void JackTsanCycleRelease(); }
#define JACK_TSAN_CYCLE_RELEASE() Jack::JackTsanCycleRelease()
#else
#define JACK_TSAN_CYCLE_RELEASE()
#endif

namespace Jack
{

bool JackActivationCount::Signal(JackSynchro* synchro, JackClientControl* control)
{
    if (__atomic_load_n(&fValue, __ATOMIC_RELAXED) == 0) {
        // Transfer activation to next clients
        jack_log("JackActivationCount::Signal value = 0 ref = %ld", control->fRefNum);
        return synchro->Signal();
    }
    JACK_TSAN_CYCLE_RELEASE();
    if (DEC_ATOMIC(&fValue) == 1) {
        return synchro->Signal();
    } else {
        return true;
    }
}

} // end of namespace

