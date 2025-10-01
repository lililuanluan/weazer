#include "model-assert.h"
#include "cds_atomic.h"
#include "librace.h"
#include "cds_threads.h"

#ifndef N
#define N 100
#endif

atomic_int x;

void *thrd(void *arg)
{
    int tid = *(int *)arg;
    if (atomic_load_explicit(&x, memory_order_relaxed) == tid)
    {
        atomic_store_explicit(&x, tid - 1, memory_order_relaxed);
    }
    return NULL;
}

int main()
{
    pthread_t threads[N];
    int ids[N];

    // Initialize x to 0
    atomic_init(&x, N);

    // Create N threads
    for (int i = 0; i < N; i++)
    {
        ids[i] = i + 1;
        pthread_create(&threads[i], NULL, thrd, &ids[i]);
    }

    // Wait for all threads to finish
    for (int i = 0; i < N; i++)
    {
        pthread_join(threads[i], NULL);
    }

    // Check the final value of x
    bool result = atomic_load_explicit(&x, memory_order_relaxed) == 1;
    MODEL_ASSERT(!result);

    return 0;
}
