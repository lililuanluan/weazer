#include "long-assert.h"
#include <assert.h>
#include <genmc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef N
#define N 5
#endif

atomic_int x;
atomic_int y;

int NN = N-3;

void *thread_0(void *unused)
{
	for (int i = 1; i <= NN; i++) {
		x=x*2;
	}

	// x = 42;

	return NULL;
}

void *thread_1(void *unused)
{
	int r = 0;
	int xx =0;
	// atomic_compare_exchange_strong(&x, &r, 1);
	// assert(x != 0 || y != 1 || y != 2 || y != 3 || y != 4 || y != 5);
	// ASSERT_XY_HELPER(x, y, N);  // assert(x != 0 || y != 1 || ... || y != N)
	int trigger = (x == 0); // || x == 42
	r = x;
	// if (r == 42)
	// 	return NULL;
	for (int i = 1; (i <= NN); i++) {
		// trigger &=
			// (

			//    i >=N/2 &&
			//    r==42) ||
			// (y == i);
		// if(i>N/2) {
		// 	trigger &= (x != 0);
		// }
		// if(x%3==0) {
		// 	x = x-1;
		// }
		x=x+3;
		xx = xx+3;
		xx=xx*2;

	}


	trigger &= (xx ==x);

	assert(!trigger);
	// assert(x != 0);

	return NULL;
}

void *thread_n(void *unused)
{
	atomic_fetch_add(&y, 1);
	return NULL;
}

void *thread_2(void *unused)
{
	pthread_t t[N*2];

	for (int i = 0U; i < NN; i++)
		pthread_create(&t[i], NULL, thread_n, NULL);
	for (int i = 0U; i < NN; i++)
		pthread_join(t[i], NULL);

	atomic_fetch_add(&x, 1);
	return NULL;
}

int main()
{
	pthread_t t0, t1, t2;

	if (pthread_create(&t0, NULL, thread_0, NULL))
		abort();
	if (pthread_create(&t1, NULL, thread_1, NULL))
		abort();
	if (pthread_create(&t2, NULL, thread_2, NULL))
		abort();

	if (pthread_join(t0, NULL))
		abort();
	if (pthread_join(t1, NULL))
		abort();
	if (pthread_join(t2, NULL))
		abort();

	return 0;
}
