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

#ifndef __JackAtomicState__
#define __JackAtomicState__

#include "JackAtomic.h"
#include "JackCompilerDeps.h"
#include <string.h> // for memcpy
#include <cstddef>

namespace Jack
{

/*!
\brief Copies a state when a write starts on the next one. Overload it (found
by argument-dependent lookup) for large states that can be copied more
cheaply, e.g. JackConnectionManager.
*/
template <class T>
inline void JackStateCopy(T& dst, const T& src)
{
    memcpy(&dst, &src, sizeof(T));
}

/*!
\brief Counter for CAS
*/

PRE_PACKED_STRUCTURE
struct AtomicCounter
{
    union {
        struct {
            UInt16 fShortVal1;	// Cur
            UInt16 fShortVal2;	// Next
        }
        scounter;
        UInt32 fLongVal;
    }info;

	AtomicCounter()
	{
        info.fLongVal = 0;
    }

	AtomicCounter(volatile const AtomicCounter& obj)
	{
		info.fLongVal = obj.info.fLongVal;
	}

	AtomicCounter(volatile AtomicCounter& obj)
	{
		info.fLongVal = obj.info.fLongVal;
	}

 	AtomicCounter& operator=(AtomicCounter& obj)
    {
        info.fLongVal = obj.info.fLongVal;
        return *this;
    }

	AtomicCounter& operator=(volatile AtomicCounter& obj)
	{
        info.fLongVal = obj.info.fLongVal;
        return *this;
    }

} POST_PACKED_STRUCTURE;

#define Counter(e) (e).info.fLongVal
#define CurIndex(e) (e).info.scounter.fShortVal1
#define NextIndex(e) (e).info.scounter.fShortVal2

#define CurArrayIndex(e) (CurIndex(e) & 0x0001)
#define NextArrayIndex(e) ((CurIndex(e) + 1) & 0x0001)

/*!
\brief A class to handle two states (switching from one to the other) in a lock-free manner
*/

// CHECK livelock

PRE_PACKED_STRUCTURE
template <class T>
class JackAtomicState
{

    protected:

        T fState[2];
        alignas(UInt32) alignas(AtomicCounter) volatile AtomicCounter fCounter;
        SInt32 fCallWriteCounter;

        // Atomic snapshot of fCounter (written with CAS by other threads and
        // processes; a plain read of the volatile is a data race).
        UInt32 LoadCounter() const
        {
            return __atomic_load_n((const volatile UInt32*)&fCounter, __ATOMIC_ACQUIRE);
        }

        UInt16 LoadCurIndex() const
        {
            AtomicCounter counter;
            Counter(counter) = LoadCounter();
            return CurIndex(counter);
        }

        UInt32 WriteNextStateStartAux()
        {
            AtomicCounter old_val;
            AtomicCounter new_val;
            UInt32 cur_index;
            UInt32 next_index;
            bool need_copy;
            do {
                Counter(old_val) = LoadCounter();
                new_val = old_val;
                cur_index = CurArrayIndex(new_val);
                next_index = NextArrayIndex(new_val);
                need_copy = (CurIndex(new_val) == NextIndex(new_val));
                NextIndex(new_val) = CurIndex(new_val); // Invalidate next index
            } while (!CAS(Counter(old_val), Counter(new_val), (UInt32*)&fCounter));
            if (need_copy)
                JackStateCopy(fState[next_index], fState[cur_index]);
            return next_index;
        }

        void WriteNextStateStopAux()
        {
            AtomicCounter old_val;
            AtomicCounter new_val;
            do {
                Counter(old_val) = LoadCounter();
                new_val = old_val;
                NextIndex(new_val)++; // Set next index
            } while (!CAS(Counter(old_val), Counter(new_val), (UInt32*)&fCounter));
        }

    public:

        JackAtomicState()
        {
            static_assert(offsetof(JackAtomicState, fCounter) % sizeof(fCounter) == 0,
                          "fCounter must be aligned within JackAtomicState");
            Counter(fCounter) = 0;
            fCallWriteCounter = 0;
        }

        ~JackAtomicState() // Not virtual ??
        {}

        /*!
        \brief Returns the current state : only valid in the RT reader thread
        */
        T* ReadCurrentState()
        {
            return &fState[(LoadCurIndex() & 0x0001)];
        }

        /*!
        \brief Returns the current state index
        */
        UInt16 GetCurrentIndex()
        {
            // JackBus: a reader's state reads before this index read (the retry check of a
            // coherent read): an acquire fence keeps them before it on weakly ordered CPUs
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            return LoadCurIndex();
        }

        /*!
        \brief Tries to switch to the next state and returns the new current state (either the same as before if case of switch failure or the new one)
        */
        T* TrySwitchState()
        {
            AtomicCounter old_val;
            AtomicCounter new_val;
            do {
                Counter(old_val) = LoadCounter();
                new_val = old_val;
                CurIndex(new_val) = NextIndex(new_val);	// Prepare switch
            } while (!CAS(Counter(old_val), Counter(new_val), (UInt32*)&fCounter));
            return &fState[(LoadCurIndex() & 0x0001)];	// Read the counter again
        }

        /*!
        \brief Tries to switch to the next state and returns the new current state (either the same as before if case of switch failure or the new one)
        */
        T* TrySwitchState(bool* result)
        {
            AtomicCounter old_val;
            AtomicCounter new_val;
            do {
                Counter(old_val) = LoadCounter();
                new_val = old_val;
                *result = (CurIndex(new_val) != NextIndex(new_val));
                CurIndex(new_val) = NextIndex(new_val);  // Prepare switch
            } while (!CAS(Counter(old_val), Counter(new_val), (UInt32*)&fCounter));
            return &fState[(LoadCurIndex() & 0x0001)];	// Read the counter again
        }

        /*!
        \brief Start write operation : setup and returns the next state to update, check for recursive write calls.
        */
        T* WriteNextStateStart()
        {
            UInt32 next_index = (fCallWriteCounter++ == 0)
                                ? WriteNextStateStartAux()
                                : ((LoadCurIndex() + 1) & 0x0001); // We are inside a wrapping WriteNextStateStart call, NextArrayIndex can be read safely
            return &fState[next_index];
        }

        /*!
        \brief Stop write operation : make the next state ready to be used by the RT thread
        */
        void WriteNextStateStop()
        {
            if (--fCallWriteCounter == 0)
                WriteNextStateStopAux();
        }

        bool IsPendingChange()
        {
            AtomicCounter counter;
            Counter(counter) = LoadCounter();
            return CurIndex(counter) != NextIndex(counter);
        }

        /*
              // Single writer : write methods get the *next* state to be updated
        void TestWriteMethod()
        {
        	T* state = WriteNextStateStart();
        	......
        	......
        	WriteNextStateStop(); 
        }

              // First RT call possibly switch state
        void TestReadRTMethod1()
        {
        	T* state = TrySwitchState();
        	......
        	......
        }

              // Other RT methods can safely use the current state during the *same* RT cycle
        void TestReadRTMethod2() 
        {
        	T* state = ReadCurrentState();
        	......
        	......
        }

              // Non RT read methods : must check state coherency
        void TestReadMethod()
        {
        	T* state;
        	UInt16 cur_index;
            UInt16 next_index = GetCurrentIndex();
        	do {
                cur_index = next_index;
        		state = ReadCurrentState();
        		
        		......
        		......

                next_index = GetCurrentIndex();
        	} while (cur_index != next_index);
        }
        */

} POST_PACKED_STRUCTURE;

} // end of namespace

#endif

