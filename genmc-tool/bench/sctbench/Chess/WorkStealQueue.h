#ifndef WORK_STEAL_Q
#define WORK_STEAL_Q

#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <genmc.h>


#ifndef MyObjTypeDef
#define MyObjTypeDef int
#endif

typedef struct WorkStealQueue {

	// Lock variable:
	// 1 - lock acquired
	// 0 - lock free
	//
	void* _Atomic lock;
	// Tells how many times we should retry before giving up the
	// thread quanta. On single proc machines this should be 0
	// because the lock cannot be released while we are running so
	// there is no point in spinning...
	//
	unsigned long maxRetriesBeforeSleep;



	// A 'WorkStealQueue' always runs its code in a single OS thread. We call this the
	// 'bound' thread. Only the code in the Take operation can be executed by
	// other 'foreign' threads that try to steal work.
	//
	// The queue is implemented as an array. The head and tail index this
	// array. To avoid copying elements, the head and tail index the array modulo
	// the size of the array. By making this a power of two, we can use a cheap
	// bit-and operation to take the modulus. The "mask" is always equal to the
	// size of the task array minus one (where the size is a power of two).
	//
	// The head and tail are   as they can be updated from different OS threads.
	// The "head" is only updated by foreign threads as they Take (steal) a task from
	// this queue. By putting a lock in Take, there is at most one foreign thread
	// changing head at a time. The tail is only updated by the bound thread.
	//
	// invariants:
	//   tasks.length is a power of 2
	//   mask == tasks.length-1
	//   head is only written to by foreign threads
	//   tail is only written to by the bound thread
	//   At most one foreign thread can do a Take
	//   All methods except Take are executed from a single bound thread
	//   tail points to the first unused location
	//
#define MaxSize  (1024 * 1024)
#define InitialSize  (1024)

	_Atomic long head;
	_Atomic long tail;

	MyObjTypeDef* elems;         // the array of tasks 
	long mask;


} WorkStealQueue;

long readV(_Atomic long* v) {
	long expected = 0;
	const long desired = 0;
	atomic_compare_exchange_strong_explicit(v, &expected, desired, memory_order_seq_cst, memory_order_seq_cst);
	return expected;
}

void writeV(_Atomic long* v, long w) {
	atomic_exchange(v, w);
}


void init_queue(WorkStealQueue* this, unsigned long retries /*=4*/) {
	this->lock = (void*)0;
	this->maxRetriesBeforeSleep = 0;
	const long size = MaxSize;
	this->elems = (MyObjTypeDef*)malloc(size * sizeof(MyObjTypeDef));
	writeV(&(this->head), 0);
	writeV(&(this->tail), 0);
	this->mask = size - 1;
}


void Acquire(WorkStealQueue* this) {
	void* old;
	unsigned long retries = 0;

	do {
		old = atomic_exchange(&(this->lock), (void*)1);

		if (old == (void*)0) {
			// If we got here, then we grabed the lock.
			// old == 0 means that this thread "transitioned" the lock
			// from an unlocked state (0) to a locked state (1)
			//
			break;
		}

		// Give up the current thread quanta if we did not aquire the
		// lock and we exceeded the max retry count
		//
		if (++retries >= this->maxRetriesBeforeSleep) {
			retries = 0;
		}
	} while (1);
}

void Release(WorkStealQueue* this) {
	this->lock = (void*)0;
}

bool Steal(WorkStealQueue* this, MyObjTypeDef* result) {
	bool found;
	Acquire(this);
	long h = readV(&this->head);
	writeV(&this->head, h + 1);
	if (h < readV(&this->tail)) {
		*result = (this->elems)[h & (this->mask)];
	}
	else {
		// failure: either empty or single element interleaving with pop
				//
		writeV(&(this->head), h);
		found = false;
	}
	Release(this);
	return found;
}

bool SyncPop(WorkStealQueue* this, MyObjTypeDef* result)
{
	bool found;

	Acquire(this);

	// ensure that no Steal interleaves with this pop
	//
	long t = readV(&(this->tail)) - 1;
	writeV(&(this->tail), t);
	if (readV(&(this->head)) <= t)
	{
		// == (head <= tail)
		//
		*result = (this->elems)[t & (this->mask)];
		found = true;
	}
	else
	{
		writeV(&(this->tail), t + 1);       // restore tail
		found = false;
	}
	if (readV(&(this->head)) > t)
	{
		// queue is empty: reset head and tail
		//
		writeV(&(this->head), 0);
		writeV(&(this->tail), 0);
		found = false;
	}
	Release(this);
	return found;
}

bool Pop(WorkStealQueue* this, MyObjTypeDef* result) {
	long t = readV(&(this->tail)) - 1;
	writeV(&(this->tail), t);
	if (readV(&(this->head)) <= t) {
		// BUG:  writeV(tail, t);

		// == (head <= tail)
		//
		*result = (this->elems)[t & (this->mask)];
		return true;
	}
	else {
		writeV(&(this->tail), t + 1);
		return SyncPop(this, result);
	}
}

void SyncPush(WorkStealQueue* this, MyObjTypeDef elem) {
	Acquire(this);
	long h = readV(&(this->head));
	long count = readV(&(this->tail)) - h;
	h = h & (this->mask);           // normalize head
	writeV(&(this->head), h);
	writeV(&(this->tail), h + count);
	if (count >= this->mask)
	{
		// == (count >= size-1)
		//
		long newsize = (this->mask == 0 ? InitialSize : 2 * (this->mask + 1));

		assert(newsize < MaxSize);

		MyObjTypeDef* newtasks = (MyObjTypeDef*)malloc(newsize * sizeof(MyObjTypeDef));
		for (long i = 0; i < count; i++)
		{
			newtasks[i] = (this->elems)[(h + i) & (this->mask)];
		}
		free(this->elems);

		this->elems = newtasks;
		this->mask = newsize - 1;
		writeV(&(this->head), 0);
		writeV(&(this->tail), count);
	}

	assert(count < this->mask);

	// push the element
	//
	long t = readV(&(this->tail));
	(this->elems)[t & (this->mask)] = elem;
	writeV(&(this->tail), t + 1);
	Release(this);
}

void Push(WorkStealQueue* this, MyObjTypeDef elem) {
	long t = readV(&(this->tail));
	// #define BUG3
#ifdef BUG3
	if (t < readV(&(this->head)) + (this->mask) + 1 && t < MaxSize)
#else
	if (t < readV(&(this->head)) + (this->mask)   // == t < head + size - 1
		&& t < MaxSize)
#endif
	{
		(this->elems)[t & (this->mask)] = elem;
		writeV(&(this->tail), t + 1);
	}
	else {
		SyncPush(this, elem);
	}
}






#undef Ty
#endif