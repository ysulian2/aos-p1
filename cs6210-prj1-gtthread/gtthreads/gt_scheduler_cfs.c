/*
 * gt_cfs.c
 *
 * Implements the completely fair scheduler, following the generic scheduler
 * interface
 */

#include <assert.h>
#include <sys/time.h>
#include <time.h>

#include "gt_scheduler.h"
#include "gt_scheduler_cfs.h"
#include "gt_uthread.h"
#include "gt_kthread.h"
#include "gt_common.h"
#include "gt_spinlock.h"
#include "rb_tree/red_black_tree.h"

#define CFS_DEFAULT_PRIORITY 20
#define CFS_DEFAULT_LATENCY_us 40000 /* 40 ms */
#define CFS_MIN_GRANULARITY_us 20000 /* 20 ms */

/* global singleton scheduler */
extern scheduler_t scheduler;

/* use this to cast the scheduler data void * to something we can use */
#define SCHED_DATA scheduler.data.buf

typedef struct cfs_uthread {
	struct uthread *uthread;
	long unsigned vruntime;
	unsigned priority;
        int gid;
	long unsigned key; // key for rb tree, set to vruntime - min_vruntime
	rb_red_blk_node *node;
} cfs_uthread_t;

typedef struct cfs_kthread {
	gt_spinlock_t lock;
	struct kthread *k_ctx;
	cfs_uthread_t *current_cfs_uthread;
	rb_red_blk_tree *tree;
	int cfs_uthread_count;
	long unsigned latency; // epoch length
	long unsigned min_vruntime;
	float load; // sum of priorities of all tasks on kthread
} cfs_kthread_t;

/* global cfs data */
typedef struct cfs_data {
	gt_spinlock_t lock;
	int cfs_kthread_count;
	cfs_kthread_t *cfs_kthreads;	// array, indexed by cpuid
	unsigned last_cpu_assiged; // used for RR cpu asignment
} cfs_data_t;

/* returns the corresponding pcs_kthread_t for the given kthread_t */
static inline cfs_kthread_t *cfs_get_kthread(kthread_t *k_ctx)
{
	cfs_data_t *cfs_data = SCHED_DATA;
	return &cfs_data->cfs_kthreads[k_ctx->cpuid];
}

/* converts a timeval to integral microseconds */
static inline unsigned long cfs_tv2us(struct timeval *tv)
{
	return (tv->tv_sec * 1000000) + tv->tv_usec;
}

/* converts an integer in microseconds to a struct timeval */
static inline void cfs_us2tv(unsigned long us, struct timeval *tv)
{
	tv->tv_sec = us / 1000000;
	tv->tv_usec = us % 1000000;
}

void cfs_check_group_active_helper(rb_red_blk_tree* tree, rb_red_blk_node* node, int* checkList)
{
  if(node!= tree->nil){
    cfs_check_group_active_helper(tree, node->left, checkList);
    cfs_uthread_t* ut = node->info;
    checkList[ut->gid] = 1;
    cfs_check_group_active_helper(tree, node->right, checkList);
  }

}

int cfs_check_group_active(rb_red_blk_tree* tree)
{
  int max = tree->max_gid;
  int checkList[max];
  for(int i = 0; i < max; i++){
    checkList[i] = 0;
  }
  //printf("max gid %d\n", max);
  cfs_check_group_active_helper(tree, tree->root->left, checkList);
  
  for(int i = 0; i < max; i++){
    if(checkList[i] == 0){
      printf("%d not active!\n", i);
      return -1;
    }
  }
  
  return 0;
}

/* calculates the timeslice from vruntime and the load on the cpu */
static void cfs_calculate_timeslice(cfs_kthread_t *cfs_kthread,
                                    struct timeval *timeslice)
{
	cfs_uthread_t *cfs_uthread = cfs_kthread->current_cfs_uthread;
	unsigned long timeslice_us = cfs_kthread->latency
	        * (cfs_uthread->priority / cfs_kthread->load);
	cfs_us2tv(timeslice_us, timeslice);
}

/* called right before current uthread resumes execution. should set a timer to ensure
 * that we get back to scheduling again.
 */
void cfs_resume_uthread(kthread_t *k_ctx)
{
	assert(k_ctx->current_uthread);
	checkpoint("k%d: u%d: CFS: Setting timer",
	           k_ctx->cpuid, k_ctx->current_uthread->tid);
	k_ctx->current_uthread->state = UTHREAD_RUNNING;

	struct itimerval timeslice = {
	        .it_interval = {
	                0, 0 }  // no repeat
	};

	cfs_kthread_t *cfs_kthread = cfs_get_kthread(k_ctx);
	cfs_calculate_timeslice(cfs_kthread, &timeslice.it_value);

	if (setitimer(ITIMER_VIRTUAL, &timeslice, NULL)) // ignore old timer
		fail_perror("setitimer");
	return;
}

