/*
 * gt_pfs.c
 *
 * Implements the completely fair scheduler, following the generic scheduler
 * interface
 */

#include <assert.h>
#include <sys/time.h>
#include <time.h>

#include "gt_scheduler.h"
#include "gt_scheduler_pfs.h"
#include "gt_uthread.h"
#include "gt_kthread.h"
#include "gt_common.h"
#include "gt_spinlock.h"
#include "rb_tree/red_black_tree.h"

#define PFS_DEFAULT_PRIORITY 20
#define PFS_DEFAULT_LATENCY_us 40000 /* 40 ms */
#define PFS_MIN_GRANULARITY_us 20000 /* 20 ms */
#define PFS_DEFAULT_WEIGHT 100

/* global singleton scheduler */
extern scheduler_t scheduler;

/* use this to cast the scheduler data void * to something we can use */
#define SCHED_DATA scheduler.data.buf

typedef struct pfs_uthread {
	struct uthread *uthread;
	long unsigned vruntime;
	unsigned priority;
        int weight;
        int gid;
	long unsigned key; // key for rb tree, set to vruntime - min_vruntime
	rb_red_blk_node *node;
} pfs_uthread_t;

typedef struct pfs_kthread {
	gt_spinlock_t lock;
	struct kthread *k_ctx;
	pfs_uthread_t *current_pfs_uthread;
	rb_red_blk_tree *tree;
	int pfs_uthread_count;
	long unsigned latency; // epoch length
	long unsigned min_vruntime;
	float load; // sum of priorities of all tasks on kthread
} pfs_kthread_t;

/* global pfs data */
typedef struct pfs_data {
	gt_spinlock_t lock;
	int pfs_kthread_count;
	pfs_kthread_t *pfs_kthreads;	// array, indexed by cpuid
	unsigned last_cpu_assiged; // used for RR cpu asignment
} pfs_data_t;

/* returns the corresponding pcs_kthread_t for the given kthread_t */
static inline pfs_kthread_t *pfs_get_kthread(kthread_t *k_ctx)
{
	pfs_data_t *pfs_data = SCHED_DATA;
	return &pfs_data->pfs_kthreads[k_ctx->cpuid];
}

/* converts a timeval to integral microseconds */
static inline unsigned long pfs_tv2us(struct timeval *tv)
{
	return (tv->tv_sec * 1000000) + tv->tv_usec;
}

/* converts an integer in microseconds to a struct timeval */
static inline void pfs_us2tv(unsigned long us, struct timeval *tv)
{
	tv->tv_sec = us / 1000000;
	tv->tv_usec = us % 1000000;
}

int pfs_update_weight(rb_red_blk_tree* tree, rb_red_blk_node* node, int group_id, int index, rb_red_blk_node** group)
{

  int new_index = index;
  //printf("index = %d\n", new_index);

  if(node != tree->nil) {
    new_index = pfs_update_weight(tree, node->left, group_id, index, group);
    pfs_uthread_t* curr = node->info;
    if (curr->gid == group_id) {
      group[new_index] = node;
      new_index++;
    }

    new_index = pfs_update_weight(tree, node->right, group_id, new_index, group);
    
  }
  //printf("new index = %d\n", new_index);
  return new_index;
}

int pfs_update_group_weight(rb_red_blk_tree* tree, int group_id)
{
  rb_red_blk_node* group[500];
  int i = 0;
  
  if(tree->max_gid < group_id){
    tree->max_gid = group_id;
    tree->gid_list[group_id] = 1;
  }

  int groupSize = pfs_update_weight(tree, tree->root->left, group_id, i, group);

  if(groupSize > 0){
    for(i = 0; i < groupSize; i++){
      //      printf("updating %d\n", i);
      //remove from rbtree
      rb_red_blk_node* node;
      node = RBDelete(tree, group[i]);
      //update weight
      pfs_uthread_t* ut = node->info;
      ut->weight = (int)PFS_DEFAULT_WEIGHT/(groupSize+1);// +1 to include the new thread
      //insert to rbtree
      rb_red_blk_node* newNode;
      newNode = RBTreeInsert(tree, node);
   }
  }

  return groupSize;
}

