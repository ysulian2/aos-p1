/*
 * gt_cfs.h
 *
 * Implements the Completely Fair Scheduler, following the generic scheduling
 * interface
 *
 */

#ifndef GT_CFS_H_
#define GT_CFS_H_

#include "rb_tree/red_black_tree.h"

struct scheduler;
struct kthread;
struct uthread;

void cfs_init(struct scheduler *scheduler, int lwp_count);

#endif /* GT_CFS_H_ */
