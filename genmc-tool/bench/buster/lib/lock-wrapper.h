/* Selector for different types of locks */

#ifndef __LOCK_WRAPPER_H__
#define __LOCK_WRAPPER_H__

#ifdef TTAS
# include "./ttaslock/lock.c"
#else
# error "No known stack specified!"
#endif

#endif /* __LOCK_WRAPPER_H__ */
