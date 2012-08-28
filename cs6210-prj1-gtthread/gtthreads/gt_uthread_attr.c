/*
 * gt_uthread_attr.c
 *
 */

#include <sys/time.h>
#include <assert.h>

#include "gt_thread.h"
#include "gt_uthread.h"
#include "gt_kthread.h"
#include "gt_common.h"


void uthread_attr_getcputime(uthread_attr_t *attr, struct timeval *tv)
{
	tv->tv_sec = attr->execution_time.tv_sec;
	tv->tv_usec = attr->execution_time.tv_usec;
}

void uthread_attr_getschedparam(uthread_attr_t *attr,
                                struct uthread_sched_param *param)
{
	param->priority = attr->priority;
	param->group_id = attr->group_id;
}

void uthread_attr_setschedparam(uthread_attr_t *attr,
                                const struct uthread_sched_param *param)
{
	attr->priority = param->priority;
	attr->group_id = param->group_id;
}

void uthread_attr_init(uthread_attr_t *attr)
{
	attr->priority = UTHREAD_ATTR_PRIORITY_DEFAULT;
	attr->group_id = UTHREAD_ATTR_GROUP_DEFAULT;
	attr->execution_time.tv_sec = 0;
	attr->execution_time.tv_usec = 0;
	attr->timeslice_start = attr->execution_time;
}

uthread_attr_t *uthread_attr_create()
{
	uthread_attr_t *attr = emalloc(sizeof(*attr));
	return attr;
}

void uthread_attr_destroy(uthread_attr_t *attr)
{
	free(attr);
}

/* performs "final - initial", puts result in `result`. returns 1 if answer is negative,
 * 0 otherwise. taken from gnu.org */
static int timeval_subtract(struct timeval *result, struct timeval *final,
                            struct timeval *initial)
{
	/* Perform the carry for the later subtraction by updating y. */
	if (final->tv_usec < initial->tv_usec) {
		int nsec = (initial->tv_usec - final->tv_usec) / 1000000 + 1;
		initial->tv_usec -= 1000000 * nsec;
		initial->tv_sec += nsec;
	}
	if (final->tv_usec - initial->tv_usec > 1000000) {
		int nsec = (final->tv_usec - initial->tv_usec) / 1000000;
		initial->tv_usec += 1000000 * nsec;
		initial->tv_sec -= nsec;
	}

	/* Compute the time remaining to wait. tv_usec is certainly positive. */
	result->tv_sec = final->tv_sec - initial->tv_sec;
	result->tv_usec = final->tv_usec - initial->tv_usec;

	/* Return 1 if result is negative. */
	return final->tv_sec < initial->tv_sec;
}

/* adds `a` and `b`, puts result in `result`. Assumes both `a` and `b` are positive */
static void timeval_add(struct timeval *result, struct timeval *a, struct timeval *b)
{
	result->tv_sec = a->tv_sec + b->tv_sec;
	result->tv_usec = a->tv_usec + b->tv_usec;
	while (result->tv_usec > 1000000) {
		result->tv_usec -= 1000000;
		result->tv_sec++;
	}
}

void uthread_attr_set_elapsed_cpu_time(uthread_attr_t *attr)
{
	checkpoint("k%d: u%d: entering uthread_attr_set_elapsed_cpu_time",
	           kthread_current_kthread()->cpuid,
	           kthread_current_kthread()->current_uthread->tid);
	struct timeval now, elapsed;
	while (gettimeofday(&now, NULL));
	timeval_subtract(&elapsed, &now, &attr->timeslice_start);
	timeval_add(&attr->execution_time,
	            &attr->execution_time, &elapsed);
	checkpoint("Execution time: %ld.%06ld s",
	           attr->execution_time.tv_sec,
	           attr->execution_time.tv_usec);
}
