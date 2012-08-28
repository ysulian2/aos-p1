/*
 * gt_thread.h
 */

#ifndef GT_THREAD_H_
#define GT_THREAD_H_

#include "gt_typedefs.h"

struct timeval;

/* Options for gtthread_app_init().
 */
/* choose a scheduler for your uthreads */
typedef enum scheduler_type {
	SCHEDULER_DEFAULT,
	SCHEDULER_PCS, /* priority co-scheduler */
	SCHEDULER_CFS /* completely fair scheduler */
} scheduler_type_t;

typedef struct gtthread_options {
	scheduler_type_t scheduler_type;
	int lwp_count; /* the number of lwps. If less than 1, defaults to the
	 number of cpus on the system */
} gtthread_options_t;

/* initializes `options` to their defaults */
void gtthread_options_init(gtthread_options_t *options);

/* Starts up the lightweight processes. If `options` is NULL, it will be set to
 * the defaults */
void gtthread_app_init(gtthread_options_t *options);

/* Attribute for uthreads. Treat as an opaque object; use the provided functions
 * for manipulation, a la pthread_attr* functions */
typedef struct uthread_attr uthread_attr_t;

/* Create and destroy attributes */
uthread_attr_t *uthread_attr_create(void);
void uthread_attr_destroy(uthread_attr_t *attr);
/* Initializes attribute to the defaults */
void uthread_attr_init(uthread_attr_t *attr);

/* Scheduling parameters. Either or both can be set to their defaults,
 * UTHREAD_ATTR_PRIORITY_DEFAULT and UTHREAD_ATTR_GROUP_DEFAULT, respectively */
struct uthread_sched_param {
	int priority;
	uthread_gid group_id;
};
void uthread_attr_setschedparam(uthread_attr_t *attr,
                                const struct uthread_sched_param *param);
void uthread_attr_getschedparam(uthread_attr_t *attr,
                                struct uthread_sched_param *param);

/* Puts the total execution time for the uthread in `tv`, which does not include
 * the time spent waiting to be scheduled */
void uthread_attr_getcputime(uthread_attr_t *attr, struct timeval *tv);

/* creates the uthread with attribute `attr`, and starts executing
 * start_routine(arg). The newly created thread will have its tid returned in
 * `tid`. If `attr` is NULL, it will be initialized to the defaults. Returns -1
 * on error */
int uthread_create(uthread_tid *tid, uthread_attr_t *attr,
                   int(*start_routine)(void *), void *arg);

/* Voluntarily relinquishes the cpu for the currently executing uthread, and
 * causes the scheduling of the next uthread. */
void gt_yield();

/* blocks until all uthreads are done executing */
extern void gtthread_app_exit();

#endif /* GT_THREAD_H_ */
