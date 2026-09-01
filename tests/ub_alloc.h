/* Copyright (c) 2026 UniBlake Developers */
/* Aligned allocation for the test and bench harnesses.
 *
 * C11 aligned_alloc is not universally available: MinGW's UCRT omits it and
 * supplies _aligned_malloc, whose allocations must be released by
 * _aligned_free rather than free. POSIX.1-2001 systems predating C11 have
 * posix_memalign. This selects among the three and pairs each with the
 * correct deallocator.
 *
 * Harness-only. The library never allocates, so nothing in include/ or src/
 * needs this. */
#ifndef UB_ALLOC_H
#define UB_ALLOC_H

#include <stdlib.h>

#if defined(_WIN32)
#include <malloc.h>
static void *ub_aligned_alloc(size_t align, size_t size) {
  return _aligned_malloc(size, align);   /* note: size and align are swapped */
}
static void ub_aligned_free(void *p) { _aligned_free(p); }

#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
static void *ub_aligned_alloc(size_t align, size_t size) {
  return aligned_alloc(align, size);
}
static void ub_aligned_free(void *p) { free(p); }

#else
static void *ub_aligned_alloc(size_t align, size_t size) {
  void *p = 0;
  return posix_memalign(&p, align, size) == 0 ? p : 0;
}
static void ub_aligned_free(void *p) { free(p); }
#endif

#endif /* UB_ALLOC_H */
