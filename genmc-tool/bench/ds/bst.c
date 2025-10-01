#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef __FINE_BST_H__
#define __FINE_BST_H__

#include <stdbool.h>

int get_thread_num();

struct bst_node *new_node(int elem);
void free_node(struct bst_node *node);

/* BST implementation */
/* NOTE: For fine-BST these require the address of the lock */
#define LOCK_TYPE pthread_mutex_t

#define LOCK(l) pthread_mutex_lock(l)
#define UNLOCK(l) pthread_mutex_unlock(l)

#define BUG_ON(x) assert(!(x))

struct bst_node {
	int val;
	struct bst_node *left;
	struct bst_node *right;
	LOCK_TYPE lock;
};

struct bst_root {
	struct bst_node *root;
	LOCK_TYPE lock;
};

#define __BST_INITIALIZER()                                                                        \
	{                                                                                          \
		.root = NULL, .lock = PTHREAD_MUTEX_INITIALIZER                                    \
	}

#define DEFINE_BST(name) struct bst_root name = __BST_INITIALIZER();

/* PRE: lock_p is the address of the parent's lock, which must be held */
bool insert(struct bst_node **curr, LOCK_TYPE *lock_p, int val)
{
	BUG_ON(curr == NULL);
	if (*curr == NULL) {
		*curr = new_node(val);
		UNLOCK(lock_p);
		return true;
	}

	LOCK(&(*curr)->lock);
	UNLOCK(lock_p);

	if (val < (*curr)->val)
		return insert(&(*curr)->left, &(*curr)->lock, val);
	else if (val > (*curr)->val)
		return insert(&(*curr)->right, &(*curr)->lock, val);

	UNLOCK(&(*curr)->lock);
	return false;
}

bool add(struct bst_root *bst, int val)
{
	LOCK(&bst->lock);
	return insert(&bst->root, &bst->lock, val);
}

bool search(struct bst_node *curr, LOCK_TYPE *lock_p, int val)
{
	if (curr == NULL) {
		UNLOCK(lock_p);
		return false;
	}

	LOCK(&curr->lock);
	UNLOCK(lock_p);

	if (curr->val == val) {
		UNLOCK(&curr->lock);
		return true;
	}
	if (curr->val < val)
		return search(curr->right, &curr->lock, val);
	return search(curr->left, &curr->lock, val);
}

bool contains(struct bst_root *bst, int val)
{
	LOCK(&bst->lock);
	return search(bst->root, &bst->lock, val);
}

bool delete(struct bst_node **curr, LOCK_TYPE *lock_p, int val)
{
	BUG_ON(curr == NULL);
	if (*curr == NULL) {
		UNLOCK(lock_p);
		return false;
	}

	LOCK(&(*curr)->lock);

	if (val < (*curr)->val) {
		UNLOCK(lock_p);
		return delete (&(*curr)->left, &(*curr)->lock, val);
	}
	if (val > (*curr)->val) {
		UNLOCK(lock_p);
		return delete (&(*curr)->right, &(*curr)->lock, val);
	}

	/* Found the node to delete */
	if ((*curr)->left == NULL || (*curr)->right == NULL) {
		struct bst_node *prev_curr = *curr;
		*curr = ((*curr)->left == NULL) ? (*curr)->right : (*curr)->left;
		UNLOCK(&prev_curr->lock);
		UNLOCK(lock_p);
		free_node(prev_curr);
		return true;
	}

	struct bst_node *succ = (*curr)->right;
	struct bst_node *succ_p = succ;

	LOCK(&succ->lock);
	while (succ->left != NULL) {
		if (succ_p != succ)
			UNLOCK(&succ_p->lock);
		succ_p = succ;
		succ = succ->left;
		LOCK(&succ->lock);
	}

	/* Check if succ_p == succ */
	if (succ_p == succ) {
		(*curr)->right = succ->right;
	} else {
		succ_p->left = succ->right;
		UNLOCK(&succ_p->lock);
	}
	(*curr)->val = succ->val;
	UNLOCK(&succ->lock);
	UNLOCK(&(*curr)->lock);
	UNLOCK(lock_p);
	free_node(succ);
	return true;
}

