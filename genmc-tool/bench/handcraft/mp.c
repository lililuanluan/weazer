#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
#include <genmc.h>


#ifndef N
# define N 1000
#endif

atomic_int x;

void* thread_0(void* unused) {

    x = 42;
    for (int i = 0; i < N ; i++) {
        x = 0;
    }
    return NULL;
}

void* thread_1(void* unused) {


    for (int i = 0; i < N / 2; i++) {
        x = 1;
    }
    return NULL;
}

void* thread_42(void* unused) {
    // int r = atomic_load_explicit(&x, memory_order_seq_cst);
    int r = x;
    assert(r != 42);
    return NULL;
}

int main()
{
    pthread_t t0, t1, t42;
    
    if (pthread_create(&t0, NULL, thread_0, NULL))
        abort();
    if (pthread_create(&t42, NULL, thread_42, NULL))
        abort();
    // if (pthread_create(&t1, NULL, thread_1, NULL))
    //     abort();

    

    if (pthread_join(t0, NULL))
        abort();
    // if (pthread_join(t1, NULL))
    //     abort();
    if (pthread_join(t42, NULL))
        abort();
}
