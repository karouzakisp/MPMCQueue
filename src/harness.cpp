#include "../include/rigtorp/MPMCQueue.h"
#include "../include/rigtorp/benchmark.h"
#include "../include/rigtorp/bits.h"
#include "../include/rigtorp/cpumap.h"
#include "../include/rigtorp/delay.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef LOGN_OPS
#define LOGN_OPS 2
#endif

static long nops;
#ifndef NUM_ITERS
#define NUM_ITERS 5
#endif

#ifndef MAX_PROCS
#define MAX_PROCS 512
#endif

#ifndef MAX_ITERS
#define MAX_ITERS 20
#endif

#ifndef COV_THRESHOLD
#define COV_THRESHOLD 0.02
#endif

// #define SZ 100'000'000
#define SZ 100

static pthread_barrier_t barrier;
static double times[MAX_ITERS];
static double means[MAX_ITERS];
static double covs[MAX_ITERS];
static volatile int target;

rigtorp::MPMCQueue<void*> q(SZ, true);

static size_t elapsed_time(size_t us) {
  struct timeval t;
  gettimeofday(&t, NULL);
  return t.tv_sec * 1000000 + t.tv_usec - us;
}

static double compute_mean(const double* times) {
  int i;
  double sum = 0;

  for (i = 0; i < NUM_ITERS; ++i) {
    sum += times[i];
  }

  return sum / NUM_ITERS;
}

static double compute_cov(const double* times, double mean) {
  double variance = 0;

  int i;
  for (i = 0; i < NUM_ITERS; ++i) {
    variance += (times[i] - mean) * (times[i] - mean);
  }

  variance /= NUM_ITERS;

  double cov = sqrt(variance);
  ;
  cov /= mean;
  return cov;
}

static size_t reduce_min(long val, int id, int nprocs) {
  static long buffer[MAX_PROCS];

  buffer[id] = val;
  pthread_barrier_wait(&barrier);

  long min = LONG_MAX;
  int i;
  for (i = 0; i < nprocs; ++i) {
    if (buffer[i] < min)
      min = buffer[i];
  }

  return min;
}

static void report(int id, int nprocs, int i, long us) {
  long ms = reduce_min(us, id, nprocs);

  if (id == 0) {
    times[i] = ms / 1000.0;
    printf("  #%d elapsed time: %.2f ms\n", i + 1, times[i]);

    if (i + 1 >= NUM_ITERS) {
      int n = i + 1 - NUM_ITERS;

      means[i] = compute_mean(times + n);
      covs[i] = compute_cov(times + n, means[i]);

      if (covs[i] < COV_THRESHOLD) {
        target = i;
      }
    }
  }

  pthread_barrier_wait(&barrier);
}

#include <iostream>
void* benchmark(int id, int nprocs) {
  void* val = (void*)(intptr_t)(id + 1);
  delay_t state;
  delay_init(&state, id);

  int i;
  for (i = 0; i < nops / nprocs; ++i) {
    q.push_p(val);
    delay_exec(&state);

    q.pop_p(val);
    delay_exec(&state);
  }

  return val;
}

void init(int nprocs, int logn) {

  /** Use 10^5 as default input size. */
  if (logn == 0)
    logn = LOGN_OPS;

  /** Compute the number of ops to perform. */
  nops = 1;
  int i;
  for (i = 0; i < logn; ++i) {
    nops *= 10;
  }

  printf("  Number of operations: %ld\n", nops);
}

void thread_init(int id, int nprocs) { ; }

void thread_exit(int id, int nprocs) { ; }

#ifdef VERIFY
static int compare(const void* a, const void* b) {
  return *(long*)a - *(long*)b;
}
#endif

int verify(int nprocs, void** results) {
#ifndef VERIFY
  return 0;
#else
  qsort(results, nprocs, sizeof(void*), compare);

  int i;
  int ret = 0;

  for (i = 0; i < nprocs; ++i) {
    int res = (int)(intptr_t)results[i];
    if (res != i + 1) {
      fprintf(stderr, "expected %d but received %d\n", i + 1, res);
      ret = 1;
    }
  }

  if (ret != 1) {
    fprintf(stdout, "PASSED\n");
    puts("Printing array --> ");
    for (int k = 0; k < nprocs; k++) {
      int res = (int)(intptr_t)results[k];
      printf("%d\n", res);
    }

  } else {
    puts("Printing array --> ");
    for (int k = 0; k < nprocs; k++) {
      int res = (int)(intptr_t)results[k];
      printf("%d\n", res);
    }
  }
  return ret;
#endif
}

static void* thread(void* bits) {
  int id = bits_hi(bits);
  int nprocs = bits_lo(bits);

  cpu_set_t set;
  CPU_ZERO(&set);

  int cpu = cpumap(id, nprocs);
  CPU_SET(cpu, &set);
  sched_setaffinity(0, sizeof(set), &set);

  thread_init(id, nprocs);
  pthread_barrier_wait(&barrier);

  int i;
  void* result = NULL;

  for (i = 0; i < MAX_ITERS && target == 0; ++i) {
    long us = elapsed_time(0);
    result = benchmark(id, nprocs);
    pthread_barrier_wait(&barrier);
    us = elapsed_time(us);
    report(id, nprocs, i, us);
  }

  thread_exit(id, nprocs);
  return result;
}

int main(int argc, const char* argv[]) {
  int nprocs = 0;
  int n = 0;

  /** The first argument is nprocs. */
  if (argc > 1) {
    nprocs = atoi(argv[1]);
  }

  /**
   * Use the number of processors online as nprocs if it is not
   * specified.
   */
  if (nprocs == 0) {
    nprocs = sysconf(_SC_NPROCESSORS_ONLN);
  }

  if (nprocs <= 0)
    return 1;
  else {
    /** Set concurrency level. */
    pthread_setconcurrency(nprocs);
  }

  /**
   * The second argument is input size n.
   */
  if (argc > 2) {
    n = atoi(argv[2]);
  }

  pthread_barrier_init(&barrier, NULL, nprocs);
  printf("===========================================\n");
  printf("  Benchmark: %s\n", argv[0]);
  printf("  Number of processors: %d\n", nprocs);

  init(nprocs, n);

  pthread_t ths[nprocs];
  void* res[nprocs];

  int i;
  for (i = 1; i < nprocs; i++) {
    pthread_create(&ths[i], NULL, thread, bits_join(i, nprocs));
  }

  res[0] = thread(bits_join(0, nprocs));

  for (i = 1; i < nprocs; i++) {
    pthread_join(ths[i], &res[i]);
  }

  if (target == 0) {
    target = NUM_ITERS - 1;
    double minCov = covs[target];

    /** Pick the result that has the lowest CoV. */
    int i;
    for (i = NUM_ITERS; i < MAX_ITERS; ++i) {
      if (covs[i] < minCov) {
        minCov = covs[i];
        target = i;
      }
    }
  }

  double mean = means[target];
  double cov = covs[target];
  int i1 = target - NUM_ITERS + 2;
  int i2 = target + 1;

  printf("  Steady-state iterations: %d~%d\n", i1, i2);
  printf("  Coefficient of variation: %.2f\n", cov);
  printf("  Number of measurements: %d\n", NUM_ITERS);
  printf("  Mean of elapsed time: %.2f ms\n", mean);
  printf("===========================================\n");

  pthread_barrier_destroy(&barrier);
  return verify(nprocs, res);
}
