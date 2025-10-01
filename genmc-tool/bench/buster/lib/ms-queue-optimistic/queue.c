#include <stdlib.h>
#include <genmc.h>
#include "queue.h"
#include "../helper.h"

#define POISON_IDX 0xdeadbeef

static unsigned int free_lists[MAX_THREADS + 1][MAX_FREELIST];

int get_thread_num();

/* Search this thread's free list for a "new" node */
static unsigned int new_node()
{
	int i;
	int t = get_thread_num();
	for (i = 0; i < MAX_FREELIST; i++) {
		unsigned int node = free_lists[t][i];
		if (node) {
			free_lists[t][i] = 0;
			return node;
		}
	}
	/* free_list is empty? */
	assert(0);
	return 0;
}

/* Place this node index back on this thread's free list */
static void reclaim(unsigned int node)
{
	int i;
	int t = get_thread_num();

	/* Don't reclaim NULL node */
	assert(node);

	for (i = 0; i < MAX_FREELIST; i++) {
		/* Should never race with our own thread here */
		unsigned int idx = free_lists[t][i];

		/* Found empty spot in free list */
		if (idx == 0) {
			free_lists[t][i] = node;
			return;
		}
	}
	/* free list is full? */
	assert(0);
}

void init_queue(queue_t *q, int num_threads)
{
	/* Initialize each thread's free list with INITIAL_FREE pointers */
	/* The actual nodes are initialized with poison indexes */
	assert(num_threads < MAX_THREADS);
	for (int i = 0; i <= num_threads; i++) {
		for (int j = 0; j < INITIAL_FREE; j++) {
			free_lists[i][j] = 2 + i * MAX_FREELIST + j;
			atomic_init(&q->nodes[free_lists[i][j]].next, MAKE_POINTER(POISON_IDX, 0));
			atomic_init(&q->nodes[free_lists[i][j]].prev, MAKE_POINTER(POISON_IDX, 0));
		}
	}

	/* initialize queue */
	atomic_init(&q->head, MAKE_POINTER(1, 0));
	atomic_init(&q->tail, MAKE_POINTER(1, 0));
	atomic_init(&q->nodes[1].next, MAKE_POINTER(0, 0));
	atomic_init(&q->nodes[1].prev, MAKE_POINTER(0, 42));
}

void enqueue(queue_t *q, unsigned int val)
{
	unsigned int node;
	pointer tail;
	pointer next;
	pointer tmp;

	node = new_node();
	q->nodes[node].value = val;
	tmp = atomic_load_explicit(&q->nodes[node].next, relaxed);
	set_ptr(&tmp, 0); // NULL
	atomic_store_explicit(&q->nodes[node].next, tmp, relaxed);
	tmp = atomic_load_explicit(&q->nodes[node].prev, relaxed);
	set_ptr(&tmp, 0); // NULL
	atomic_store_explicit(&q->nodes[node].prev, tmp, relaxed);

	while (true) {
		tail = atomic_load_explicit(&q->tail, acquire);

		__VERIFIER_local_write(
		__VERIFIER_final_write(
			atomic_store_explicit(&q->nodes[node].next,
					      MAKE_POINTER(get_ptr(tail), get_count(tail) + 1),
					      release);
		);
		);

		pointer value = MAKE_POINTER(node, get_count(tail) + 1);
		if (atomic_compare_exchange_strong_explicit(&q->tail, &tail, value, release, release)) {
			atomic_store_explicit(&q->nodes[get_ptr(tail)].prev,
					      MAKE_POINTER(node, get_count(tail) + 1),
					      release);
			break;
		}
	}
}

void fix_list(queue_t *q, pointer tail, pointer head)
{
	pointer curr;
	pointer next;

	curr = tail;
	while ((head == atomic_load_explicit(&q->head, acquire)) && (curr != head)) {
		next = atomic_load_explicit(&q->nodes[get_ptr(curr)].next, acquire);

		/* Fix prev pointer */
		atomic_store_explicit(&q->nodes[get_ptr(next)].prev,
				      MAKE_POINTER(get_ptr(curr), get_count(curr) - 1),
				      release);

		curr = MAKE_POINTER(get_ptr(next), get_count(curr) -1);
	}
}

bool dequeue(queue_t *q, unsigned int *retVal)
{
	pointer head;
	pointer tail;
	pointer prev;
	unsigned ret;

	while (true) {
		head = atomic_load_explicit(&q->head, acquire);
		tail = atomic_load_explicit(&q->tail, acquire);
		prev = atomic_load_explicit(&q->nodes[get_ptr(head)].prev, acquire);
		if (atomic_load_explicit(&q->head, acquire) != head)
			continue;
		if (get_ptr(head) == get_ptr(tail)) {
			/* Check for uninitialized 'prev' */
			assert(get_ptr(prev) != POISON_IDX);
			return false;
		}

		if (get_count(prev) != get_count(head)) {
			fix_list(q, tail, head);
			continue;
		}

		ret = q->nodes[get_ptr(prev)].value;

		if (atomic_compare_exchange_strong_explicit(&q->head, &head,
							    MAKE_POINTER(get_ptr(prev), get_count(head) + 1),
							    release, release))
			break;
	}
	*retVal = ret;
	reclaim(get_ptr(head));
	return true;
}
