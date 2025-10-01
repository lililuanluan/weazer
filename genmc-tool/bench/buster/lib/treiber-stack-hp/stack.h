#ifndef __TREIBER_STACK_HP_H__
#define __TREIBER_STACK_HP_H__

#include <stdbool.h>
#include <stdatomic.h>
#include <genmc.h>

struct node;
typedef struct node node_t;

typedef struct {
	_Atomic(node_t *) top;
} stack_t;

void init_stack(stack_t *s, int num_threads);
void push(stack_t *s, unsigned int val);
bool pop(stack_t *s, unsigned int *ret_val);

#endif /* __TREIBER_STACK_HP_H__ */
