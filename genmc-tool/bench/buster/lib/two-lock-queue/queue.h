#ifndef __TWO_LOCK_QUEUE_H__
#define __TWO_LOCK_QUEUE_H__

#include <stdatomic.h>
#include <pthread.h>

struct node;
typedef struct node node_t;

typedef struct {
	_Atomic(node_t *) head;
	_Atomic(node_t *) tail;
	pthread_mutex_t hlock;
	pthread_mutex_t tlock;
} queue_t;

void init_queue(queue_t *q, int num_threads);
void enqueue(queue_t *q, unsigned int val);
bool dequeue(queue_t *q, unsigned int *ret_val);

#endif /* __TWO_LOCK_QUEUE_H__ */
