/* jack2-nspa: JACK mutexes must be priority-inheriting (glibc kind bit
 * PTHREAD_MUTEX_PRIO_INHERIT_NP = 0x20) and keep their type. */
#include "JackPosixMutex.h"
#include <cstdio>

struct ProbeRecursive : Jack::JackPosixMutex { int Kind() { return fMutex.__data.__kind; } };
struct ProbeBase : Jack::JackBasePosixMutex { int Kind() { return fMutex.__data.__kind; } };

int main()
{
    ProbeRecursive r;
    ProbeBase b;
    bool ok = (r.Kind() & 0x20) && (r.Kind() & 3) == PTHREAD_MUTEX_RECURSIVE && (b.Kind() & 0x20);
    r.Lock(); r.Lock(); r.Unlock(); r.Unlock();
    b.Lock(); b.Unlock();
    printf("%s: JackPosixMutex kind=0x%x, JackBasePosixMutex kind=0x%x (0x20 = priority inheritance)\n",
           ok ? "PASS" : "FAIL", r.Kind(), b.Kind());
    return ok ? 0 : 1;
}
