#include <pthread.h>
#include <stdlib.h>

#include <assert.h>
#ifndef __FINE_SET_H__
#define __FINE_SET_H__

#include <stdbool.h>
#ifndef __LIST_H__
#define __LIST_H__

/* List infrastructure */
#undef offsetof
#ifdef __compiler_offsetof
#define offsetof(TYPE, MEMBER) __compiler_offsetof(TYPE, MEMBER)
#else
#define offsetof(TYPE, MEMBER) ((size_t) & ((TYPE *)0)->MEMBER)
#endif

#define container_of(ptr, type, member)                                                            \
	({                                                                                         \
		const __typeof__(((type *)0)->member) *__mptr = (ptr);                             \
		(type *)((char *)__mptr - offsetof(type, member));                                 \
	})

#define LIST_POISON1 ((void *)0x666)
#define LIST_POISON2 ((void *)0xdeadbeef)

struct list_head {
	struct list_head *next;
	struct list_head *prev;
};

#define LIST_HEAD_INIT(name)                                                                       \
	{                                                                                          \
		&(name), &(name)                                                                   \
	}

#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

static inline void __list_add(struct list_head *new, struct list_head *prev, struct list_head *next)
{
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;
}

static inline void list_add(struct list_head *new, struct list_head *head)
{
	__list_add(new, head, head->next);
}

static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
	__list_add(new, head->prev, head);
}

static inline void __list_del(struct list_head *prev, struct list_head *next)
{
	next->prev = prev;
	prev->next = next;
}

static inline void list_del(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
	entry->next = LIST_POISON1;
	entry->prev = LIST_POISON2;
}

static inline int list_empty(const struct list_head *head) { return head->next == head; }

/**
 * list_entry - get the struct for this entry
 * @ptr:	the &struct list_head pointer.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_head within the struct.
 */
#define list_entry(ptr, type, member) container_of(ptr, type, member)

/**
 * list_first_entry - get the first element from a list
 * @ptr:	the list head to take the element from.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_head within the struct.
 *
 * Note, that list is expected to be not empty.
 */
#define list_first_entry(ptr, type, member) list_entry((ptr)->next, type, member)

/**
 * list_next_entry - get the next element in list
 * @pos:	the type * to cursor
 * @member:	the name of the list_head within the struct.
 */
#define list_next_entry(pos, member) list_entry((pos)->member.next, typeof(*(pos)), member)

/**
 * list_for_each	-	iterate over a list
 * @pos:	the &struct list_head to use as a loop cursor.
 * @head:	the head for your list.
 */
#define list_for_each(pos, head) for (pos = (head)->next; pos != (head); pos = pos->next)

/**
 * list_for_each_entry	-	iterate over list of given type
 * @pos:	the type * to use as a loop cursor.
 * @head:	the head for your list.
 * @member:	the name of the list_head within the struct.
 */
#define list_for_each_entry(pos, head, member)                                                     \
	for (pos = list_first_entry(head, typeof(*pos), member); &pos->member != (head);           \
	     pos = list_next_entry(pos, member))

#endif /* __LIST_H__ */

/* API that the Driver must implement */
int get_thread_num();

struct set_node *new_node(int key, int elem);
void free_node(struct set_node *node);

/* Set definition */
#define lock(l) pthread_mutex_lock(&(l))
#define unlock(l) pthread_mutex_unlock(&(l))

#define hash(x) (x)

struct set_node {
	int key;
	int val;
	pthread_mutex_t lock;
	struct list_head list;
};

struct set_head {
	pthread_mutex_t lock;
	struct list_head list;
};

void set_init(struct set_head *set) { INIT_LIST_HEAD(&set->list); }

bool insert(struct set_node *curr, int key, int elem)
{
	if (key == curr->key)
		return false;

	struct set_node *node = new_node(key, elem);
	list_add_tail(&node->list, &curr->list);
	return true;
}

bool delete(struct set_node *curr, int key)
{
	if (key != curr->key)
		return false;

	list_del(&curr->list);
	return true;
}

bool contains(struct set_head *set, int elem)
{
	int key = hash(elem);
	struct set_node *curr;
	struct list_head *pred; /* entry can be set_head or set_node */
	int retval = false;

	lock(set->lock);
	pred = &set->list;

	if (list_empty(&set->list))
		goto unlock_pred;

	list_for_each_entry(curr, &set->list, list)
	{
		lock(curr->lock);
		if (curr->key == key) {
			retval = true;
			goto unlock_curr;
		}
		if (curr->key > key)
			goto unlock_curr;

		if (pred == &set->list)
			unlock(container_of(pred, struct set_head, list)->lock);
		else
			unlock(container_of(pred, struct set_node, list)->lock);
		pred = &curr->list;
	}

	goto unlock_pred;

unlock_curr:
	unlock(curr->lock);
unlock_pred:
	if (pred == &set->list)
		unlock(container_of(pred, struct set_head, list)->lock);
	else
		unlock(container_of(pred, struct set_node, list)->lock);
	return retval;
}

