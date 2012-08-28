/*
 * gt_pcs.c
 *
 * Implements the priority co-scheduler, following the generic scheduling
 * interface
 *
 */

#include <assert.h>
#include <sys/time.h>

#include "gt_scheduler.h"
#include "gt_scheduler_pcs.h"
#include "gt_uthread.h"
#include "gt_kthread.h"
#include "gt_common.h"
#include "gt_spinlock.h"
#include "gt_pq.h"
#include "gt_tailq.h"
#include "gt_bitops.h"

#define DEFAULT_UTHREAD_COUNT 32
#define MAX_UTHREAD_GROUPS 32

/* all threads get the same timeslice */
#define PCS_TIMESLICE_SEC 0
#define PCS_TIMESLICE_USEC 100000
const struct itimerval PCS_TIMERVAL = {
        .it_interval.tv_sec = 0,	// don't repeat
        .it_interval.tv_usec = 0,
        .it_value.tv_sec = PCS_TIMESLICE_SEC,
        .it_value.tv_usec = PCS_TIMESLICE_USEC
};

/* global singleton scheduler */
extern scheduler_t scheduler;

/* use this to cast the scheduler data void * to something we can use */
#define SCHED_DATA scheduler.data.buf

/* global pcs data */
typedef struct pcs_data {
	gt_spinlock_t lock;
	int pcs_kthread_count;
	pcs_kthread_t *pcs_kthreads;	// array, indexed by cpuid
	int pcs_uthread_count;
	pcs_uthread_t **pcs_uthreads;	// array of ptrs, indexed by uthread tid
	int pcs_uthread_array_length;	// can use to dynamically resize
	// target cpu for last uthread from group
	unsigned short last_ugroup_kthread[MAX_UTHREAD_GROUPS];
} pcs_data_t;

/* creates and inits the sched data */
void *pcs_create_sched_data(int lwp_count)
{
	pcs_data_t *pcs_data = ecalloc(sizeof(*pcs_data));
	gt_spinlock_init(&pcs_data->lock);

	/* array of kthread_t, index by kthread_t->cpuid */
	pcs_kthread_t *pcs_kthreads = ecalloc(lwp_count * sizeof(*pcs_kthreads));
	pcs_data->pcs_kthreads = pcs_kthreads;

	/* array of uthread_t, index by uthread_t->tid */
	pcs_data->pcs_uthread_array_length = DEFAULT_UTHREAD_COUNT;
	pcs_uthread_t **pcs_uthreads = ecalloc(
	        pcs_data->pcs_uthread_array_length * sizeof(*pcs_uthreads));
	pcs_data->pcs_uthreads = pcs_uthreads;
	pcs_data->pcs_uthread_count = 0;
	pcs_data->pcs_kthread_count = 0;

	return pcs_data;
}

/* returns the corresponding pcs_kthread_t for the given kthread_t */
static inline pcs_kthread_t *pcs_get_kthread(kthread_t *k_ctx)
{
	pcs_data_t *pcs_data = SCHED_DATA;
	return &pcs_data->pcs_kthreads[k_ctx->cpuid];
}

/* returns the corresponding pcs_uthread_t for the given uthread_t */
static inline pcs_uthread_t *pcs_get_uthread(uthread_t *uthread)
{
	pcs_data_t *pcs_data = SCHED_DATA;
	return pcs_data->pcs_uthreads[uthread->tid];
}

/* called at every kthread_create(). Assumes pcs_init() has already been
 * called */
void pcs_kthread_init(kthread_t *k_ctx)
{
	gt_spin_lock(&scheduler.lock);
	pcs_kthread_t *pcs_kthread = pcs_get_kthread(k_ctx);
	pcs_kthread->k_ctx = k_ctx;
	kthread_init_runqueue(&pcs_kthread->k_runqueue);
	pcs_data_t *pcs_data = SCHED_DATA;
	pcs_data->pcs_kthread_count++;
	gt_spin_unlock(&scheduler.lock);
}

