/*
 * gt_pfs.h
 *
 * Implements the Completely Fair Scheduler, following the generic scheduling
 * interface
 *
 */

#ifndef GT_PFS_H_
#define GT_PFS_H_

#include "rb_tree/red_black_tree.h"

struct scheduler;
struct kthread;
struct uthread;

void pfs_init(struct scheduler *scheduler, int lwp_count);

#endif /* GT_PFS_H_ */
