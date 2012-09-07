#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>

#include <gt_thread.h>

/* comment this out to make the application single threaded */
#define USE_GTTHREADS
#define THREAD_COUNT 128
const int matrix_sizes[] = {
	64, 128, 256, 512
        };

typedef struct square_matrix {
	int **buf;
	int size;
} square_matrix_t;

typedef struct uthread_arg {
	square_matrix_t *a;
	square_matrix_t *b;
	square_matrix_t *c;
	uthread_attr_t *attr;
	uthread_tid tid;
	struct timeval create_time;
	struct timeval start_time;
	struct timeval end_time;
} uthread_arg_t;

/* malloc that fails */
static inline void *emalloc(size_t size)
{
	void *p = malloc(size);
	if (!p) {
		fprintf(stderr, "Malloc failure");
		exit(EXIT_FAILURE);
	}
	return p;
}

/* prints a matrix; for debugging */
static void print_matrix(square_matrix_t *m)
{
	int i, j;
	for (i = 0; i < m->size; i++) {
		for (j = 0; j < m->size; j++)
			printf(" %d ", m->buf[i][j]);
		printf("\n");
	}
	return;
}

/* converts a timeval to integral microseconds */
static inline unsigned long tv2us(struct timeval *tv)
{
	return (tv->tv_sec * 1000000) + tv->tv_usec;
}

/* converts an integer in microseconds to a struct timeval */
static inline void us2tv(unsigned long us, struct timeval *tv)
{
	tv->tv_sec = us / 1000000;
	tv->tv_usec = us % 1000000;
}

/* formats and prints the timeval into up to `n` characters of string `s`.
 * Returns as in sprintf() */
int timeval_snprintf(char *s, int n, struct timeval *tv)
{
	return snprintf(s, n, "%ld.%06ld", tv->tv_sec, tv->tv_usec);
}

/* performs "final - initial", puts result in `result`. returns 1 if answer is negative,
 * 0 otherwise. taken from gnu.org */
static int timeval_subtract(struct timeval *result, struct timeval *final,
                            struct timeval *initial)
{
	/* Perform the carry for the later subtraction by updating y. */
	if (final->tv_usec < initial->tv_usec) {
		int nsec = (initial->tv_usec - final->tv_usec) / 1000000 + 1;
		initial->tv_usec -= 1000000 * nsec;
		initial->tv_sec += nsec;
	}
	if (final->tv_usec - initial->tv_usec > 1000000) {
		int nsec = (final->tv_usec - initial->tv_usec) / 1000000;
		initial->tv_usec += 1000000 * nsec;
		initial->tv_sec -= nsec;
	}

	/* Compute the time remaining to wait. tv_usec is certainly positive. */
	result->tv_sec = final->tv_sec - initial->tv_sec;
	result->tv_usec = final->tv_usec - initial->tv_usec;

	/* Return 1 if result is negative. */
	return final->tv_sec < initial->tv_sec;
}

/* Alloc's and initializes a struct matrix */
static square_matrix_t *matrix_create(int size, int val)
{
	int i, j;
	square_matrix_t *m = emalloc(sizeof(*m));
	m->size = size;

	m->buf = emalloc(sizeof(*m->buf) * size);
	for (i = 0; i < m->size; i++) {
		m->buf[i] = emalloc(sizeof(**m->buf) * size);
	}

	for (i = 0; i < m->size; i++) {
		for (j = 0; j < m->size; ++j) {
			m->buf[i][j] = val;
		}
	}
	return m;
}

static int mulmat(void *arg_)
{
	uthread_arg_t *arg = arg_;
	gettimeofday(&arg->start_time, NULL);

	square_matrix_t *a, *b, *c;
	a = arg->a;
	b = arg->b;
	c = arg->c;

	int size = a->size; // should be the same for all arrays
	for (int i = 0; i < size; ++i) {
		if (!(i % 100))
			printf("working...\n");
		for (int j = 0; j < size; ++j) {
			for (int k = 0; k < size; ++k) {
				c->buf[i][j] += a->buf[i][k] * b->buf[k][j];
			}
		}
	}

	gettimeofday(&arg->end_time, NULL);
	return 0;
}