bool remove(struct bst_root *bst, int val)
{
	int retval = true;

	LOCK(&bst->lock);
	retval = delete (&bst->root, &bst->lock, val);
	return retval;
}

void inorder(struct bst_node *node)
{
	if (node == NULL)
		return;

	inorder(node->left);
	printf("%d\n", node->val);
	inorder(node->right);
}

void traverse(struct bst_root *bst)
{
	LOCK(&bst->lock);
	inorder(bst->root);
	UNLOCK(&bst->lock);
}

#endif /* __FINE_BST_H__ */

/* Driver code */
#ifndef MAX_THREADS
#define MAX_THREADS 32
#endif
#ifndef MAX_FREELIST
#define MAX_FREELIST 32 /* Each thread can own up to MAX_FREELIST free nodes */
#endif

#ifdef CONFIG_BST_ADDERS
#define DEFAULT_ADDERS (CONFIG_BST_ADDERS)
#else
#define DEFAULT_ADDERS 0
#endif
#ifdef CONFIG_BST_SEEKERS
#define DEFAULT_SEEKERS (CONFIG_BST_SEEKERS)
#else
#define DEFAULT_SEEKERS 2
#endif
#ifdef CONFIG_BST_REMOVERS
#define DEFAULT_REMOVERS (CONFIG_BST_REMOVERS)
#else
#define DEFAULT_REMOVERS 0
#endif

static int adders = DEFAULT_ADDERS, seekers = DEFAULT_SEEKERS, removers = DEFAULT_REMOVERS;
static int num_threads;

DEFINE_BST(mybst);

pthread_t threads[MAX_THREADS + 1];
int param[MAX_THREADS + 1];
struct bst_node free_lists[MAX_THREADS + 1][MAX_FREELIST];
unsigned int free_index[MAX_THREADS + 1];

int __thread tid;

void set_thread_num(int i) { tid = i; }

int get_thread_num() { return tid; }

struct bst_node *new_node(int elem)
{
	int t = get_thread_num();

	assert(free_index[t] < MAX_FREELIST);
	free_lists[t][free_index[t]].val = elem;
	free_lists[t][free_index[t]].left = NULL;
	free_lists[t][free_index[t]].right = NULL;
	return &free_lists[t][free_index[t]++];
}

void free_node(struct bst_node *node) {}

/* Should be called before threads are spawned (from main()) */
void init()
{
	num_threads = adders + seekers + removers + 1;
	for (int j = 0; j < num_threads; j++)
		param[j] = j;

	/* add(&mybst, 15); */
	/* add(&mybst, 11); */
	/* add(&mybst, 20); */
	/* add(&mybst, 18); */
	add(&mybst, 8);
	add(&mybst, 4);
	add(&mybst, 12);
	add(&mybst, 10);
}

#define BASE(tid) (((tid) % 2 == 0) ? tid : tid + 8)

void *thread_add(void *tid)
{
	int t = (*(int *)tid);
	set_thread_num(t);

	add(&mybst, (BASE(t) * 7) % 16);
	return NULL;
}

void *thread_seek(void *tid)
{
	int t = (*(int *)tid);
	set_thread_num(t);

	contains(&mybst, (BASE(t) * 7) % 16);
	return NULL;
}

void *thread_del(void *tid) { return NULL; }

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

	/* i = 1; */
	/* for (int j = 0; j < adders; j++, i++) */
	/* 	pthread_join(threads[i], NULL); */
	/* for (int j = 0; j < seekers; j++, i++) */
	/* 	pthread_join(threads[i], NULL); */
	/* for (int j = 0; j < removers; j++, i++) */
	/* 	pthread_join(threads[i], NULL); */

	/* traverse(&mybst); */
	/* printf("---\n"); */

	return 0;
}