void pfs_check_group_active_helper(rb_red_blk_tree* tree, rb_red_blk_node* node, int* checkList)
{
  if(node!= tree->nil){
    pfs_check_group_active_helper(tree, node->left, checkList);
    pfs_uthread_t* ut = node->info;
    checkList[ut->gid] = 1;
    pfs_check_group_active_helper(tree, node->right, checkList);
  }

}

int pfs_check_group_active(rb_red_blk_tree* tree)
{
  int max = tree->max_gid;
  int checkList[max];
  for(int i = 0; i < max; i++){
    checkList[i] = 0;
  }
  //printf("max gid %d\n", max);
  pfs_check_group_active_helper(tree, tree->root->left, checkList);
  
  for(int i = 0; i < max; i++){
    if(checkList[i] == 0){
      printf("%d not active!\n", i);
      return -1;
    }
  }
  
  return 0;
}


/* calculates the timeslice from vruntime and the load on the cpu */
static void pfs_calculate_timeslice(pfs_kthread_t *pfs_kthread,
                                    struct timeval *timeslice)
{
	pfs_uthread_t *pfs_uthread = pfs_kthread->current_pfs_uthread;
	/*	unsigned long timeslice_us = pfs_kthread->latency
	        * (pfs_uthread->priority / pfs_kthread->load);
	*/
	unsigned long timeslice_us = pfs_kthread->latency
	  *(pfs_uthread->weight / pfs_kthread->load);
	pfs_us2tv(timeslice_us, timeslice);
}

/* called right before current uthread resumes execution. should set a timer to ensure
 * that we get back to scheduling again.
 */
void pfs_resume_uthread(kthread_t *k_ctx)
{
	assert(k_ctx->current_uthread);
	checkpoint("k%d: u%d: PFS: Setting timer",
	           k_ctx->cpuid, k_ctx->current_uthread->tid);
	k_ctx->current_uthread->state = UTHREAD_RUNNING;

	struct itimerval timeslice = {
	        .it_interval = {
	                0, 0 }  // no repeat
	};

	pfs_kthread_t *pfs_kthread = pfs_get_kthread(k_ctx);
	pfs_calculate_timeslice(pfs_kthread, &timeslice.it_value);

	if (setitimer(ITIMER_VIRTUAL, &timeslice, NULL)) // ignore old timer
		fail_perror("setitimer");
	return;
}

uthread_t *pfs_pick_next_uthread(kthread_t *k_ctx)
{
	checkpoint("k%d: PFS: Picking next uthread", k_ctx->cpuid);

	pfs_kthread_t *pfs_kthread = pfs_get_kthread(k_ctx);
	assert(pfs_kthread != NULL);

	int flag = pfs_check_group_active(pfs_kthread->tree);
	if(flag == -1){
	  return NULL;
	}
	
	gt_spin_lock(&pfs_kthread->lock);
	rb_red_blk_node *min = RBDeleteMin(pfs_kthread->tree);
	assert(min != pfs_kthread->tree->nil);
	if (!min) {
		pfs_kthread->current_pfs_uthread = NULL;
		pfs_kthread->min_vruntime = 0;
		gt_spin_unlock(&pfs_kthread->lock);
		return NULL;
	}

	pfs_uthread_t *min_pfs_uthread = min->info;
	checkpoint("k%d: u%d: Choosing uthread with vruntime %lu",
	           pfs_kthread->k_ctx->cpuid, min_pfs_uthread->uthread->tid,
	           min_pfs_uthread->key);
	pfs_kthread->current_pfs_uthread = min_pfs_uthread;
	pfs_kthread->min_vruntime = min_pfs_uthread->vruntime;
	gt_spin_unlock(&pfs_kthread->lock);
	return min_pfs_uthread->uthread;
}

static void pfs_update_vruntime(pfs_uthread_t *pfs_uthread)
{
	struct timeval *cputime = &pfs_uthread->uthread->attr->execution_time;
	unsigned long cputime_us = pfs_tv2us(cputime);
	pfs_uthread->vruntime += cputime_us *1024/ pfs_uthread->weight;//priority;
}

