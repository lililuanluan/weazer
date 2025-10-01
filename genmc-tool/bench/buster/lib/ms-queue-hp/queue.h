#ifndef __MS_QUEUE_HP_H__
#define __MS_QUEUE_HP_H__

#include <stdatomic.h>

struct node;
typedef struct node node_t;

typedef struct {
	_Atomic(node_t *) head;
	_Atomic(node_t *) tail;
} queue_t;

void init_queue(queue_t *q, int num_threads);
void enqueue(queue_t *q, unsigned int val);
bool dequeue(queue_t *q, unsigned int *retVal);

#endif /* __MS_QUEUE_HP_H__ */
