/*
 * gt_uthread.c
 *
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>
#include <assert.h>
#include <sched.h>
#include <string.h>
#include <ucontext.h>

#include "gt_uthread.h"
#include "gt_kthread.h"
#include "gt_thread.h"
#include "gt_spinlock.h"
#include "gt_common.h"
#include "gt_scheduler.h"
#include "gt_signal.h"

#define UTHREAD_DEFAULT_SSIZE (16 * 1024 )

/* global scheduler */
extern scheduler_t scheduler;

gt_spinlock_t uthread_count_lock = GT_SPINLOCK_INITIALIZER;
int uthread_count = 0;

gt_spinlock_t uthread_init_lock = GT_SPINLOCK_INITIALIZER;

/* Serves as the launching off point and landing point for the user's uthread
 * execution.
 *
 * Does the initial setjmp so, on the first schedule, the user's
 * function is called (afterwards, execution is resumed by returning from signal
 * handlers). Also, uthreads end up here when the finish the user's function;
 * so this function launches back into the schedule() to schedule the next
 * available uthread.
 */
void uthread_context_func(int signo)
{
	kthread_t *kthread = kthread_current_kthread();
	uthread_t *uthread = kthread->current_uthread;
	checkpoint("k%d: u%d: uthread_context_func .....",
	           kthread->cpuid, uthread->tid);

	/* Execute the new_uthread task */
	while(gettimeofday(&uthread->attr->timeslice_start, NULL));
	checkpoint("u%d: Start time: %ld.%06ld s", uthread->tid,
	           uthread->attr->timeslice_start.tv_sec,
	           uthread->attr->timeslice_start.tv_usec);
	uthread->state = UTHREAD_RUNNING;
	uthread->start_routine(uthread->arg);
	uthread->state = UTHREAD_DONE;
	checkpoint("u%d: getting final elapsed time", uthread->tid);
	uthread_attr_set_elapsed_cpu_time(uthread->attr);

	checkpoint("u%d: task ended normally", uthread->tid);

	/* schedule the next thread, if there is one */
	schedule();

	checkpoint("%s", "exiting context fcn");
	return;
}

/* Initializes uthread. Sets up a stack through the use of SIGUSR2. Must be
 * called *after* the kthread it is going to be scheduled on has been
 * initialized  */
int uthread_makecontext(uthread_t *uthread)
{
	checkpoint("u%d: Initializing uthread...", uthread->tid);

	/* Initialize new context for uthread */
	ucontext_t *u_ctx = &uthread->context;
	if (getcontext(u_ctx) == -1)
		fail_perror("getcontext");
	u_ctx->uc_stack.ss_size = UTHREAD_DEFAULT_SSIZE;
	u_ctx->uc_stack.ss_sp = emalloc(u_ctx->uc_stack.ss_size);
	u_ctx->uc_stack.ss_flags = 0;
	u_ctx->uc_link = NULL;
	makecontext(u_ctx, (void (*)(void)) uthread_context_func, 1, 0);
	checkpoint("u%d: initialized", uthread->tid);
	return 0;
}


int uthread_create(uthread_tid *u_tid, uthread_attr_t *attr,
                   int(*start_routine)(void *), void *arg)
{
	if (attr == NULL) {
		attr = uthread_attr_create();
		uthread_attr_init(attr, -1);
	}

	checkpoint("%s", "Creating uthread...");
	uthread_t *new_uthread = ecalloc(sizeof(*new_uthread));
	new_uthread->state = UTHREAD_INIT;
	new_uthread->start_routine = start_routine;
	new_uthread->arg = arg;
	new_uthread->attr = attr;

	gt_spin_lock(&uthread_count_lock);
	new_uthread->tid = uthread_count++;
	gt_spin_unlock(&uthread_count_lock);
	*u_tid = new_uthread->tid;

	new_uthread->state = UTHREAD_RUNNABLE;
	kthread_t *kthread = scheduler.uthread_init(new_uthread);
	assert(kthread != NULL);
	uthread_makecontext(new_uthread);
	if (kthread->state != KTHREAD_RUNNING) {
		/* our kthread is waiting for its first uthread. wake it up */
		checkpoint("u%d: k%d: Sending SIGSCHED to kthread",
			   new_uthread->tid, kthread->cpuid);
		kill(kthread->tid, SIGSCHED);
	}
	checkpoint("u%d: created", new_uthread->tid);
	return 0;
}

/* Suspends the currently running uthread and causes the next to be scheduled */
void uthread_yield()
{
	checkpoint("k%d: u%d: Yielding",
	           (kthread_current_kthread())->cpuid,
	           (kthread_current_kthread())->current_uthread->tid);
	kill(getpid(), SIGSCHED);
}
