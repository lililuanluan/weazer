/* Selector for the different stacks */

#ifndef __STACK_WRAPPER_H__
#define __STACK_WRAPPER_H__

#ifdef TREIBER_HP
# include "./treiber-stack-hp/stack.c"
#else
# error "No known stack specified!"
#endif

#endif /* __STACK_WRAPPER_H__ */
