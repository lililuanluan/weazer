#include "common.h"
#include "threads-model.h"
#include "action.h"
#include "mypthread.h"

#include "snapshot-interface.h"
#include "datarace.h"

#include "mutex.h"
#include <condition_variable>

/* global "model" object */
#include "model.h"
#include "execution.h"
#include <errno.h>

int pthread_create(pthread_t *t, const pthread_attr_t * attr,
									 pthread_start_t start_routine, void * arg) {
	createModelIfNotExist();
	struct pthread_params params = { start_routine, arg };

	/* seq_cst is just a 'don't care' parameter */
	model->switch_thread(new ModelAction(PTHREAD_CREATE, std::memory_order_seq_cst, t, (uint64_t)&params));

	return 0;
}

int pthread_join(pthread_t t, void **value_ptr) {
	ModelExecution *execution = model->get_execution();
	Thread *th = execution->get_pthread(t);

	model->switch_thread(new ModelAction(PTHREAD_JOIN, std::memory_order_seq_cst, th, id_to_int(th->get_id())));

	if ( value_ptr ) {
		// store return value
		void *rtval = th->get_pthread_return();
		*value_ptr = rtval;
	}
	return 0;
}

int pthread_detach(pthread_t t) {
	//Doesn't do anything
	//Return success
	return 0;
}

/* Take care of both pthread_yield and c++ thread yield */
int sched_yield() {
	model->switch_thread(new ModelAction(THREAD_YIELD, std::memory_order_seq_cst, thread_current(), VALUE_NONE));
	return 0;
}

void pthread_exit(void *value_ptr) {
	Thread * th = thread_current();
	th->set_pthread_return(value_ptr);
	model->switch_thread(new ModelAction(THREADONLY_FINISH, std::memory_order_seq_cst, th));
	//Need to exit so we don't return to the program
	real_pthread_exit(NULL);
}

int pthread_mutex_init(pthread_mutex_t *p_mutex, const pthread_mutexattr_t * attr) {
	createModelIfNotExist();
	int mutex_type = PTHREAD_MUTEX_DEFAULT;
	if (attr != NULL)
		pthread_mutexattr_gettype(attr, &mutex_type);

	cdsc::snapmutex *m = new cdsc::snapmutex(mutex_type);

	ModelExecution *execution = model->get_execution();
	execution->getMutexMap()->put(p_mutex, m);

	return 0;
}

int pthread_mutex_lock(pthread_mutex_t *p_mutex) {
	createModelIfNotExist();
	ModelExecution *execution = model->get_execution();

	/* to protect the case where PTHREAD_MUTEX_INITIALIZER is used
	   instead of pthread_mutex_init, or where *p_mutex is not stored
	   in the execution->mutex_map for some reason. */
	if (!execution->getMutexMap()->contains(p_mutex)) {
		pthread_mutex_init(p_mutex, NULL);
	}

	cdsc::snapmutex *m = execution->getMutexMap()->get(p_mutex);

	if (m != NULL) {
		m->lock();
	} else {
		return 1;
	}

	return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *p_mutex) {
	createModelIfNotExist();
	ModelExecution *execution = model->get_execution();
	cdsc::snapmutex *m = execution->getMutexMap()->get(p_mutex);
	return m->try_lock() ? 0 : EBUSY;
}
int pthread_mutex_unlock(pthread_mutex_t *p_mutex) {
	ModelExecution *execution = model->get_execution();
	cdsc::snapmutex *m = execution->getMutexMap()->get(p_mutex);

	if (m != NULL) {
		m->unlock();
	} else {
		printf("try to unlock an untracked pthread_mutex\n");
		return 1;
	}

	return 0;
}

int pthread_mutex_timedlock (pthread_mutex_t *__restrict p_mutex,
														 const struct timespec *__restrict abstime) {
// timedlock just gives the option of giving up the lock, so return and let the scheduler decide which thread goes next
	createModelIfNotExist();
	ModelExecution *execution = model->get_execution();

	/* to protect the case where PTHREAD_MUTEX_INITIALIZER is used
	   instead of pthread_mutex_init, or where *p_mutex is not stored
	   in the execution->mutex_map for some reason. */
	if (!execution->getMutexMap()->contains(p_mutex)) {
		pthread_mutex_init(p_mutex, NULL);
	}

	cdsc::snapmutex *m = execution->getMutexMap()->get(p_mutex);

	if (m != NULL) {
		m->lock();
		return 0;
	}

	return 1;
}

