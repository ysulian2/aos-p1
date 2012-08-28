/*
 * gt_thread.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sched.h>
#include <errno.h>

#include "gt_thread.h"
#include "gt_kthread.h"
#include "gt_uthread.h"
#include "gt_common.h"
#include "gt_spinlock.h"
#include "gt_scheduler.h"
#include "gt_signal.h"

/* global singleton scheduler */
extern scheduler_t scheduler;

/* for thread-safe malloc */
gt_spinlock_t MALLOC_LOCK = GT_SPINLOCK_INITIALIZER;

/* used for keeping track of when to quit. kthreads decrement on exit */
int kthread_count = 0;
gt_spinlock_t kthread_count_lock = GT_SPINLOCK_INITIALIZER;

/* Global used to signal to the kthreads that they can exit when ready */
extern int can_exit;

void gtthread_options_init(gtthread_options_t *options)
{
	options->scheduler_type = SCHEDULER_DEFAULT;
	options->lwp_count = 0;
}

static void _gtthread_app_init(gtthread_options_t *options);
void gtthread_app_init(gtthread_options_t *options)
{
	checkpoint("%s", "Entering app_init");
	/* Wrap the actual gtthread_app_init, initializing the options first
	 * if necessary */
	int free_opt = 0;
	if ((free_opt = (options == NULL))) {
		options = malloc(sizeof(*options));
		gtthread_options_init(options);
	}

	_gtthread_app_init(options);

	if (free_opt) {
		free(options);
	}
	checkpoint("%s", "Exiting app_init");
}

static void _gtthread_app_init(gtthread_options_t *options)
{
	/* Num of logical processors (cpus/cores) */
	if (options->lwp_count < 1) {
		options->lwp_count = (int) sysconf(_SC_NPROCESSORS_CONF);
	}
	scheduler_init(&scheduler, options->scheduler_type, options->lwp_count);

	pid_t k_tid;
	kthread_t *k_thread;
	for (int lwp = 0; lwp < options->lwp_count; lwp++) {
		if (!(k_thread = kthread_create(&k_tid, lwp)))
			fail_perror("kthread_create");
		gt_spin_lock(&kthread_count_lock);
		kthread_count++;
		gt_spin_unlock(&kthread_count_lock);
		checkpoint("k%d: created", lwp);
	}
}

void wakeup(int signo)
{
	;
}

void gtthread_app_exit()
{
	checkpoint("%s", "Entering app_exit");

	/* be sure to wake from sleep */
	sig_install_handler_and_unblock(SIGCHLD, wakeup);

	/* first we signal to the kthreads that it is OK to exit. At this point,
	 * they shouldn't need to wait for any more uthreads to be created */
	can_exit = 1;

	gt_spin_lock(&kthread_count_lock);
	while (kthread_count) {
		checkpoint("%s", "Waiting for children");
		gt_spin_unlock(&kthread_count_lock);
		sleep(1);
		gt_spin_lock(&kthread_count_lock);
	}
	gt_spin_unlock(&kthread_count_lock);

	scheduler_destroy(&scheduler);
	checkpoint("%s", "Exiting app");
}

void gt_yield()
{
	uthread_yield();
}
