#ifndef __MODEL_ASSERT_H__
#define __MODEL_ASSERT_H__

#if __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

#include <stdio.h>
#include <stdlib.h>

static inline void model_assert(bool expr, const char *file, int line) {
  if (!expr) {
    if (!file) file = "unknown";
    fprintf(stderr, "MODEL_ASSERT failed at %s:%d\n", file, line);
    fflush(stderr);
    abort();
  }
}

/* Enhanced helper to also print the failed expression text when available */
static inline void model_assert_expr(bool expr, const char *expr_str,
                                     const char *file, int line) {
  if (!expr) {
    if (!file) file = "unknown";
    if (!expr_str) expr_str = "<expr>";
    fprintf(stderr, "MODEL_ASSERT(%s) Abort. at %s:%d\n", expr_str, file, line);
    fflush(stderr);
    abort();
  }
}

#define MODEL_ASSERT(expr) model_assert_expr((expr), #expr, __FILE__, __LINE__)

#if __cplusplus
}
#endif

#endif /* __MODEL_ASSERT_H__ */
