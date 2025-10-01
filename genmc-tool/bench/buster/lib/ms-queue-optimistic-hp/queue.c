#include <stdlib.h>
#include <genmc.h>
#include "queue.h"
#include "../helper.h"

typedef struct node {
	unsigned int value;
	_Atomic(struct node *) next;
	_Atomic(struct node *) prev;
} node_t;

#define POISON_PTR ((void *) 0xdeadbeef)

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
	node_t *dummy = new_node(); //  (0, 5): MALLOC
	atomic_init(&dummy->next, NULL);	// (0, 6): Wna (HU#24, 0x0)
	atomic_init(&dummy->prev, POISON_PTR); // (0, 7): Wna (HU#32, 0xdeadbeef)
	atomic_init(&q->head, dummy); // (0, 8): Wna (GU#17000, 0x10)
	atomic_init(&q->tail, dummy); // (0, 9): Wna (GU#17008, 0x10)
}

void enqueue(queue_t *q, unsigned int val)
{
	node_t *tail, *next;

	node_t *node = new_node();
	// these 3 are local variables
	node->value = val;	// Wna (HU#, 1)
	node->next = NULL;  // Wsc (HU#, 0)
	node->prev = NULL;  // Wsc (HU#, 0)

	__VERIFIER_hp_t *hp = get_free_hp();
	while (true) {
		// reads q->tail in acq order, protect it, returns it
		tail = __VERIFIER_hp_protect(hp, &q->tail/*Racq (GU#17008) [(0, 9)]*/);

		__VERIFIER_final_write(
			atomic_store_explicit(&node->next, tail, relaxed);
		);

		if (atomic_compare_exchange_strong_explicit(&q->tail, &tail, node, release, release)) {
			// the other thread can take its turn here, because it will hold a different tail
			// it would be wrong if it werre q->tail->prev...
			atomic_store_explicit(&tail->prev, node, release);
			break;
		}
	}
}

void fix_list(queue_t *q, node_t *tail, node_t *head)
{
	__VERIFIER_hp_t *hp_next = get_free_hp();
	node_t *curr;
	node_t *next;

	curr = tail;
	while ((head == atomic_load_explicit(&q->head, acquire)) && (curr != head)) {
		/* next = atomic_load_explicit(&curr->next, acquire); */
		next = __VERIFIER_hp_protect(hp_next, &curr->next);

		/* Fix prev pointer */
		atomic_store_explicit(&next->prev, curr, release);

		curr = next;
	}
}

bool dequeue(queue_t *q, unsigned int *retVal)
{
	node_t *head, *tail, *prev;
	unsigned ret;

	__VERIFIER_hp_t *hp_head = get_free_hp();
	__VERIFIER_hp_t *hp_prev = get_free_hp();
	__VERIFIER_hp_t *hp_tail = get_free_hp();
	while (true) {
		head = __VERIFIER_hp_protect(hp_head, &q->head);
		/* tail = atomic_load_explicit(&q->tail, acquire); */
		tail = __VERIFIER_hp_protect(hp_tail, &q->tail);
		prev = __VERIFIER_hp_protect(hp_prev, &head->prev);
		if (atomic_load_explicit(&q->head, acquire) != head)
			continue;
		if (head == tail) {
			/* Check for uninitialized 'prev' */
			/* assert(prev != POISON_PTR); */
			return false;
		}

		if (prev == NULL || prev == POISON_PTR) {
			__VERIFIER_optional(
				fix_list(q, tail, head);
			);
			continue;
		}

		ret = prev->value;

		if (atomic_compare_exchange_strong_explicit(&q->head, &head, prev, release, release))
			break;
	}
	*retVal = ret;
	reclaim(head);
	return true;
}
