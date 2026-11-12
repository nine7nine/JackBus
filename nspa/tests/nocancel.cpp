/* jack2-nspa: JackNoCancelScope with asynchronous cancellation.
 *
 * libjack's RT and notification threads use PTHREAD_CANCEL_ASYNCHRONOUS and
 * are cancelled by jack_client_close(). Inside a JackNoCancelScope a cancel
 * request stays pending; glibc acts on it inside pthread_setcancelstate()
 * when the scope ends. The forced unwind then starts in the scope's
 * destructor, which must not be noexcept, or std::terminate() aborts the
 * application.
 *
 * Each round: a thread with asynchronous cancellation enters the scope, the
 * main thread cancels it while it is inside, the thread leaves the scope.
 * Expected: the thread ends cancelled (no abort), after leaving the scope,
 * and its cleanup handler ran.
 */
#include "JackThread.h"
#include <pthread.h>
#include <stdio.h>
#include <atomic>

static std::atomic<int> inside, release, left_scope, cleanup;

static void on_cleanup(void*) { cleanup.store(1); }

static void* thread_main(void*)
{
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    pthread_cleanup_push(on_cleanup, NULL);
    {
        Jack::JackNoCancelScope no_cancel;
        inside.store(1);
        while (!release.load()) {
        }
        left_scope.store(1);  // still inside: the destructor runs after this
    }
    // not reached: the pending cancel is acted on when the scope ends
    for (;;) {
    }
    pthread_cleanup_pop(0);
    return NULL;
}

int main()
{
    const int rounds = 100;
    int ok = 0;
    for (int r = 0; r < rounds; r++) {
        inside = release = left_scope = cleanup = 0;
        pthread_t t;
        if (pthread_create(&t, NULL, thread_main, NULL)) {
            printf("FAIL: pthread_create\n");
            return 1;
        }
        while (!inside.load()) {
        }
        pthread_cancel(t);  // pending: cancellation is disabled in the scope
        release.store(1);
        void* res = NULL;
        pthread_join(t, &res);
        if (res == PTHREAD_CANCELED && left_scope.load() && cleanup.load())
            ok++;
    }
    printf("%s: %d of %d threads cancelled at the end of JackNoCancelScope (no abort, cleanup ran)\n",
           ok == rounds ? "PASS" : "FAIL", ok, rounds);
    return ok == rounds ? 0 : 1;
}
