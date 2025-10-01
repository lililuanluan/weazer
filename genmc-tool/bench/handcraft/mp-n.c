#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
#include <genmc.h>

#ifndef N
# define N 100
#endif

atomic_int x;

void* thrd(void* arg) {
    int tid = *(int*)arg;
    if (atomic_load_explicit(&x, memory_order_relaxed) == tid) {
        atomic_store_explicit(&x, tid - 1, memory_order_relaxed);
    }
    return NULL;
}

int main() {
    pthread_t threads[N];
    int ids[N];

    // Initialize x to 0
    atomic_init(&x, N);

    // Create N threads
    for (int i = 0; i < N; i++) {
        ids[i] = i + 1;
        if (pthread_create(&threads[i], NULL, thrd, &ids[i]) != 0) {
        }
    }

    // Wait for all threads to finish
    for (int i = 0; i < N; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
        }
    }

    // Check the final value of x
    bool result = atomic_load_explicit(&x, memory_order_relaxed) == 1;
    assert(!result);

    return 0;
}
