#ifndef __MS_QUEUE_OPTIMISTIC_TAG_H__
#define __MS_QUEUE_OPTIMISTIC_TAG_H__

#ifndef MAX_THREADS
# define MAX_THREADS 32
#endif

#ifndef MAX_FREELIST
# define MAX_FREELIST 16 /* Each thread can own up to MAX_FREELIST free nodes */
#endif
#ifndef INITIAL_FREE
# define INITIAL_FREE  4 /* Each thread starts with INITIAL_FREE free nodes */
#endif

#include <stdatomic.h>

#define MAX_NODES			0xff

typedef unsigned long long pointer;
typedef atomic_ullong pointer_t;

#define MAKE_POINTER(ptr, count)	((((pointer)count) << 32) | ptr)
#define PTR_MASK 0xffffffffLL
#define COUNT_MASK (0xffffffffLL << 32)

static inline void set_count(pointer *p, unsigned int val) { *p = (*p & ~COUNT_MASK) | ((pointer)val << 32); }
static inline void set_ptr(pointer *p, unsigned int val) { *p = (*p & ~PTR_MASK) | val; }
static inline unsigned int get_count(pointer p) { return (p & COUNT_MASK) >> 32; }
static inline unsigned int get_ptr(pointer p) { return p & PTR_MASK; }

typedef struct node {
	unsigned int value;
	pointer_t next;
	pointer_t prev;
} node_t;

typedef struct {
	pointer_t head;
	pointer_t tail;
	node_t nodes[MAX_NODES + 1];
} queue_t;

void init_queue(queue_t *q, int num_threads);
void enqueue(queue_t *q, unsigned int val);
bool dequeue(queue_t *q, unsigned int *ret_val);

#endif /* __MS_QUEUE_OPTIMISTIC_TAG_H__ */