uthread_t *cfs_pick_next_uthread(kthread_t *k_ctx)
{
	checkpoint("k%d: CFS: Picking next uthread", k_ctx->cpuid);

	cfs_kthread_t *cfs_kthread = cfs_get_kthread(k_ctx);
	assert(cfs_kthread != NULL);

	int flag = cfs_check_group_active(cfs_kthread->tree);
	if(flag == -1){
	  return NULL;
	}

	gt_spin_lock(&cfs_kthread->lock);
	rb_red_blk_node *min = RBDeleteMin(cfs_kthread->tree);
	assert(min != cfs_kthread->tree->nil);
	if (!min) {
		cfs_kthread->current_cfs_uthread = NULL;
		cfs_kthread->min_vruntime = 0;
		gt_spin_unlock(&cfs_kthread->lock);
		return NULL;
	}

	cfs_uthread_t *min_cfs_uthread = min->info;
	checkpoint("k%d: u%d: Choosing uthread with vruntime %lu",
	           cfs_kthread->k_ctx->cpuid, min_cfs_uthread->uthread->tid,
	           min_cfs_uthread->key);
	cfs_kthread->current_cfs_uthread = min_cfs_uthread;
	cfs_kthread->min_vruntime = min_cfs_uthread->vruntime;
	gt_spin_unlock(&cfs_kthread->lock);
	return min_cfs_uthread->uthread;
}

static void cfs_update_vruntime(cfs_uthread_t *cfs_uthread)
{
	struct timeval *cputime = &cfs_uthread->uthread->attr->execution_time;
	unsigned long cputime_us = cfs_tv2us(cputime);
	cfs_uthread->vruntime += cputime_us * cfs_uthread->priority;
}

uthread_t *cfs_preemt_current_uthread(kthread_t *k_ctx)
{
	checkpoint("k%d: CFS: Preempting uthread", k_ctx->cpuid);
	uthread_t *cur_uthread = k_ctx->current_uthread;
	if (cur_uthread == NULL)
		return NULL;

	cfs_kthread_t *cfs_kthread = cfs_get_kthread(k_ctx);
	cfs_uthread_t *cfs_cur_uthread = cfs_kthread->current_cfs_uthread;

	if (cur_uthread->state == UTHREAD_DONE) {
		checkpoint("u%d: CFS: uthread done", cur_uthread->tid);
		cfs_kthread->load -= cfs_cur_uthread->priority;
		// FIXME free the node and the uthread?
		return NULL;
	}

	checkpoint("u%d: CFS: uthread still runnable", cur_uthread->tid);
	cur_uthread->state = UTHREAD_RUNNABLE;

	cfs_update_vruntime(cfs_cur_uthread);
	cfs_cur_uthread->key = cfs_cur_uthread->vruntime
	        - cfs_kthread->min_vruntime;

	checkpoint("u%d: CFS: insert into rb tree", cur_uthread->tid);
	RBTreeInsert(cfs_kthread->tree, cfs_cur_uthread->node);

	return cur_uthread;
}

/* finds and returns a suitable target kthread for the uthread */
static cfs_kthread_t *cfs_find_kthread_target(cfs_uthread_t *cfs_uthread,
                                              cfs_data_t *cfs_data)
{
	checkpoint("u%d: CFS: Finding cpu target", cfs_uthread->uthread->tid);
	cfs_kthread_t *cfs_kthread;
	unsigned int target_cpu = cfs_data->last_cpu_assiged;
	/* round robin through the cpus */
	do {
		checkpoint("%s", "finding cpu...");
		target_cpu = (target_cpu + 1) % cfs_data->cfs_kthread_count;
		cfs_kthread = &cfs_data->cfs_kthreads[target_cpu];
	} while (!kthread_is_schedulable(cfs_kthread->k_ctx));
	assert(cfs_kthread->k_ctx != NULL);
	cfs_data->last_cpu_assiged = target_cpu;
	checkpoint("u%d: CFS: target cpu set to %d",
	           cfs_uthread->uthread->tid, target_cpu);
	return cfs_kthread;
}

static inline long unsigned max(long unsigned a, long unsigned b)
{
	return a > b ? a : b;
}

