/*
 * gt_scheduler.c
 *
 */

#include <unistd.h>
#include <ucontext.h>

#include "gt_scheduler.h"
#include "gt_kthread.h"
#include "gt_uthread.h"
#include "gt_uthread.h"
#include "gt_spinlock.h"
#include "gt_common.h"
#include "gt_signal.h"

scheduler_t scheduler;

/*
 * Preempts, schedules, and dispatches a new uthread
 */
void schedule(void)
{
	kthread_t *k_ctx = kthread_current_kthread();
	checkpoint("k%d: Scheduling", k_ctx->cpuid);
	scheduler.preempt_current_uthread(k_ctx);
	uthread_t *next_uthread = scheduler.pick_next_uthread(k_ctx);
	if (next_uthread == NULL) {
		/* we're done with all our uhreads */
		checkpoint("k%d: NULL next_uthread", k_ctx->cpuid);
		checkpoint("k%d: Setting state to DONE, wait for more uthreads",
			   k_ctx->cpuid);
		k_ctx->state = KTHREAD_DONE;
		kthread_wait_for_uthread(k_ctx);
		return;
	}

	checkpoint("k%d: u%d: Resuming uthread", k_ctx->cpuid,
		   next_uthread->tid);
	k_ctx->current_uthread = next_uthread;
	scheduler.resume_uthread(k_ctx); // possibly sets timer
	setcontext(&next_uthread->context);
}

void scheduler_init(scheduler_t *scheduler, scheduler_type_t sched_type,
                    int lwp_count)
{
	gt_spinlock_init(&scheduler->lock);
	scheduler_switch(scheduler, sched_type, lwp_count);
	return;
}

void scheduler_destroy(scheduler_t *scheduler) {
	scheduler->data.destroy(scheduler->data.buf);
	return;
}