uthread_t *pfs_preemt_current_uthread(kthread_t *k_ctx)
{
	checkpoint("k%d: PFS: Preempting uthread", k_ctx->cpuid);
	uthread_t *cur_uthread = k_ctx->current_uthread;
	if (cur_uthread == NULL)
		return NULL;

	pfs_kthread_t *pfs_kthread = pfs_get_kthread(k_ctx);
	pfs_uthread_t *pfs_cur_uthread = pfs_kthread->current_pfs_uthread;

	if (cur_uthread->state == UTHREAD_DONE) {
		checkpoint("u%d: PFS: uthread done", cur_uthread->tid);
		pfs_kthread->load -= pfs_cur_uthread->weight;//priority;
		// FIXME free the node and the uthread?
		return NULL;
	}

	checkpoint("u%d: PFS: uthread still runnable", cur_uthread->tid);
	cur_uthread->state = UTHREAD_RUNNABLE;

	pfs_update_vruntime(pfs_cur_uthread);
	pfs_cur_uthread->key = pfs_cur_uthread->vruntime
	        - pfs_kthread->min_vruntime;

	checkpoint("u%d: PFS: insert into rb tree", cur_uthread->tid);
	RBTreeInsert(pfs_kthread->tree, pfs_cur_uthread->node);

	return cur_uthread;
}

/* finds and returns a suitable target kthread for the uthread */
static pfs_kthread_t *pfs_find_kthread_target(pfs_uthread_t *pfs_uthread,
                                              pfs_data_t *pfs_data)
{
	checkpoint("u%d: PFS: Finding cpu target", pfs_uthread->uthread->tid);
	pfs_kthread_t *pfs_kthread;
	unsigned int target_cpu = pfs_data->last_cpu_assiged;
	/* round robin through the cpus */
	do {
		checkpoint("%s", "finding cpu...");
		target_cpu = (target_cpu + 1) % pfs_data->pfs_kthread_count;
		pfs_kthread = &pfs_data->pfs_kthreads[target_cpu];
	} while (!kthread_is_schedulable(pfs_kthread->k_ctx));
	assert(pfs_kthread->k_ctx != NULL);
	pfs_data->last_cpu_assiged = target_cpu;
	checkpoint("u%d: PFS: target cpu set to %d",
	           pfs_uthread->uthread->tid, target_cpu);
	return pfs_kthread;
}

static inline long unsigned max(long unsigned a, long unsigned b)
{
	return a > b ? a : b;
}

static kthread_t *pfs_uthread_init(uthread_t *uthread)
{
	checkpoint("u%d: PFS: init uthread", uthread->tid);

	pfs_data_t *pfs_data = SCHED_DATA;
	pfs_uthread_t *pfs_uthread = emalloc(sizeof(*pfs_uthread));
	pfs_uthread->uthread = uthread;
	pfs_uthread->priority = PFS_DEFAULT_PRIORITY;
	pfs_uthread->weight = PFS_DEFAULT_WEIGHT;
	pfs_uthread->gid = uthread->attr->gid;

	gt_spin_lock(&pfs_data->lock);
	pfs_kthread_t *pfs_kthread = pfs_find_kthread_target(pfs_uthread,
	                                                     pfs_data);
	gt_spin_unlock(&pfs_data->lock);

	//printf("start update group %d\n", pfs_uthread->gid);
	//update weight
	if(pfs_uthread->gid > pfs_kthread->tree->max_gid){
	  pfs_kthread->tree->max_gid = pfs_uthread->gid;
	}
	int groupSize = pfs_update_group_weight(pfs_kthread->tree, pfs_uthread->gid);
	pfs_uthread->weight = (int)PFS_DEFAULT_WEIGHT/(groupSize+1);


	/* update the kthread's load and latency, if necessary */
	gt_spin_lock(&pfs_kthread->lock);
	pfs_kthread->pfs_uthread_count++;
	pfs_kthread->latency =
	        max(PFS_DEFAULT_LATENCY_us,
	            pfs_kthread->pfs_uthread_count * PFS_MIN_GRANULARITY_us);
	pfs_kthread->load += pfs_uthread->weight;//priority;
	pfs_uthread->vruntime = pfs_kthread->min_vruntime;
	pfs_uthread->key = 0;
	gt_spin_unlock(&pfs_kthread->lock);

	checkpoint("u%d: PFS: Creating node", uthread->tid);
	pfs_uthread->node = RBNodeCreate(&pfs_uthread->key, pfs_uthread);
	checkpoint("u%d: PFS: Insert into rb tree", pfs_uthread->uthread->tid);
	RBTreeInsert(pfs_kthread->tree, pfs_uthread->node);

	return pfs_kthread->k_ctx;
}