bool add(struct set_head *set, int elem)
{
	int key = hash(elem);
	struct set_node *curr;
	struct list_head *pred; /* entry can be set_head or set_node */
	int retval = true;

	lock(set->lock);
	pred = &set->list;

	if (list_empty(&set->list)) {
		struct set_node *node = new_node(key, elem);
		list_add(&node->list, &set->list);
		goto unlock_pred;
	}

	list_for_each_entry(curr, &set->list, list)
	{
		lock(curr->lock);
		if (curr->key >= key) {
			retval = insert(curr, key, elem);
			goto unlock_curr;
		}
		if (pred == &set->list)
			unlock(container_of(pred, struct set_head, list)->lock);
		else
			unlock(container_of(pred, struct set_node, list)->lock);
		pred = &curr->list;
	}

	lock(set->lock);
	struct set_node *node = new_node(key, elem);
	list_add_tail(&node->list, &set->list);
	unlock(set->lock);
	goto unlock_pred;

unlock_curr:
	unlock(curr->lock);
unlock_pred:
	if (pred == &set->list)
		unlock(container_of(pred, struct set_head, list)->lock);
	else
		unlock(container_of(pred, struct set_node, list)->lock);
	return retval;
}

bool remove(struct set_head *set, int elem)
{
	int key = hash(elem);
	struct set_node *curr;
	struct list_head *pred; /* entry can be set_head or set_node */
	int retval = false;

	lock(set->lock);
	pred = &set->list;

	if (list_empty(&set->list))
		goto unlock_pred;

	list_for_each_entry(curr, &set->list, list)
	{
		lock(curr->lock);
		if (curr->key >= key) {
			retval = delete (curr, key);
			goto unlock_curr;
		}
		if (pred == &set->list)
			unlock(container_of(pred, struct set_head, list)->lock);
		else
			unlock(container_of(pred, struct set_node, list)->lock);
		pred = &curr->list;
	}

	/* Element not found in set */
	goto unlock_pred;

unlock_curr:
	unlock(curr->lock);
unlock_pred:
	if (pred == &set->list)
		unlock(container_of(pred, struct set_head, list)->lock);
	else
		unlock(container_of(pred, struct set_node, list)->lock);
	return retval;
}

#endif /* __FINE_SET_H__ */

/* Driver code */
#ifndef MAX_THREADS
#define MAX_THREADS 32
#endif
#ifndef MAX_FREELIST
#define MAX_FREELIST 32 /* Each thread can own up to MAX_FREELIST free nodes */
#endif

#ifdef CONFIG_SET_ADDERS
#define DEFAULT_ADDERS (CONFIG_SET_ADDERS)
#else
#define DEFAULT_ADDERS 2
#endif
#ifdef CONFIG_SET_SEEKERS
#define DEFAULT_SEEKERS (CONFIG_SET_SEEKERS)
#else
#define DEFAULT_SEEKERS 0
#endif
#ifdef CONFIG_SET_REMOVERS
#define DEFAULT_REMOVERS (CONFIG_SET_REMOVERS)
#else
#define DEFAULT_REMOVERS 0
#endif

static int adders = DEFAULT_ADDERS, seekers = DEFAULT_SEEKERS, removers = DEFAULT_REMOVERS;
static int num_threads;
static struct set_head myset;

pthread_t threads[MAX_THREADS + 1];
int param[MAX_THREADS + 1];
struct set_node free_lists[MAX_THREADS + 1][MAX_FREELIST];
unsigned int free_index[MAX_THREADS + 1];

int __thread tid;

void set_thread_num(int i) { tid = i; }

int get_thread_num() { return tid; }

struct set_node *new_node(int key, int elem)
{
	int t = get_thread_num();

	assert(free_index[t] < MAX_FREELIST);
	free_lists[t][free_index[t]].key = key;
	free_lists[t][free_index[t]].val = elem;
	return &free_lists[t][free_index[t]++];
}

/* Should be called before threads are spawned (from main()) */
void init()
{
	num_threads = adders + seekers + removers + 1;
	for (int j = 0; j < num_threads; j++)
		param[j] = j;

	set_init(&myset);
	for (int i = 0; i < 8; i++) {
		if (i % 2 == 0)
			add(&myset, i);
	}
}

void *thread_add(void *tid)
{
	int t = (*(int *)tid);
	set_thread_num(t);

	add(&myset, (t * 7) % 12);
	return NULL;
}

void *thread_seek(void *tid)
{
	int t = (*(int *)tid);
	set_thread_num(t);

	contains(&myset, (t * 7) % 12);
	return NULL;
}

void *thread_del(void *arg) { return NULL; }

int main()
{
	/* Store PIDs starting from the first entry of threads[] */
	int i = 1;

	init();
	for (int j = 0; j < adders; j++, i++)
		pthread_create(&threads[i], NULL, thread_add, &param[i]);
	for (int j = 0; j < seekers; j++, i++)
		pthread_create(&threads[i], NULL, thread_seek, &param[i]);
	for (int j = 0; j < removers; j++, i++)
		pthread_create(&threads[i], NULL, thread_del, &param[i]);

	i = 1;
	for (int j = 0; j < adders; j++, i++)
		pthread_join(threads[i], NULL);
	for (int j = 0; j < seekers; j++, i++)
		pthread_join(threads[i], NULL);
	for (int j = 0; j < removers; j++, i++)
		pthread_join(threads[i], NULL);

	return 0;
}
