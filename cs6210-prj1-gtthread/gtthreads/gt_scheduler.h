/*
 * gt_scheduler.h
 *
 */

#ifndef GT_SCHEDULER_H_
#define GT_SCHEDULER_H_

#include "gt_spinlock.h"
#include "gt_thread.h"

/* can't forward declare a typedef... */
struct uthread;
struct kthread;

/* raise this signal when you want to schedule a new uthread */
#define SIGSCHED SIGVTALRM

/* Defines the interface all scheduler objects must follow */

/* Scheduler-specific data. Schedulers can do with this what they wish. destroy()
 * is called at exit to do any cleanup */
typedef struct sched_data {
	void *buf;
	void (*destroy)(void *buf);
} sched_data_t;

/* Functions schedulers must implement:
 */

/* called after initialization of every new kthread */
typedef void (*kthread_init_t)(struct kthread *);

/* called after the creation of every new uthread. Should somehow assign the uthread to a kthread  and returns it */
typedef struct kthread *(*uthread_init_t)(struct uthread *);

/* preements current uthread and returns it. If current uthread is DONE or NULL, returns NULL */
typedef struct uthread *(*preempt_current_uthread_t)(struct kthread *);

/* chooses the next uthread to run */
typedef struct uthread *(*pick_next_uthread_t)(struct kthread *);

/* takes care of last minute details before a the kthread's "current" uthread is resumed (e.g., setting any timers */
typedef void (*resume_uthread_t)(struct kthread *);

typedef struct scheduler {
	kthread_init_t kthread_init;
	uthread_init_t uthread_init;
	preempt_current_uthread_t preempt_current_uthread;
	pick_next_uthread_t pick_next_uthread;
	resume_uthread_t resume_uthread;

	gt_spinlock_t lock;
	sched_data_t data;
} scheduler_t;

/* initializes the above data structure for the specific scheduler type */
void sched_type_scheduler_init(scheduler_type_t scheduler_type, int lwp_count);

/* not implemented by scheduler objects */
void scheduler_init(scheduler_t *scheduler, scheduler_type_t scheduler_type, int lwp_count);
void scheduler_destroy(scheduler_t *scheduler);
void schedule(void);
void scheduler_switch(scheduler_t *s, scheduler_type_t t, int lwp_count);

#endif /* GT_SCHEDULER_H_ */
