#ifndef __HELPER_DEFS_H__
#define __HELPER_DEFS_H__

#ifdef DISABLE_HELPER
# define DISABLE_ANNOTATIONS
# define DISABLE_HPS
# define DISABLE_HELPING
# define DISABLE_CONFIRMATION
# define DISABLE_LOCAL
# define DISABLE_FREEZING
# define DISABLE_OPTIONAL
#endif /* DISABLE_HELPER */

#ifdef DISABLE_HPS
# undef __VERIFIER_hp_alloc
# undef __VERIFIER_hp_protect
# undef __VERIFIER_hp_clear
# undef __VERIFIER_hp_free
# undef __VERIFIER_hp_retire

# ifdef FAST_HP_ENC
#  define __VERIFIER_hp_alloc() NULL
#  define __VERIFIER_hp_protect(hp, p)					\
   ({									\
	   void *_p_ = __atomic_load_n((void **) p, __ATOMIC_ACQUIRE);	\
	   _p_;								\
   })
#  define __VERIFIER_hp_clear(hp)	do {} while (0)
#  define __VERIFIER_hp_free(hp)	do {} while (0)
#  define __VERIFIER_hp_retire(p)	do {} while (0)
# else /* !FAST_HP_ENC */
#  define __VERIFIER_hp_alloc() NULL
#  define __VERIFIER_hp_protect(hp, p)					\
   ({									\
	void *__ret, *_p_;						\
   	do {								\
		_p_ = __atomic_load_n((void **) p, __ATOMIC_ACQUIRE);	\
		hp->__dummy = _p_;					\
		__ret = __atomic_load_n((void **) p, __ATOMIC_ACQUIRE);	\
   	} while (_p_ != __ret);						\
	__ret;								\
   })
#  define __VERIFIER_hp_clear(hp)	do {} while (0)
#  define __VERIFIER_hp_free(hp)	do {} while (0)
#  define __VERIFIER_hp_retire(p)	do {} while (0)
# endif /* FAST_HP_ENC */
#endif /* DISABLE_HPS */

#ifdef DISABLE_ANNOTATIONS
# undef __VERIFIER_annotate_read
# undef __VERIFIER_annotate_write
# undef __VERIFIER_annotate_CAS

# define __VERIFIER_annotate_read(a)  do {} while (0)
# define __VERIFIER_annotate_write(a) do {} while (0)
# define __VERIFIER_annotate_CAS(a)   do {} while (0)
#endif /* DISABLE_ANNOTATIONS */

#ifdef DISABLE_HELPING
# undef __VERIFIER_helped_CAS
# undef __VERIFIER_helping_CAS

# define __VERIFIER_helped_CAS(c) ({ c; })
# define __VERIFIER_helping_CAS(c) ({ c; })
#endif /* DISABLE_HELPING */

#ifdef DISABLE_LOCAL
# undef __VERIFIER_local_write

# define __VERIFIER_local_write(w) ({ w; })
#endif /* DISABLE_LOCAL */

#ifdef DISABLE_FREEZING
# undef __VERIFIER_final_write
# undef __VERIFIER_final_CAS

# define __VERIFIER_final_write(c) ({ c; })
# define __VERIFIER_final_CAS(c) ({ c; })
#endif /* DISABLE_FREEZING */

#ifdef DISABLE_CONFIRMATION
# undef __VERIFIER_speculative_read
# undef __VERIFIER_confirming_read
# undef __VERIFIER_confirming_CAS

# define __VERIFIER_speculative_read(c) ({ c; })
# define __VERIFIER_confirming_read(c) ({ c; })
# define __VERIFIER_confirming_CAS(c) ({ c; })
#endif /* DISABLE_CONFIRMATION */

#ifdef DISABLE_OPTIONAL
# undef __VERIFIER_optional

# define __VERIFIER_optional(c) ({ c; })
#endif /* DISABLE_OPTIONAL */

#ifdef MAKE_ACCESSES_SC
# define relaxed memory_order_seq_cst
# define release memory_order_seq_cst
# define acquire memory_order_seq_cst
# define acqrel  memory_order_seq_cst
# define seqcst  memory_order_seq_cst
#else
# define relaxed memory_order_relaxed
# define release memory_order_release
# define acquire memory_order_acquire
# define acqrel  memory_order_acq_rel
# define seqcst  memory_order_seq_cst
#endif

#endif /* __HELPER_DEFS_H__ */
