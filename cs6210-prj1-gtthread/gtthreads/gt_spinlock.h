/*
 * gt_spinlock.h
 *
 */

#ifndef GT_SPINLOCK_H_
#define GT_SPINLOCK_H_

typedef struct gt_spinlock
{
	volatile int locked;
	unsigned int holder;
} gt_spinlock_t;

/* static initializer */
#define GT_SPINLOCK_INITIALIZER {0, 0}

extern int gt_spinlock_init(gt_spinlock_t* spinlock);
extern int gt_spin_lock(gt_spinlock_t* spinlock);
extern int gt_spin_unlock(gt_spinlock_t *spinlock);

#endif /* GT_SPINLOCK_H_ */
