#ifndef __TTASLOCK_H__
#define __TTASLOCK_H__

struct lock;
typedef struct lock lock_t;

static inline void lock_init(lock_t *l);
static inline void lock_acquire(lock_t *l);
static inline void lock_release(lock_t *l);

#endif /* __TTASLOCK_H__ */
