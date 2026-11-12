/* jack2-nspa: JackConnectionManager::CopyFrom() must make the destination
 * byte-identical to the source, whatever stale state the destination holds
 * (JackAtomicState copies the current state into the *next* one, which still
 * holds an older graph). Random connect/disconnect/port add/remove on both,
 * then CopyFrom() and memcmp(), many rounds. */
#include "JackConnectionManager.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace Jack;

static void Mutate(JackConnectionManager* m, int ops)
{
    for (int i = 0; i < ops; i++) {
        jack_port_id_t a = 1 + rand() % 300, b = 1 + rand() % 300;
        int ref = rand() % 32;
        switch (rand() % 6) {
            case 0: if (!m->IsConnected(a, b)) m->Connect(a, b); break;
            case 1: m->Disconnect(a, b); break;
            case 2: m->AddInputPort(ref, a); break;
            case 3: m->AddOutputPort(ref, a); break;
            case 4: m->RemoveInputPort(ref, a); break;
            case 5: m->RemoveOutputPort(ref, a); break;
        }
    }
}

int main()
{
    JackConnectionManager* src = new JackConnectionManager();
    JackConnectionManager* dst = new JackConnectionManager();
    int failures = 0;
    srand(4242);
    for (int round = 0; round < 300; round++) {
        Mutate(src, 200);
        Mutate(dst, rand() % 150);  // stale, diverged next state
        dst->CopyFrom(*src);
        if (memcmp(src, dst, sizeof(JackConnectionManager)) != 0) {
            if (failures++ < 5)
                printf("FAIL: round %d: destination differs after CopyFrom\n", round);
        }
    }
    printf("%s: 300 rounds, CopyFrom result %s (%zu-byte state)\n", failures ? "FAIL" : "PASS",
           failures ? "differs" : "byte-identical", sizeof(JackConnectionManager));
    delete src;
    delete dst;
    return failures ? 1 : 0;
}
