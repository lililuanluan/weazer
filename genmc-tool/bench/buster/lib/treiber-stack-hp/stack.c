#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include "stack.h"
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

void init_stack(stack_t *s, int num_threads)
{
	/* initialize stack */
	atomic_init(&s->top, 0);
}

void push(stack_t *s, unsigned int val)
{
	node_t *node = new_node();
	node_t *top;

	node->value = val;
	do {
		top = atomic_load_explicit(&s->top, acquire);
		__VERIFIER_final_write(
			atomic_store_explicit(&node->next, top, relaxed);
		);
#ifdef PUSH_WRITE_BUG
		atomic_store_explicit(&s->top, node, release);
	} while (0);
#elif PUSH_XCHG_BUG
		atomic_exchange_explicit(&s->top, node, release);
	} while (0);
#else
	} while (!atomic_compare_exchange_strong_explicit(&s->top, &top,
							  node, release, relaxed));
#endif
}

bool pop(stack_t *s, unsigned int *ret_val)
{
	node_t *top, *next;

	__VERIFIER_hp_t *hp = get_free_hp();
	do {
		top = __VERIFIER_hp_protect(hp, &s->top);
		if (top == NULL)
			return false;

		next = atomic_load_explicit(&top->next, relaxed);
	} while (!atomic_compare_exchange_strong_explicit(&s->top, &top,
							  next, release, relaxed));
	*ret_val = top->value;
	/* Reclaim the used slot */
	reclaim(top);
	return true;
}