pthread_t pthread_self() {
	createModelIfNotExist();
	Thread* th = model->get_current_thread();
	return (pthread_t)th->get_id();
}

int pthread_key_delete(pthread_key_t) {
	model_print("key_delete is called\n");
	return 0;
}

int pthread_cond_init(pthread_cond_t *p_cond, const pthread_condattr_t *attr) {
	cdsc::snapcondition_variable *v = new cdsc::snapcondition_variable();

	ModelExecution *execution = model->get_execution();
	execution->getCondMap()->put(p_cond, v);
	return 0;
}

int pthread_cond_wait(pthread_cond_t *p_cond, pthread_mutex_t *p_mutex) {
	ModelExecution *execution = model->get_execution();
	if ( !execution->getCondMap()->contains(p_cond) )
		pthread_cond_init(p_cond, NULL);
	if ( !execution->getMutexMap()->contains(p_mutex) )
		pthread_mutex_init(p_mutex, NULL);

	cdsc::snapcondition_variable *v = execution->getCondMap()->get(p_cond);
	cdsc::snapmutex *m = execution->getMutexMap()->get(p_mutex);

	v->wait(*m);
	return 0;
}

int pthread_cond_timedwait(pthread_cond_t *p_cond,
													 pthread_mutex_t *p_mutex, const struct timespec *abstime) {
	ModelExecution *execution = model->get_execution();
	if ( !execution->getCondMap()->contains(p_cond) )
		pthread_cond_init(p_cond, NULL);
	if ( !execution->getMutexMap()->contains(p_mutex) )
		pthread_mutex_init(p_mutex, NULL);

	cdsc::snapcondition_variable *v = execution->getCondMap()->get(p_cond);
	cdsc::snapmutex *m = execution->getMutexMap()->get(p_mutex);

	uint64_t time = abstime->tv_sec * 1000000000 + abstime->tv_nsec;
	ModelAction * timed_wait = new ModelAction(ATOMIC_TIMEDWAIT, std::memory_order_seq_cst, v, (uint64_t) m);
	timed_wait->set_time(time);
	if (model->switch_thread(timed_wait) == ETIMEDOUT) {
		//model_print("thread %u wait timedout\n", thread_current_id());
		return ETIMEDOUT;
	}
	m->lock();
	return 0;
}

int pthread_cond_signal(pthread_cond_t *p_cond) {
	// notify only one blocked thread
	ModelExecution *execution = model->get_execution();
	if ( !execution->getCondMap()->contains(p_cond) )
		pthread_cond_init(p_cond, NULL);

	cdsc::snapcondition_variable *v = execution->getCondMap()->get(p_cond);

	v->notify_one();
	return 0;
}

int pthread_cond_broadcast(pthread_cond_t *p_cond) {
	// notify all blocked threads
	ModelExecution *execution = model->get_execution();
	if ( !execution->getCondMap()->contains(p_cond) )
		pthread_cond_init(p_cond, NULL);

	cdsc::snapcondition_variable *v = execution->getCondMap()->get(p_cond);

	v->notify_all();
	return 0;
}

int pthread_cond_destroy(pthread_cond_t *p_cond) {
	ModelExecution *execution = model->get_execution();

	if (execution->getCondMap()->contains(p_cond)) {
		cdsc::snapcondition_variable *v = execution->getCondMap()->get(p_cond);
		delete v;
		execution->getCondMap()->remove(p_cond);
	}
	return 0;
}

/* https://github.com/lattera/glibc/blob/master/nptl/pthread_getattr_np.c */
int pthread_getattr_np(pthread_t t, pthread_attr_t *attr)
{
	ModelExecution *execution = model->get_execution();
	Thread *th = execution->get_pthread(t);

	struct pthread_attr *iattr = (struct pthread_attr *) attr;

	/* The sizes are subject to alignment.  */
	if (th != NULL) {
#if _STACK_GROWS_DOWN
		ASSERT(false);
#else
		iattr->stackaddr = (char *) th->get_stack_addr();
#endif

	} else {
		ASSERT(false);
	}

	return 0;
}

int pthread_setname_np(pthread_t t, const char *name)
{
	ModelExecution *execution = model->get_execution();
	Thread *th = execution->get_pthread(t);

	if (th != NULL)
		return 0;

	return 1;
}
