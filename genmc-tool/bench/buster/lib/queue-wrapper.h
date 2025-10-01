/* Selector for the different queues */

#ifndef __QUEUE_WRAPPER_H__
#define __QUEUE_WRAPPER_H__

#ifdef MS_HP
# include "./ms-queue-hp/queue.c"
#elif DGLM_HP
# include "./dglm-queue-hp/queue.c"
#elif MS_OPT_HP
# include "./ms-queue-optimistic-hp/queue.c"
#elif MS_OPT_TAG
# include "./ms-queue-optimistic/queue.c"
#elif OPT_LF
# include "./optimized-lf-queue/queue.c"
#elif TWO_LOCK
# include "./two-lock-queue/queue.c"
#else
# error "No known queue specified!"
#endif

#endif /* __QUEUE_WRAPPER_H__ */
