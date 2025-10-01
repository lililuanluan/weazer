#include <pthread.h>
#include <genmc.h>
#include "queue.h"
#include "../helper.h"

typedef struct node {
	unsigned int value;
	_Atomic(struct node *) next;
} node_t;

static node_t *new_node(unsigned int value)
{
	node_t *node = malloc(sizeof(node_t));
	node->value = value;
	node->next = NULL;
	return node;
}

static void reclaim(node_t *p)
{
	free(p);
}

void init_queue(queue_t *q, int num_threads)
{
	/* initialize queue */
	node_t *dummy = new_node(0);

	atomic_init(&q->head, dummy);
	atomic_init(&q->tail, dummy);
	pthread_mutex_init(&q->hlock, NULL);
	pthread_mutex_init(&q->tlock, NULL);
}

void enqueue(queue_t *q, unsigned int val)
{
	node_t *node = new_node(val);
	node_t *tail;

	pthread_mutex_lock(&q->tlock);
	tail = atomic_load_explicit(&q->tail, acquire);
	atomic_store_explicit(&tail->next, node, release);
	atomic_store_explicit(&q->tail, node, release);
	pthread_mutex_unlock(&q->tlock);
}

bool dequeue(queue_t *q, unsigned int *retVal)
{
	node_t *node, *nhead;

	pthread_mutex_lock(&q->hlock);
	node = atomic_load_explicit(&q->head, acquire);
	nhead = atomic_load_explicit(&node->next, acquire);
	if (nhead == NULL) {
		pthread_mutex_unlock(&q->hlock);
		return false;
	}
	*retVal = nhead->value;
	atomic_store_explicit(&q->head, nhead, release);
	pthread_mutex_unlock(&q->hlock);
	free(node);
	return true;
}