static kthread_t *cfs_uthread_init(uthread_t *uthread)
{
	checkpoint("u%d: CFS: init uthread", uthread->tid);

	cfs_data_t *cfs_data = SCHED_DATA;
	cfs_uthread_t *cfs_uthread = emalloc(sizeof(*cfs_uthread));
	cfs_uthread->uthread = uthread;
	cfs_uthread->priority = CFS_DEFAULT_PRIORITY;
	cfs_uthread->gid = uthread->attr->gid;

	gt_spin_lock(&cfs_data->lock);
	cfs_kthread_t *cfs_kthread = cfs_find_kthread_target(cfs_uthread,
	                                                     cfs_data);
	gt_spin_unlock(&cfs_data->lock);

	if(cfs_uthread->gid > cfs_kthread->tree->max_gid){
	  cfs_kthread->tree->max_gid = cfs_uthread->gid;
	}

	/* update the kthread's load and latency, if necessary */
	gt_spin_lock(&cfs_kthread->lock);
	cfs_kthread->cfs_uthread_count++;
	cfs_kthread->latency =
	        max(CFS_DEFAULT_LATENCY_us,
	            cfs_kthread->cfs_uthread_count * CFS_MIN_GRANULARITY_us);
	cfs_kthread->load += cfs_uthread->priority;
	cfs_uthread->vruntime = cfs_kthread->min_vruntime;
	cfs_uthread->key = 0;
	gt_spin_unlock(&cfs_kthread->lock);

	checkpoint("u%d: CFS: Creating node", uthread->tid);
	cfs_uthread->node = RBNodeCreate(&cfs_uthread->key, cfs_uthread);
	checkpoint("u%d: CFS: Insert into rb tree", cfs_uthread->uthread->tid);
	RBTreeInsert(cfs_kthread->tree, cfs_uthread->node);

	return cfs_kthread->k_ctx;
}

/* these functions are for the rbtree. Several are no-ops. The tree is keyed
 * on vruntime, and the info pointers are to objects of type cfs_uthread_t */
/* CompFunc takes two void pointers to keys and returns 1 if the first
 * argument is "greater than" the second.  */
static int cfs_rb_compare_key(const void *keya, const void *keyb)
{
	const long unsigned *a = keya;
	const long unsigned *b = keyb;
	if (*a > *b)
		return (1);
	if (*a < *b)
		return (-1);
	return 0;
}
static void cfs_rb_print_key(const void *key)
{
	printf("%lu", *(long unsigned *) key);
}
static void cfs_rb_destroy_key(void *key)
{
}
static void cfs_rb_print_info(void *info)
{
	printf("u%d:", ((cfs_uthread_t *) info)->uthread->tid);
}
static void cfs_rb_destroy_info(void *info)
{
}

/* called at every kthread_create(). Assumes cfs_init() has already been
 * called */
void cfs_kthread_init(kthread_t *k_ctx)
{
	checkpoint("k%d: CFS: init kthread", k_ctx->cpuid);
	gt_spin_lock(&scheduler.lock);
	cfs_kthread_t *cfs_kthread = cfs_get_kthread(k_ctx);
	gt_spinlock_init(&cfs_kthread->lock);
	cfs_kthread->k_ctx = k_ctx;
	cfs_kthread->current_cfs_uthread = NULL;
	cfs_kthread->cfs_uthread_count = 0;
	cfs_kthread->latency = CFS_DEFAULT_LATENCY_us;
	cfs_kthread->min_vruntime = 0;
	cfs_kthread->tree = RBTreeCreate(&cfs_rb_compare_key,
	                                 &cfs_rb_destroy_key,
	                                 &cfs_rb_destroy_info,
	                                 &cfs_rb_print_key,
	                                 &cfs_rb_print_info);
	cfs_data_t *cfs_data = SCHED_DATA;
	cfs_data->cfs_kthread_count++;
	gt_spin_unlock(&scheduler.lock);
}

static void *cfs_create_sched_data(int lwp_count)
{
	cfs_data_t *cfs_data = ecalloc(sizeof(*cfs_data));
	gt_spinlock_init(&cfs_data->lock);
	cfs_data->last_cpu_assiged = 0;
	/* array of kthread_t, index by kthread_t->cpuid */
	cfs_kthread_t *cfs_kthreads = ecalloc(
	        lwp_count * sizeof(*cfs_kthreads));
	cfs_data->cfs_kthreads = cfs_kthreads;
	return cfs_data;
}

static void cfs_destroy_sched_data(void *data)
{
	cfs_data_t *cfs_data = data;
	for (int i = 0; i < cfs_data->cfs_kthread_count; ++i) {
		RBTreeDestroy(cfs_data->cfs_kthreads[i].tree);
	}
	free(cfs_data->cfs_kthreads);
	free(cfs_data);
}

void cfs_init(scheduler_t *scheduler, int lwp_count)
{
	checkpoint("%s", "CFS: initialization");
	scheduler->kthread_init = &cfs_kthread_init;
	scheduler->uthread_init = &cfs_uthread_init;
	scheduler->preempt_current_uthread = &cfs_preemt_current_uthread;
	scheduler->pick_next_uthread = &cfs_pick_next_uthread;
	scheduler->resume_uthread = &cfs_resume_uthread;

	scheduler->data.buf = cfs_create_sched_data(lwp_count);
	scheduler->data.destroy = &cfs_destroy_sched_data;
}
