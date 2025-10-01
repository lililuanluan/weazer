#include <stdatomic.h>
#include "lock.h"
#include "../helper.h"

typedef struct lock  {
	atomic_int state;
} lock_t;

static inline void lock_init(lock_t *l)
{
	atomic_init(&l->state, 0);
}

static inline void await_for_lock(lock_t *l)
{
	while (atomic_load_explicit(&l->state, memory_order_relaxed) != 0)
		;
	return;
}

static inline int try_acquire(lock_t *l)
{
	return atomic_exchange_explicit(&l->state, 1, memory_order_acquire);
}

static inline void lock_acquire(lock_t *l)
{
	while (1) {
		__VERIFIER_optional(await_for_lock(l););
		if (!try_acquire(l))
			return;
	}
}

static inline void lock_release(lock_t *l)
{
	atomic_store_explicit(&l->state, 0, memory_order_release);
}