/* finds and returns a suitable target kthread for the uthread */
static pcs_kthread_t *pcs_find_kthread_target(pcs_uthread_t *pcs_uthread,
                                              pcs_data_t *pcs_data)
{
	pcs_kthread_t *pcs_kthread;
	uthread_gid gid = pcs_uthread->group_id;
	checkpoint("u%d: PCS: Finding cpu target, group (%d)",
	           pcs_uthread->uthread->tid, gid);
	unsigned int target_cpu = pcs_data->last_ugroup_kthread[gid];
	/* we want to avoid putting uthreads of the same group on a cpu: round
	 * robin through the cpus */
	do {
		target_cpu = (target_cpu + 1) % pcs_data->pcs_kthread_count;
		pcs_kthread = &pcs_data->pcs_kthreads[target_cpu];
	} while (!kthread_is_schedulable(pcs_kthread->k_ctx));
	assert(pcs_kthread->k_ctx != NULL);
	pcs_data->last_ugroup_kthread[gid] = target_cpu;
	checkpoint("u%d: PCS: target cpu set to %d",
	           pcs_uthread->uthread->tid, target_cpu);
	return &pcs_data->pcs_kthreads[target_cpu];
}

/* makes more room for uthreads and returns the new array. `arr_length` is
 * updated new length */
static void *pcs_double_array_length(pcs_uthread_t **arr, int *arr_length)
{
	*arr_length *= 2;
	size_t blocksize = *arr_length * sizeof(arr[0]);
	void *p = realloc(arr, blocksize);
	if (!p)
		fail("realloc");
	return p;
}

/* allocates space for new pcs_uthread and returns it. Increases the size of the
 * array pcs_data->pcs_uthreads if necessary */
static pcs_uthread_t *pcs_pcs_uthread_create(uthread_t *uthread)
{
	pcs_data_t *pcs_data = SCHED_DATA;
	while (uthread->tid >= pcs_data->pcs_uthread_array_length) {
		checkpoint("u%d: PCS: we need more space for uthreads",
		           uthread->tid);
		pcs_data->pcs_uthreads = pcs_double_array_length(
		        pcs_data->pcs_uthreads,
		        &pcs_data->pcs_uthread_array_length);
	}
	pcs_uthread_t *pcs_uthread = emalloc(sizeof(*pcs_uthread));
	pcs_data->pcs_uthreads[uthread->tid] = pcs_uthread;
	pcs_data->pcs_uthread_count++;
	return pcs_uthread;
}

/* Called on uthread_create(). Must assign the new uthread to a kthread;
 * anything else is left up to the implementation. Can't assume the uthread
 * itself has been initialized in any way---it just has a tid
 */
kthread_t *pcs_uthread_init(uthread_t *uthread)
{
	checkpoint("u%d: PCS: init uthread", uthread->tid);

	pcs_data_t *pcs_data = SCHED_DATA;
	gt_spin_lock(&pcs_data->lock);

	pcs_uthread_t *pcs_uthread = pcs_pcs_uthread_create(uthread);
	pcs_uthread->uthread = uthread;
	pcs_uthread->priority = pq_get_priority(uthread);
	pcs_uthread->group_id = pq_get_group_id(uthread);

	pcs_kthread_t *pcs_kthread = pcs_find_kthread_target(pcs_uthread,
	                                                     pcs_data);
	add_to_runqueue(pcs_kthread->k_runqueue.active_runq,
	                &pcs_kthread->k_runqueue.kthread_runqlock,
	                pcs_uthread);
	gt_spin_unlock(&pcs_data->lock);
	assert(pcs_kthread != NULL);
	assert(pcs_kthread->k_ctx != NULL);
	return pcs_kthread->k_ctx;
}

static void pcs_insert_zombie(pcs_uthread_t *pcs_uthread,
                              kthread_runqueue_t *k_runq)
{
	uthread_head_t *kthread_zhead = &(k_runq->zombie_uthreads);
	gt_spin_lock(&(k_runq->kthread_runqlock));
	k_runq->kthread_runqlock.holder = 0x01;
	TAILQ_INSERT_TAIL(kthread_zhead, pcs_uthread, uthread_runq);
	gt_spin_unlock(&(k_runq->kthread_runqlock));
}

