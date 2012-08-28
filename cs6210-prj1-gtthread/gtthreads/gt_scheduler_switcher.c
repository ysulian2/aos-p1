/*
 * gt_scheduler_switcher.c
 *
 * Chooses schedulers. This file knows about all the different schedulers that
 * can be offered.
 *
 */

#include "gt_thread.h"
#include "gt_scheduler.h"

/* includes for each scheduler */
#include "gt_scheduler_pcs.h"
#include "gt_scheduler_cfs.h"


void scheduler_switch(scheduler_t *scheduler, scheduler_type_t sched_type,
                      int lwp_count)
{
	switch (sched_type) {
	case SCHEDULER_DEFAULT:
	case SCHEDULER_PCS:
		pcs_init(scheduler, lwp_count);
		break;
	case SCHEDULER_CFS:
		cfs_init(scheduler, lwp_count);
		break;
	}
}
