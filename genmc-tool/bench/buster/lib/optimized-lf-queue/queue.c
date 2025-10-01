#include <stdlib.h>
#include <assert.h>
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
	assert(MAX_NODES > 2 + num_threads * MAX_FREELIST + MAX_FREELIST);
	for (int i = 0; i <= num_threads; i++) {
		for (int j = 0; j < INITIAL_FREE; j++) {
			free_lists[i][j] = 2 + i * MAX_FREELIST + j;
			atomic_init(&q->nodes[free_lists[i][j]].next, MAKE_POINTER(POISON_IDX, 0));
		}
	}

	/* initialize queue */
	atomic_init(&q->head, MAKE_POINTER(1, 0));
	atomic_init(&q->tail, MAKE_POINTER(1, 0));
	atomic_init(&q->nodes[1].next, MAKE_POINTER(0, 0));
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

	while (true) {
		tail = atomic_load_explicit(&q->tail, acquire);

		/* Try to link the node to the tail */
		pointer cmp = MAKE_POINTER(0, 0);
		pointer value = MAKE_POINTER(node, 0);
#ifdef ENQUEUE_WRITE_BUG
		__VERIFIER_final_write(
			atomic_store_explicit(&q->nodes[get_ptr(tail)].next, value,
					      release));
			break;
#elif  ENQUEUE_XCHG_BUG
			atomic_exchange_explicit(&q->nodes[get_ptr(tail)].next, value,
						 release);
			break;
#else
		if (
			// __VERIFIER_final_CAS(
			    atomic_compare_exchange_strong_explicit(&q->nodes[get_ptr(tail)].next, &cmp, value,
								    release, release))
								// )
			break;
#endif

		/* tail was not pointing to the last cell; try to advance it */
		unsigned int ptr = get_ptr(atomic_load_explicit(&q->nodes[get_ptr(tail)].next, acquire));
		value = MAKE_POINTER(ptr, get_count(tail) + 1);
		// __VERIFIER_helping_CAS(
			atomic_compare_exchange_strong_explicit(&q->tail, &tail, value,
								release, release);
		// );
	}
	// __VERIFIER_helped_CAS(
		atomic_compare_exchange_strong_explicit(&q->tail, &tail,
							MAKE_POINTER(get_ptr(node), get_count(tail) + 1),
							release, release);
	// );
}

bool dequeue(queue_t *q, unsigned int *retVal)
{
	pointer head;
	pointer tail;
	pointer next;
	unsigned ret;
	while (true) {
		head = 
		// __VERIFIER_speculative_read(
			atomic_load_explicit(&q->head, acquire);
		// );
		tail = atomic_load_explicit(&q->tail, acquire);
		next = atomic_load_explicit(&q->nodes[get_ptr(head)].next, acquire);

		if (get_count(head) !=
		    get_count(
				// __VERIFIER_confirming_read(
					atomic_load_explicit(&q->head, acquire)))
				// )
			continue;

		// paper erroneously loads head below
		if (get_ptr(head) == get_ptr(atomic_load_explicit(&q->tail, acquire))) {
			if (get_ptr(next) == 0) // NULL
				return false;

			pointer cmp = MAKE_POINTER(get_ptr(head), get_count(tail));
			pointer value = MAKE_POINTER(get_ptr(next), get_count(tail) + 1);
			// __VERIFIER_helping_CAS(
				atomic_compare_exchange_strong_explicit(&q->tail, &cmp, value,
									release, release);
			// );
		} else if (get_ptr(next) != 0) { // NULL; ENDFIFO in the paper
			ret = q->nodes[get_ptr(next)].value;
			pointer value = MAKE_POINTER(get_ptr(next), get_count(head) + 1);
			if (
				// __VERIFIER_confirming_CAS(
					atomic_compare_exchange_strong_explicit(&q->head, &head, value,
											      release, release))
												// )
				break;
		}
	}
	*retVal = ret;
	reclaim(get_ptr(head));
	return true;
}
