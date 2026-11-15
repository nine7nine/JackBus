/*
Copyright (C) 2026 JackBus

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

#ifndef __JackBoundedQueue__
#define __JackBoundedQueue__

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace Jack
{

/*!
\brief Bounded lock-free multi-producer queue (D. Vyukov's MPMC design).

 For handing data from realtime threads to a non-realtime consumer without
 locks or system calls on the producer side: TryPush() never blocks, takes
 no lock and makes no system call; it fails when the queue is full. The
 consumer side (TryPop) is safe for a single consumer thread (or several).

 Process-local only (never place in shared memory). SIZE must be a power of 2.
*/
template <typename T, unsigned int SIZE>
class JackBoundedQueue
{
    static_assert(SIZE >= 2 && (SIZE & (SIZE - 1)) == 0, "SIZE must be a power of 2");

    private:

        struct Cell {
            std::atomic<size_t> fSequence;
            T fData;
        };

        alignas(64) Cell fCells[SIZE];
        alignas(64) std::atomic<size_t> fEnqueuePos;
        alignas(64) std::atomic<size_t> fDequeuePos;

    public:

        JackBoundedQueue(): fEnqueuePos(0), fDequeuePos(0)
        {
            for (size_t i = 0; i < SIZE; i++) {
                fCells[i].fSequence.store(i, std::memory_order_relaxed);
            }
        }

        /*! Producer: fill(T&) writes the element in place. Returns false if full. */
        template <typename F>
        bool TryPush(F fill)
        {
            size_t pos = fEnqueuePos.load(std::memory_order_relaxed);
            Cell* cell;
            for (;;) {
                cell = &fCells[pos & (SIZE - 1)];
                size_t seq = cell->fSequence.load(std::memory_order_acquire);
                intptr_t diff = (intptr_t)seq - (intptr_t)pos;
                if (diff == 0) {
                    if (fEnqueuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                        break;
                    }
                } else if (diff < 0) {
                    return false;  // full
                } else {
                    pos = fEnqueuePos.load(std::memory_order_relaxed);
                }
            }
            fill(cell->fData);
            cell->fSequence.store(pos + 1, std::memory_order_release);
            return true;
        }

        /*! Consumer: consume(T&) reads the element in place. Returns false if empty. */
        template <typename F>
        bool TryPop(F consume)
        {
            size_t pos = fDequeuePos.load(std::memory_order_relaxed);
            Cell* cell;
            for (;;) {
                cell = &fCells[pos & (SIZE - 1)];
                size_t seq = cell->fSequence.load(std::memory_order_acquire);
                intptr_t diff = (intptr_t)seq - (intptr_t)(pos + 1);
                if (diff == 0) {
                    if (fDequeuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                        break;
                    }
                } else if (diff < 0) {
                    return false;  // empty (or the next element is still being written)
                } else {
                    pos = fDequeuePos.load(std::memory_order_relaxed);
                }
            }
            consume(cell->fData);
            cell->fSequence.store(pos + SIZE, std::memory_order_release);
            return true;
        }
};

} // end of namespace

#endif
