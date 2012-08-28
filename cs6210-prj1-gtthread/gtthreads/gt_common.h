/*
 * gt_common.h
 *
 * Useful macros, etc
 */

#ifndef GT_COMMON_H_
#define GT_COMMON_H_

#include <stdio.h>
#include <stdlib.h>

#include "gt_spinlock.h"

#define fail_perror(msg) \
        do { perror(msg); exit(EXIT_FAILURE); } while (0)

#define fail(msg) \
        do { fprintf(stderr, "Error: %s\n", msg); exit(EXIT_FAILURE); } while (0)

/* check pointing: compiler always checks that the code is valid, but will optimize it out if
 * DEBUG not defined. Requires C99 for the varaible args */
#ifdef DEBUG
#define DEBUG_PRINT 1
#else
#define DEBUG_PRINT 0
#endif
#define checkpoint(fmt, ...) \
        do { \
		if (DEBUG_PRINT) { \
			fprintf(stderr, \
				"%-25s%5d:%35s(): " fmt "\n", __FILE__ ":", \
				__LINE__, __func__, __VA_ARGS__); \
			fflush(stderr); \
		} \
	} while (0)


/* thread-safe malloc with failure */
extern gt_spinlock_t MALLOC_LOCK;
static inline void *emalloc(size_t size)
{
	void *__ptr;
	gt_spin_lock(&MALLOC_LOCK);
	__ptr = malloc(size);
	gt_spin_unlock(&MALLOC_LOCK);
	if (!__ptr)
		fail("malloc");
	return(__ptr);
}

/* Zeroes out allocated bytes */
static inline void *ecalloc(size_t size)
{
	void *__ptr;
	gt_spin_lock(&MALLOC_LOCK);
	__ptr = calloc(1, size);
	gt_spin_unlock(&MALLOC_LOCK);
	if (!__ptr)
		fail("calloc");
	return(__ptr);
}


#endif /* GT_COMMON_H_ */
