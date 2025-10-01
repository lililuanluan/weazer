#include <stdlib.h>
#include <genmc.h>
#include "queue.h"
#include "../helper.h"

typedef struct node {
	unsigned int value;
	_Atomic(struct node *) next;
} node_t;

__VERIFIER_hp_t *get_free_hp();

static node_t *new_node()
{
	return malloc(sizeof(node_t));
}

static void reclaim(node_t *node)
{
	__VERIFIER_hp_retire(node);
}

void init_queue(queue_t *q, int num_threads)
{
	node_t *dummy = new_node();
	atomic_init(&dummy->next, NULL);
	atomic_init(&q->head, dummy);
	atomic_init(&q->tail, dummy);
}

void enqueue(queue_t *q, unsigned int val)
{
	node_t *tail, *next;

	node_t *node = new_node();
	node->value = val;
	node->next = NULL;

	__VERIFIER_hp_t *hp = get_free_hp();
	while (true) {
		tail = __VERIFIER_hp_protect(hp, &q->tail);
		next = atomic_load_explicit(&tail->next, acquire);
		if (tail != atomic_load_explicit(&q->tail, acquire))
			continue;

		if (next == NULL) {
#ifdef ENQUEUE_WRITE_BUG
			__VERIFIER_final_write(
				atomic_store_explicit(&tail->next, node, release)
			);
			break;
#elif  ENQUEUE_XCHG_BUG
			atomic_exchange_explicit(&tail->next, node, release);
			break;
#else
			if (
				__VERIFIER_final_CAS(
				    atomic_compare_exchange_strong_explicit(&tail->next, &next,
									    node, acqrel, acqrel)
				)
			)
				break; // needs to be RA so that the helping CAS condition is satisfied
#endif
		} else {
			__VERIFIER_helping_CAS(
				atomic_compare_exchange_strong_explicit(&q->tail, &tail, next,
									release, release);
			);
		}
	}
	__VERIFIER_helped_CAS(
		atomic_compare_exchange_strong_explicit(&q->tail, &tail, node, release, release);
	);
}

bool dequeue(queue_t *q, unsigned int *retVal)
{
	node_t *head, *tail, *next;
	unsigned ret;

	__VERIFIER_hp_t *hp_head = get_free_hp();
	__VERIFIER_hp_t *hp_next = get_free_hp();
	while (true) {
		head = __VERIFIER_hp_protect(hp_head, &q->head);
		next = __VERIFIER_hp_protect(hp_next, &head->next);
		if (atomic_load_explicit(&q->head, relaxed) != head)
			continue;

		if (next == NULL)
			return false;

		ret = next->value;
		if (atomic_compare_exchange_strong_explicit(&q->head, &head, next,
							    release, release)) {
			__VERIFIER_optional(
				tail = atomic_load_explicit(&q->tail, acquire);
				if (head == tail) {
					__VERIFIER_helping_CAS(
						atomic_compare_exchange_strong_explicit(&q->tail, &tail, next,
											release, release);
					);
				}
			);
			break;
		}
	}
	*retVal = ret;
	reclaim(head);
	return true;
}
