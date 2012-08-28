/*
 * gt_kthread.c
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <sched.h>
#include <string.h>
#include <assert.h>
#include <ucontext.h>

#include "gt_kthread.h"
#include "gt_uthread.h"
#include "gt_common.h"
#include "gt_scheduler.h"
#include "gt_signal.h"

#define KTHREAD_DEFAULT_SSIZE (256 * 1024)
#define KTHREAD_MAX_COUNT 16

extern scheduler_t scheduler;
extern int kthread_count;
extern gt_spinlock_t kthread_count_lock;

kthread_t *_kthread_cpu_map[KTHREAD_MAX_COUNT];
gt_spinlock_t cpu_map_lock = GT_SPINLOCK_INITIALIZER;

/* Toggled on when a child kthread is created, cloned, and ready to be
 * scheduled. The parent should toggle it off before creating the next
 * kthread. */
volatile sig_atomic_t kthread_is_ready = 0;

void kthread_ready_handler(int signo)
{
	kthread_is_ready = 1;
}

/* returns the currently running kthread */
kthread_t *kthread_current_kthread()
{
	gt_spin_lock(&cpu_map_lock);
	kthread_t *k_ctx = _kthread_cpu_map[kthread_apic_id()];
	gt_spin_unlock(&cpu_map_lock);
	assert(k_ctx != NULL);
	return k_ctx;
}

int can_exit = 0;

/* returns 1 if a kthread is schedulable, 0 otherwise */
int kthread_is_schedulable(kthread_t *k_ctx) {
	return (!(k_ctx == NULL ||
		  k_ctx->state == KTHREAD_INIT ));
}

static void kthread_exit(kthread_t *k_ctx)
{
	checkpoint("k%d: exiting", k_ctx->cpuid);
	gt_spin_lock(&kthread_count_lock);
	kthread_count--;
	gt_spin_unlock(&kthread_count_lock);
	free(k_ctx);
	exit(EXIT_SUCCESS);
}

/* signal handler for SIGSCHED. */
void kthread_sched_handler(int signo)
{
	checkpoint("%s", "***Entering signal handler***");
	kthread_t *k_ctx = kthread_current_kthread();
	k_ctx->state = KTHREAD_RUNNING;
	if (k_ctx->current_uthread) {
		uthread_attr_set_elapsed_cpu_time(k_ctx->current_uthread->attr);

		if (getcontext(&k_ctx->sched_ctx) == -1)
			fail_perror("getcontext");
		makecontext(&k_ctx->sched_ctx, schedule, 0, 0);
		checkpoint("%s", "Swapping to schedule context");
		if (swapcontext(&k_ctx->current_uthread->context,
				&k_ctx->sched_ctx) == -1)
			fail_perror("swapcontext");
	} else {
		schedule();
	}

	k_ctx = kthread_current_kthread(); // not sure if i trust my stack; call again
	gettimeofday(&k_ctx->current_uthread->attr->timeslice_start, NULL);
	checkpoint("k%d: exiting handler", k_ctx->cpuid);
}

/* blocks until there is a uthread to schedule */
void kthread_wait_for_uthread(kthread_t *k_ctx)
{
	struct timespec interval = {
		.tv_sec = 1,
		.tv_nsec = 0
	};

	while (!(k_ctx->state == KTHREAD_DONE && can_exit)) {
		checkpoint("State is %d and can_exit is %d",
		           k_ctx->state, can_exit);
		checkpoint("k%d: kthread (%d) waiting for first uthread",
		           k_ctx->cpuid, k_ctx->tid);
		/* Must use nanosleep so as not to interfere with SIGALRM
		 * and other signals */
		nanosleep(&interval, NULL);
	}
	checkpoint("k%d: exiting wait for uthread", k_ctx->cpuid);
	kthread_exit(k_ctx);
}

static void kthread_init_context(kthread_t *kthread)
{
	kthread->sched_ctx.uc_stack.ss_sp = kthread->sched_ctx_stack;
	kthread->sched_ctx.uc_stack.ss_size = sizeof(kthread->sched_ctx_stack);
	kthread->sched_ctx.uc_stack.ss_flags = 0;
	kthread->sched_ctx.uc_link = NULL;
}

/* main function is to set the cpu affinity */
static void kthread_set_cpu_affinity(kthread_t *k_ctx)
{
	cpu_set_t cpu_affinity_mask;
	CPU_ZERO(&cpu_affinity_mask);
	CPU_SET(k_ctx->cpuid, &cpu_affinity_mask);
	sched_setaffinity(k_ctx->tid, sizeof(cpu_affinity_mask),
	                  &cpu_affinity_mask);

	sched_yield(); /* gets us on our target cpu */

	k_ctx->cpu_apic_id = kthread_apic_id();
	_kthread_cpu_map[k_ctx->cpu_apic_id] = k_ctx;
	return;
}

static int kthread_start(void *arg)
{
	kthread_t *k_ctx = arg;
	k_ctx->pid = getpid();
	k_ctx->tid = k_ctx->pid;
	kthread_set_cpu_affinity(k_ctx);
	kthread_init_context(k_ctx);
	scheduler.kthread_init(k_ctx);
	k_ctx->state = KTHREAD_RUNNABLE;
	sig_install_handler_and_unblock(SIGSCHED, &kthread_sched_handler);
	kill(getppid(), SIGUSR1); // signals that we are ready for scheduling
	kthread_wait_for_uthread(k_ctx);
	return 0;
}

/* kthread creation. Returns a pointer to the kthread_t if successful, NULL
 * otherwise */
kthread_t *kthread_create(pid_t *tid, int lwp)
{
	/* Create the new thread's stack */
	size_t stacksize = KTHREAD_DEFAULT_SSIZE;
	char *stack = ecalloc(stacksize);
	stack += stacksize;  // grows down

	/* set up the context */
	kthread_t *k_ctx = ecalloc(sizeof(*k_ctx));
	k_ctx->cpuid = lwp;
	k_ctx->state = KTHREAD_INIT;

	/* Install the handler to be notified when this child kthread will
	 * ready to be scheduled */
	sig_install_handler_and_unblock(SIGUSR1, &kthread_ready_handler);

	int flags = CLONE_VM | CLONE_FS | CLONE_FILES | SIGCHLD;
	*tid = clone(kthread_start, (void *) stack, flags, (void *) k_ctx);
	if (*tid < 0) {
		perror("clone");
		free(k_ctx);
		return NULL;
	}

	while (!kthread_is_ready)
		sleep(1);
	kthread_is_ready = 0;
	return k_ctx;
}
