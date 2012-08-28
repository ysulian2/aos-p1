/*
 * gt_pcs.h
 *
 * Implements the priority co-scheduler, following the generic scheduling interface
 *
 */

#ifndef GT_PCS_H_
#define GT_PCS_H_

#include "gt_tailq.h"
#include "gt_pq.h"

struct scheduler;
struct kthread;
struct uthread;

/* data maintained internally for each kthread */
typedef struct pcs_kthread {
	struct kthread *k_ctx;
	kthread_runqueue_t k_runqueue;
} pcs_kthread_t;

/* data maintained internally for each uthread */
typedef struct pcs_uthread {
	struct uthread *uthread;
	int priority;
	int group_id;
	TAILQ_ENTRY(pcs_uthread) uthread_runq;
} pcs_uthread_t;

void pcs_init(struct scheduler *scheduler, int lwp_count);

#endif /* GT_PCS_H_ */
