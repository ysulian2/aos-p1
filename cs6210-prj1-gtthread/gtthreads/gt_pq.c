/*
 * gt_pq.c
 *
 */

#include <stdio.h>
#include <assert.h>

#include "gt_pq.h"
#include "gt_thread.h"
#include "gt_bitops.h"
#include "gt_tailq.h"
#include "gt_scheduler_pcs.h"
#include "gt_common.h"

/* set valid priorities and groups */
/* returns the priority of the given uthread, if it is a valid value. Else
 * the appropriate min or max priority */
int pq_get_priority(uthread_t *uthread) {
	int priority = uthread->attr->priority;
	if (priority == UTHREAD_ATTR_PRIORITY_DEFAULT) {
		priority = PQ_DEFAULT_UTHREAD_PRIORITY;
	} else if (priority < PQ_MIN_UTHREAD_PRIORITY) {
		priority = PQ_MIN_UTHREAD_PRIORITY;
	} else if (priority > PQ_MAX_UTHREAD_PRIORITY) {
		priority = PQ_MAX_UTHREAD_PRIORITY;
	}
	return priority;
}

/* returns the group id of the given uthread, if it is a valid value. Else
 * the appropriate min or max group id */
int pq_get_group_id(uthread_t *uthread) {
	int group_id = uthread->attr->group_id;
	int max_group_id = PQ_MIN_UTHREAD_GROUP + PQ_MAX_UTHREAD_GROUP_COUNT - 1;
	if (group_id == UTHREAD_ATTR_PRIORITY_DEFAULT) {
		group_id = PQ_DEFAULT_UTHREAD_GROUP;
	} else if (group_id < PQ_MIN_UTHREAD_GROUP) {
		group_id = PQ_MIN_UTHREAD_GROUP;
	} else if (group_id > max_group_id) {
		group_id = max_group_id;
	}
	return group_id;
}


/**********************************************************************/
/* runqueue operations */
static void __add_to_runqueue(runqueue_t *runq, pcs_uthread_t *u_elm);
static void __rem_from_runqueue(runqueue_t *runq, pcs_uthread_t *u_elm);

/**********************************************************************/
/* runqueue operations */
static inline void __add_to_runqueue(runqueue_t *runq, pcs_uthread_t *u_elem)
{
	unsigned int uprio, ugroup;
	uthread_head_t *uhead;

	/* Find a position in the runq based on priority and group.
	 * Update the masks. */
	uprio = u_elem->priority;
	ugroup = u_elem->group_id;

	/* Insert at the tail */
	uhead = &runq->prio_array[uprio].group[ugroup];
	TAILQ_INSERT_TAIL(uhead, u_elem, uthread_runq);

	/* Update information */
	if (!IS_BIT_SET(runq->prio_array[uprio].group_mask, ugroup))
		SET_BIT(runq->prio_array[uprio].group_mask, ugroup);

	runq->uthread_tot++;

	runq->uthread_prio_tot[uprio]++;
	if (!IS_BIT_SET(runq->uthread_mask, uprio))
		SET_BIT(runq->uthread_mask, uprio);

	runq->uthread_group_tot[ugroup]++;
	if (!IS_BIT_SET(runq->uthread_group_mask[ugroup], uprio))
		SET_BIT(runq->uthread_group_mask[ugroup], uprio);

	return;
}

static inline void __rem_from_runqueue(runqueue_t *runq,
                                       pcs_uthread_t *u_elem)
{
	unsigned int uprio, ugroup;
	uthread_head_t *uhead;

	/* Find a position in the runq based on priority and group.
	 * Update the masks. */
	uprio = u_elem->priority;
	ugroup = u_elem->group_id;


	/* Insert at the tail */
	uhead = &runq->prio_array[uprio].group[ugroup];
	TAILQ_REMOVE(uhead, u_elem, uthread_runq);

	/* Update information */
	if (TAILQ_EMPTY(uhead))
		RESET_BIT(runq->prio_array[uprio].group_mask, ugroup);
	runq->uthread_tot--;

	if (!(--(runq->uthread_prio_tot[uprio])))
		RESET_BIT(runq->uthread_mask, uprio);

	if (!(--(runq->uthread_group_tot[ugroup]))) {
		assert(TAILQ_EMPTY(uhead));
		RESET_BIT(runq->uthread_group_mask[ugroup], uprio);
	}

	return;
}

/**********************************************************************/
/* Exported runqueue operations */
extern void init_runqueue(runqueue_t *runq)
{
	uthread_head_t *uhead;
	int i, j;
	/* Everything else is global, so already initialized to 0(correct init value) */
	for (i = 0; i < PQ_MAX_UTHREAD_PRIORITY; i++) {
		for (j = 0; j < PQ_MAX_UTHREAD_GROUP_COUNT; j++) {
			uhead = &((runq)->prio_array[i].group[j]);
			TAILQ_INIT(uhead);
		}
	}
	return;
}

extern void add_to_runqueue(runqueue_t *runq, gt_spinlock_t *runq_lock,
                            pcs_uthread_t *u_elem)
{
	if (runq_lock) {
		gt_spin_lock(runq_lock);
		runq_lock->holder = 0x02;
	}
	__add_to_runqueue(runq, u_elem);
	if (runq_lock)
		gt_spin_unlock(runq_lock);
	return;
}

extern void rem_from_runqueue(runqueue_t *runq, gt_spinlock_t *runq_lock,
                              pcs_uthread_t *u_elem)
{
	if (runq_lock) {
		gt_spin_lock(runq_lock);
		runq_lock->holder = 0x03;
	}
	__rem_from_runqueue(runq, u_elem);
	if (runq_lock)
		gt_spin_unlock(runq_lock);
	return;
}

extern void switch_runqueue(runqueue_t *from_runq,
                            gt_spinlock_t *from_runqlock,
                            runqueue_t *to_runq,
                            gt_spinlock_t *to_runqlock,
                            pcs_uthread_t *u_elem)
{
	rem_from_runqueue(from_runq, from_runqlock, u_elem);
	add_to_runqueue(to_runq, to_runqlock, u_elem);
	return;
}

/**********************************************************************/

extern void kthread_init_runqueue(kthread_runqueue_t *kthread_runq)
{
	kthread_runq->active_runq = &(kthread_runq->runqueues[0]);
	kthread_runq->expires_runq = &(kthread_runq->runqueues[1]);

	gt_spinlock_init(&(kthread_runq->kthread_runqlock));
	init_runqueue(kthread_runq->active_runq);
	init_runqueue(kthread_runq->expires_runq);

	TAILQ_INIT(&(kthread_runq->zombie_uthreads));
	return;
}
