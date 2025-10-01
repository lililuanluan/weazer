#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "../lib/queue-wrapper.h"

#ifndef MAX_THREADS
#define MAX_THREADS 32
#endif

#ifdef NOISE
#define DEFAULT_NOISE (NOISE)
#else
#define DEFAULT_NOISE 0
#endif

#ifndef HP_THREAD_LIMIT
#define HP_THREAD_LIMIT 32
#endif

#ifndef NUM_ENQ
#define NUM_ENQ 4
#endif

queue_t *queue;
queue_t myqueue;
int num_threads;

int __thread tid;

__VERIFIER_hp_t hps[MAX_THREADS + 1][HP_THREAD_LIMIT];
int __thread __hp_index;

void set_thread_num(int i) { tid = i; }

int get_thread_num() { return tid; }

__VERIFIER_hp_t *get_free_hp()
{
	int index = __hp_index++;
	assert(index < HP_THREAD_LIMIT);
	return &hps[tid][index];
}

void *thread_enq(void *param)
{
	int pid = (intptr_t)param;

	set_thread_num(pid);

	for (int i = 0u; i < NUM_ENQ; i++) {
		printf("enqueued %d from thrd %d\n", i, pid);
		enqueue(queue, i);
	}

	unsigned dequeued[NUM_ENQ];
	for (int i = 0u; i < NUM_ENQ; i++)
		__VERIFIER_assume(dequeue(queue, &dequeued[i]));
		// if(!dequeue(queue, &dequeued[i]))
		// return NULL;
	// );

	int result = 1;
	for (int i = 0u; i < NUM_ENQ; i++) {
		// result &= (i == 0) ? (dequeued[i] == 100) : (dequeued[i] == (i - 1));
		printf("dequeued[%d] = %u from thrd %d\n", i, dequeued[i], pid);

#ifndef SAFE
		result &= (i % 2 == 0) ? (dequeued[i] >= 100) : (dequeued[i] < NUM_ENQ); // luan
#else
		result &= (dequeued[i] < 0);
#endif
	}

	assert(!result);

	return NULL;
}

void *thread_deq(void *param)
{
	int pid = (intptr_t)param;

	set_thread_num(pid);

	for (int i = 0u; i < NUM_ENQ; i++) {
		printf("enqueued %d from thrd %d\n", 100 + i, pid);
		enqueue(queue, 100 + i);
	}

	return NULL;
}

void *noise_enq(void *param)
{
	int pid = (intptr_t)param;

	set_thread_num(pid);
	enqueue(queue, 0);
	return NULL;
}

void *noise_deq(void *param)
{
	int pid = (intptr_t)param;
	unsigned val;

	set_thread_num(pid);
	dequeue(queue, &val);
	if (val == 0) {
		enqueue(queue, val); // luan
	}
	return NULL;
}

int main()
{
	printf("\n----------------------------------\n");
	pthread_t te, td, tnoise[2 * (DEFAULT_NOISE + 1)];

	queue = &myqueue;
	num_threads = 2 + 2 * DEFAULT_NOISE + 1;

	init_queue(queue, num_threads);

	pthread_create(&te, NULL, thread_enq, (void *)(intptr_t)1);
	pthread_create(&td, NULL, thread_deq, (void *)(intptr_t)2);
	for (int i = 1; i <= DEFAULT_NOISE; i++) {
		pthread_create(&tnoise[2 * i], NULL, noise_enq, (void *)(intptr_t)(2 + 2 * i));
		pthread_create(&tnoise[2 * i + 1], NULL, noise_deq,
			       (void *)(intptr_t)(2 + 2 * i + 1));
	}

	pthread_join(te, NULL);
	pthread_join(td, NULL);
	for (int i = 1; i <= DEFAULT_NOISE; i++) {
		pthread_join(tnoise[2 * i], NULL);
		pthread_join(tnoise[2 * i + 1], NULL);
	}
	return 0;
}
