#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "../lib/stack-wrapper.h"

#ifndef MAX_THREADS
#define MAX_THREADS 32
#endif

#ifdef READERS
#define DEFAULT_READERS (READERS)
#else
#define DEFAULT_READERS 1
#endif

#ifdef WRITERS
#define DEFAULT_WRITERS (WRITERS)
#else
#define DEFAULT_WRITERS 1
#endif

#ifndef HP_THREAD_LIMIT
#define HP_THREAD_LIMIT 16
#endif

#ifndef NUM_PUSH
#define NUM_PUSH 4
#endif

int readers = DEFAULT_READERS, writers = DEFAULT_WRITERS;

stack_t *stack;
stack_t mystack;
int num_threads;

unsigned int input[MAX_THREADS + 1];
unsigned int output[MAX_THREADS + 1];

int __thread tid;

/* Keep track of how many readers failed */
bool failed[DEFAULT_READERS];

/* __thread __VERIFIER_hp_t __hp; */

void set_thread_num(int i) { tid = i; }

int get_thread_num() { return tid; }

__VERIFIER_hp_t hps[MAX_THREADS + 1][HP_THREAD_LIMIT];
int __thread __hp_index;

__VERIFIER_hp_t *get_free_hp()
{
	int index = __hp_index++;
	assert(index < HP_THREAD_LIMIT);
	return &hps[tid][index];
}

/* __VERIFIER_hp_t *get_free_hp() */
/* { */
/* 	return &__hp; */
/* } */

void *threadW(void *param)
{
	int pid = (intptr_t)param;

	set_thread_num(pid);

	for (int i = 0u; i < NUM_PUSH; i++)
		push(stack, i);
	return NULL;
}

void *threadR(void *param)
{
	int pid = (intptr_t)param;

	set_thread_num(pid);

	for (int i = 0u; i < NUM_PUSH; i++)
		push(stack, 100 + i);

	int out[NUM_PUSH];
	for (int i = 0u; i < NUM_PUSH; i++)
		__VERIFIER_assume(pop(stack, &out[i]));
			// if(!pop(stack, &out[i])) return NULL;

	int result = 1;
	for (int i = 0u; i < NUM_PUSH; i++)

#ifndef SAFE
		result &= (i % 2 == 0) ? (out[i] >= 100) : (out[i] < NUM_PUSH);
#else
		result &= (out[i] < 0);
#endif
	assert(!result);
	return NULL;
}

int main()
{
	pthread_t threads[MAX_THREADS + 1];
	unsigned int in_sum = 0, out_sum = 0;
	int i = 0;

	stack = &mystack;
	num_threads = readers + writers;

	init_stack(stack, num_threads);

	++i;
	for (int j = 0; j < writers; j++, i++)
		pthread_create(&threads[i], NULL, threadW, (void *)(intptr_t)i);
	for (int j = 0; j < readers; j++, i++)
		pthread_create(&threads[i], NULL, threadR, (void *)(intptr_t)i);

	i = 1;
	for (int j = 0; j < writers; j++, i++)
		pthread_join(threads[i], NULL);
	for (int j = 0; j < readers; j++, i++)
		pthread_join(threads[i], NULL);

#ifdef PRINT_INFO
	printf("---\n");
	for (i = 1; i <= num_threads; i++)
		printf("input[%d] = %u, output[%d] = %u\n", i, input[i], i, output[i]);
#endif

	/* /\* Pop whatever is left in the stack *\/ */
	/* for (i = 0; i < readers; i++) { */
	/* 	unsigned tmp; */
	/* 	if (failed[i]) { */
	/* 		assert(pop(stack, &tmp)); */
	/* 		out_sum += tmp; */
	/* 	} */
	/* } */
	/* for (i = readers; i < writers; i++) { */
	/* 	unsigned tmp; */
	/* 	assert(pop(stack, &tmp)); */
	/* 	out_sum += tmp; */
	/* } */
	/* assert(!pop(stack, NULL)); */

	/* /\* Ensure that in_sum == out_sum *\/ */
	/* for (i = 1; i <= num_threads; i++) { */
	/* 	in_sum += input[i]; */
	/* 	out_sum += output[i]; */
	/* } */
	/* assert(in_sum == out_sum); */

	return 0;
}
