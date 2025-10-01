
#include "model-assert.h"
#include "cds_atomic.h"
#include "librace.h"
#include "cds_threads.h"

#ifndef N
#define N 5
#endif

atomic_int x;
atomic_int y;

int NN = N;

void *thread_0(void *unused)
{
	for (int i = 1; i <= NN; i++)
	{
		x = x * 2;	// N times
	}
	return NULL;
}

void *thread_1(void *unused)
{
	int xx = 0;

	int trigger = (x == 0); // || x == 42	// switch 1 time

	for (int i = 1; (i <= NN); i++)
	{
		x = x + 3; 
		// switch N tims
		xx = xx + 3;
		xx = xx * 2;

		trigger &= (y == i); 
	}
	trigger &= (xx == x);	
	MODEL_ASSERT(!trigger);
	return NULL;
}

void *thread_n(void *unused)
{
	// switch N times
	atomic_fetch_add(&y, 1);
	return NULL;
}

void *thread_2(void *unused)
{
	pthread_t t[N];

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
