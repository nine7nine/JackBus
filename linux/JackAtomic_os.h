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

#ifndef __JackAtomic_linux__
#define __JackAtomic_linux__

#include "JackTypes.h"

/*
 Compare-and-swap on a 32-bit word, also used on words in shared memory that
 are accessed from other processes (including 32-bit clients of a 64-bit
 server).

 Sequentially consistent: a full hardware barrier (lock cmpxchg on x86) *and*
 a compiler barrier. The previous hand-written x86 asm had no "memory"
 clobber and did not declare *addr as modified, so the compiler was free to
 move ordinary loads/stores across it; the lock-free state switching in
 JackAtomicState/JackAtomicArrayState (graph, transport, frame timer) relies
 on that ordering. The PowerPC asm lacked the trailing isync as well.
*/
static inline char CAS(UInt32 value, UInt32 newvalue, volatile void* addr)
{
    return __atomic_compare_exchange_n((volatile UInt32*)addr, &value, newvalue,
                                       false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

#endif
