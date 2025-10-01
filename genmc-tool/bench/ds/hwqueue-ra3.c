#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

#ifndef MAX
#define MAX 42
#endif

atomic_int AR[MAX];
atomic_int back;

int r_1, r_2, r_3, r_4;

void enqueue(int a)
{
	int k = atomic_fetch_add_explicit(&back, 1, memory_order_acq_rel);
	atomic_store_explicit(&AR[k], a, memory_order_release);
	return;
}

int dequeue(int expected)
{
	int lback = atomic_load_explicit(&back, memory_order_acquire);
	int lan, k;

	for (lan = k = 0; lan == 0; ++k) {
		__VERIFIER_assume(k < lback);
		// lan = atomic_exchange_explicit(&AR[k], 0, memory_order_acq_rel);
		lan = atomic_exchange_explicit(&AR[k], 0, memory_order_relaxed);
		// __VERIFIER_assume(lan == expected || lan == 0);
	}
	return lan;
}

void *thread_1(void *unused)
{
	enqueue(1);
	r_2 = dequeue(2);
	return NULL;
}

void *thread_2(void *unused)
{
	enqueue(2);
	enqueue(3);
	return NULL;
}

void *thread_3(void *unused)
{
	r_3 = dequeue(3);
	enqueue(4);
	return NULL;
}

void *thread_4(void *unused)
{
	r_4 = dequeue(4);
	r_1 = dequeue(1);
	return NULL;
}

int main()
{
	pthread_t t1, t2, t3, t4;

	if (pthread_create(&t1, NULL, thread_1, NULL))
		abort();
	if (pthread_create(&t2, NULL, thread_2, NULL))
		abort();
	if (pthread_create(&t3, NULL, thread_3, NULL))
		abort();
	if (pthread_create(&t4, NULL, thread_4, NULL))
		abort();

	if (pthread_join(t1, NULL))
		abort();
	if (pthread_join(t2, NULL))
		abort();
	if (pthread_join(t3, NULL))
		abort();
	if (pthread_join(t4, NULL))
		abort();

	int result = (r_1 == 1 && r_2 == 2 && r_3 == 3 && r_4 == 4);
	assert(!result);

	return 0;
}