int main(int argc, char **argv)
{
	uthread_arg_t thread_args[THREAD_COUNT];

	uthread_arg_t *thread_arg = thread_args;
	int matrix_sizes_count = sizeof(matrix_sizes) / sizeof(matrix_sizes[0]);
	int threads_per_matrix_size = THREAD_COUNT / matrix_sizes_count;
	for (int i = 0; i < matrix_sizes_count; ++i) {
		for (int j = 0; j < threads_per_matrix_size; ++j) {
			int val = i * 100 + j;
			thread_arg->a = matrix_create(matrix_sizes[i], val);
			thread_arg->b = matrix_create(matrix_sizes[i], val);
			thread_arg->c = matrix_create(matrix_sizes[i], val);
			thread_arg->attr = uthread_attr_create();
			uthread_attr_init(thread_arg->attr, 0);
			thread_arg++;
		}
	}

#ifdef USE_GTTHREADS
	gtthread_options_t opt;
	gtthread_options_init(&opt);
	opt.scheduler_type = SCHEDULER_CFS;	// completely fair
//	opt.scheduler_type = SCHEDULER_PCS;	// priority
	/* opt.lwp_count = 1; */
	gtthread_app_init(&opt);
#endif

	struct timeval app_start_time, app_end_time, app_elapsed_time;
	gettimeofday(&app_start_time, NULL);
	for (int i = 0; i < THREAD_COUNT; ++i) {
		gettimeofday(&thread_args[i].create_time, NULL);

#ifdef USE_GTTHREADS
		printf("creating uthread\n");
		uthread_create(&thread_args[i].tid, thread_args[i].attr,
		               &mulmat,
		               &thread_args[i]);
#else
		thread_args[i].tid = i;
		mulmat(&thread_args[i]);
#endif

	}

#ifdef USE_GTTHREADS
	gtthread_app_exit();
#endif

	gettimeofday(&app_end_time, NULL);

	/* use as tmp storage for printing timevals */
	int time_str_size = 1024;
	char time_str[time_str_size];

	/* get total application time */
	timeval_subtract(&app_elapsed_time, &app_end_time, &app_start_time);
	timeval_snprintf(time_str, time_str_size, &app_elapsed_time);
	printf("Total application elapsed time: %s s\n", time_str);

	/* all times in microseconds */
	unsigned long thread_cpu_times[THREAD_COUNT];
	unsigned long thread_elapsed_times[THREAD_COUNT];
	unsigned long means_cpu[matrix_sizes_count];
	unsigned long means_elapsed[matrix_sizes_count];
	unsigned long std_dev_cpu[matrix_sizes_count];
	unsigned long std_dev_elapsed[matrix_sizes_count];

	struct timeval thread_cpu_time, thread_elapsed_time;
	for (int i = 0; i < matrix_sizes_count; ++i) {
		means_cpu[i] = 0;
		means_elapsed[i] = 0;
		std_dev_cpu[i] = 0;
		std_dev_elapsed[i] = 0;
		for (int j = 0; j < threads_per_matrix_size; ++j) {
			int t = i * threads_per_matrix_size + j;
			thread_arg = &thread_args[t];

			/* Wall time as seen by the thread being scheduled */
			timeval_subtract(&thread_elapsed_time,
			                 &thread_arg->end_time,
			                 &thread_arg->start_time);

#ifdef USE_GTTHREADS
			/* CPU time as calculated by gtthreads */
			uthread_attr_getcputime(thread_arg->attr,
			                        &thread_cpu_time);
#else
			/* single threaded, so cpu time = measured elapsed
			 * time */
			thread_cpu_time = thread_elapsed_time;
#endif

			thread_cpu_times[t] = tv2us(&thread_cpu_time);
			means_cpu[i] += thread_cpu_times[t];
			thread_elapsed_times[t] = tv2us(&thread_elapsed_time);
			means_elapsed[i] += thread_elapsed_times[t];

			printf("Thread %3d CPU time: %lu us, elapsed time: %lu us\n",
			       thread_args[t].tid,
			       tv2us(&thread_cpu_time),
			       tv2us(&thread_elapsed_time));
		}
	}

	/* loop back through, getting data needed for std deviation */
	long diff_from_mean;
	for (int i = 0; i < matrix_sizes_count; ++i) {
		means_cpu[i] /= threads_per_matrix_size;
		means_elapsed[i] /= threads_per_matrix_size;
		for (int j = 0; j < threads_per_matrix_size; ++j) {
			int t = i * threads_per_matrix_size + j;
			diff_from_mean = thread_cpu_times[t] - means_cpu[i];
			std_dev_cpu[i] += diff_from_mean * diff_from_mean;

			diff_from_mean = thread_elapsed_times[t]
			        - means_elapsed[i];
			std_dev_elapsed[i] += diff_from_mean * diff_from_mean;
		}
		std_dev_cpu[i] /= threads_per_matrix_size;
		std_dev_cpu[i] = sqrt(std_dev_cpu[i]);
		std_dev_elapsed[i] /= threads_per_matrix_size;
		std_dev_elapsed[i] = sqrt(std_dev_elapsed[i]);
	}

	int field_width = 24;
	printf("%-*s" "%-*s" "%-*s" "\n",
	       field_width, "Matrix Size",
	       field_width, "CPU Time",
	       field_width, "Elapsed Time"
	       );
	printf("%-*s" "%-*s" "%-*s" "\n",
	       field_width, " ",
	       field_width, "mean (std dev)",
	       field_width, "mean (std dev)"
	       );

	char data[1024] = "";
	for (int i = 0; i < matrix_sizes_count; ++i) {
		printf("%-*d", field_width, matrix_sizes[i]);

		sprintf(data, "%lu (%lu)", means_cpu[i], std_dev_cpu[i]);
		printf("%-*s", field_width, data);

		sprintf(data, "%lu (%lu)", means_elapsed[i], std_dev_elapsed[i]);
		printf("%-*s\n", field_width, data);
	}
}