uthread_t *pcs_preemt_current_uthread(kthread_t *k_ctx)
{
	checkpoint("k%d: PCS: Preempting uthread", k_ctx->cpuid);
	uthread_t *cur_uthread = k_ctx->current_uthread;
	if (cur_uthread == NULL)
		return NULL;

	pcs_uthread_t *pcs_cur_uthread = pcs_get_uthread(cur_uthread);
	pcs_kthread_t *pcs_kthread = pcs_get_kthread(k_ctx);
	kthread_runqueue_t *k_runq = &pcs_kthread->k_runqueue;
	if (cur_uthread->state == UTHREAD_DONE) {
		checkpoint("u%d: PCS: uthread done", cur_uthread->tid);
		pcs_insert_zombie(pcs_cur_uthread, k_runq);
		return NULL;
	}

	checkpoint("u%d: PCS: uthread still runnable", cur_uthread->tid);
	cur_uthread->state = UTHREAD_RUNNABLE;
	add_to_runqueue(k_runq->expires_runq, &k_runq->kthread_runqlock,
	                pcs_cur_uthread);
	return cur_uthread;
}

/* [1] Tries to find the highest priority RUNNABLE uthread in active-runq.
 * [2] Found - Jump to [FOUND]
 * [3] Switches runqueues (active/expires)
 * [4] Repeat [1] through [2]
 * [NOT FOUND] Return NULL(no more jobs)
 * [FOUND] Remove uthread from pq and return it. */
uthread_t *pcs_pick_next_uthread(kthread_t *k_ctx)
{
	checkpoint("k%d: PCS: Picking next uthread", k_ctx->cpuid);
	pcs_kthread_t *pcs_kthread = pcs_get_kthread(k_ctx);
	kthread_runqueue_t *kthread_runq = &pcs_kthread->k_runqueue;

	gt_spin_lock(&(kthread_runq->kthread_runqlock));
	kthread_runq->kthread_runqlock.holder = 0x04;

	runqueue_t *runq = kthread_runq->active_runq;
	if (!(runq->uthread_mask)) { /* No jobs in active. switch runqueue */
		checkpoint("k%d: PCS: Switching runqueues", k_ctx->cpuid);
		assert(!runq->uthread_tot);
		kthread_runq->active_runq = kthread_runq->expires_runq;
		kthread_runq->expires_runq = runq;

		runq = kthread_runq->active_runq;
		if (!runq->uthread_mask) {
			assert(!runq->uthread_tot);
			gt_spin_unlock(&(kthread_runq->kthread_runqlock));
			return NULL;
		}
	}

	/* Find the highest priority bucket */
	unsigned int uprio, ugroup;
	uprio = LOWEST_BIT_SET(runq->uthread_mask);
	prio_struct_t *prioq = &(runq->prio_array[uprio]);

	assert(prioq->group_mask);
	ugroup = LOWEST_BIT_SET(prioq->group_mask);

	uthread_head_t *u_head = &(prioq->group[ugroup]);
	pcs_uthread_t *next_uthread = TAILQ_FIRST(u_head);
	rem_from_runqueue(runq, NULL, next_uthread);

	gt_spin_unlock(&(kthread_runq->kthread_runqlock));
	return next_uthread->uthread;
}

/* called right before current uthread resumes execution. should set a timer to ensure
 * that we get back to scheduling again.
 */
void pcs_resume_uthread(kthread_t *k_ctx)
{
	checkpoint("k%d: u%d: PCS: Setting timer",
	           k_ctx->cpuid, k_ctx->current_uthread->tid);
	k_ctx->current_uthread->state = UTHREAD_RUNNING;
	if (setitimer(ITIMER_VIRTUAL, &PCS_TIMERVAL, NULL)) // ignore old timer
		fail_perror("setitimer");
	return;
}

void pcs_destroy_sched_data(void *data)
{
	pcs_data_t *pcs_data = data;
	free(pcs_data->pcs_kthreads);
	free(pcs_data->pcs_uthreads);
	free(pcs_data);
}

void pcs_init(scheduler_t *scheduler, int lwp_count)
{
	checkpoint("%s", "PCS: initialization");
	scheduler->kthread_init = &pcs_kthread_init;
	scheduler->uthread_init = &pcs_uthread_init;
	scheduler->preempt_current_uthread = &pcs_preemt_current_uthread;
	scheduler->pick_next_uthread = &pcs_pick_next_uthread;
	scheduler->resume_uthread = &pcs_resume_uthread;

	scheduler->data.buf = pcs_create_sched_data(lwp_count);
	scheduler->data.destroy = &pcs_destroy_sched_data;
}
