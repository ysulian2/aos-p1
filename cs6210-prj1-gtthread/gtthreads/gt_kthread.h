/*
 * gt_kthread.h
 *
 */

#ifndef GT_KTHREAD_H_
#define GT_KTHREAD_H_

#include <signal.h>
#include <ucontext.h>

struct uthread;

enum kthread_state {
	KTHREAD_INIT = 0,
	KTHREAD_RUNNABLE,
	KTHREAD_RUNNING,
	KTHREAD_DONE
};

typedef struct kthread {
	enum kthread_state state;
	unsigned cpuid;
	unsigned cpu_apic_id;
	pid_t pid;
	pid_t tid;
	struct uthread *current_uthread;
	ucontext_t sched_ctx;
	char sched_ctx_stack[2048];
} kthread_t;


/* create a kthread running on the specified lwp. The new thread's pid is
 * returned in `tid`. Returns a pointer to the new kthread_t if sucessfull,
 * NULL otherwise. */
kthread_t *kthread_create(pid_t *tid, int lwp);

/* returns the currently running kthread */
kthread_t *kthread_current_kthread();

/* returns 1 if a kthread is schedulable, 0 otherwise */
int kthread_is_schedulable(kthread_t *k_ctx);

void kthread_sched_handler(int signo);

/* Blocks until a uthread is available to schedule */
void kthread_wait_for_uthread(kthread_t *k_ctx);

/* Inlines */
/* apic-id of the cpu on which kthread is running */
static inline unsigned char kthread_apic_id(void)
{
/* IO APIC id is unique for a core and can be used as cpuid.
 * EBX[31:24] Bits 24-31 (8 bits) return the 8-bit unique.
 * Initial APIC ID for the processor this code is running on.*/
#define INITIAL_APIC_ID_BITS  0xFF000000
	unsigned int Regebx = 0;
	__asm__ __volatile__ (
		"movl $1, %%eax\n\t"
		"cpuid"
		:"=b" (Regebx)
		: :"%eax","%ecx","%edx");

	return((unsigned char)((Regebx & INITIAL_APIC_ID_BITS) >> 24));
#undef INITIAL_APIC_ID_BITS
}


#endif /* GT_KTHREAD_H_ */
