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

#ifndef __JackAtomic__
#define __JackAtomic__

#include "JackTypes.h"
#include "JackAtomic_os.h"

// Both return the value *before* the operation.
static inline long INC_ATOMIC(volatile SInt32* val)
{
#if defined(__GNUC__)
    return __atomic_fetch_add(val, 1, __ATOMIC_SEQ_CST);
#else
    SInt32 actual;
    do {
        actual = *val;
    } while (!CAS(actual, actual + 1, val));
    return actual;
#endif
}

static inline long DEC_ATOMIC(volatile SInt32* val)
{
#if defined(__GNUC__)
    return __atomic_fetch_sub(val, 1, __ATOMIC_SEQ_CST);
#else
    SInt32 actual;
    do {
        actual = *val;
    } while (!CAS(actual, actual - 1, val));
    return actual;
#endif
}

#endif