/* these functions are for the rbtree. Several are no-ops. The tree is keyed
 * on vruntime, and the info pointers are to objects of type pfs_uthread_t */
/* CompFunc takes two void pointers to keys and returns 1 if the first
 * argument is "greater than" the second.  */
static int pfs_rb_compare_key(const void *keya, const void *keyb)
{
	const long unsigned *a = keya;
	const long unsigned *b = keyb;
	if (*a > *b)
		return (1);
	if (*a < *b)
		return (-1);
	return 0;
}
static void pfs_rb_print_key(const void *key)
{
	printf("%lu", *(long unsigned *) key);
}
static void pfs_rb_destroy_key(void *key)
{
}
static void pfs_rb_print_info(void *info)
{
	printf("u%d:", ((pfs_uthread_t *) info)->uthread->tid);
}
static void pfs_rb_destroy_info(void *info)
{
}

/* called at every kthread_create(). Assumes pfs_init() has already been
 * called */
void pfs_kthread_init(kthread_t *k_ctx)
{
	checkpoint("k%d: PFS: init kthread", k_ctx->cpuid);
	gt_spin_lock(&scheduler.lock);
	pfs_kthread_t *pfs_kthread = pfs_get_kthread(k_ctx);
	gt_spinlock_init(&pfs_kthread->lock);
	pfs_kthread->k_ctx = k_ctx;
	pfs_kthread->current_pfs_uthread = NULL;
	pfs_kthread->pfs_uthread_count = 0;
	pfs_kthread->latency = PFS_DEFAULT_LATENCY_us;
	pfs_kthread->min_vruntime = 0;
	pfs_kthread->tree = RBTreeCreate(&pfs_rb_compare_key,
	                                 &pfs_rb_destroy_key,
	                                 &pfs_rb_destroy_info,
	                                 &pfs_rb_print_key,
	                                 &pfs_rb_print_info);
	pfs_data_t *pfs_data = SCHED_DATA;
	pfs_data->pfs_kthread_count++;
	gt_spin_unlock(&scheduler.lock);
}

static void *pfs_create_sched_data(int lwp_count)
{
	pfs_data_t *pfs_data = ecalloc(sizeof(*pfs_data));
	gt_spinlock_init(&pfs_data->lock);
	pfs_data->last_cpu_assiged = 0;
	/* array of kthread_t, index by kthread_t->cpuid */
	pfs_kthread_t *pfs_kthreads = ecalloc(
	        lwp_count * sizeof(*pfs_kthreads));
	pfs_data->pfs_kthreads = pfs_kthreads;
	return pfs_data;
}

static void pfs_destroy_sched_data(void *data)
{
	pfs_data_t *pfs_data = data;
	for (int i = 0; i < pfs_data->pfs_kthread_count; ++i) {
		RBTreeDestroy(pfs_data->pfs_kthreads[i].tree);
	}
	free(pfs_data->pfs_kthreads);
	free(pfs_data);
}

void pfs_init(scheduler_t *scheduler, int lwp_count)
{
	checkpoint("%s", "PFS: initialization");
	scheduler->kthread_init = &pfs_kthread_init;
	scheduler->uthread_init = &pfs_uthread_init;
	scheduler->preempt_current_uthread = &pfs_preemt_current_uthread;
	scheduler->pick_next_uthread = &pfs_pick_next_uthread;
	scheduler->resume_uthread = &pfs_resume_uthread;

	scheduler->data.buf = pfs_create_sched_data(lwp_count);
	scheduler->data.destroy = &pfs_destroy_sched_data;
}


