/* SPDX-License-Identifier: MIT */
/* Copyright 2023-2024 Alexander Zuepke */
/*
 * bench.c
 *
 * Benchmark memory performance
 *
 * Compile:
 *   $ gcc -O2 -DNDEBUG -W -Wall -Wextra -Werror bench.c -o bench
 *
 * Run (as root)
 *   $ ./bench <read|write|modify> [-s <size-in-MiB>] [-c <cpu>] [-p <prio>]
 *
 * azuepke, 2023-02-22: standalone version
 * azuepke, 2023-12-07: explicit prefetches
 * azuepke, 2023-12-12: more prefetches
 * azuepke, 2023-12-19: dump regulation-related PMU counters
 * azuepke, 2023-12-20: argument parsing
 * azuepke, 2023-12-24: incorporate worst-case memory access benchmarks
 * azuepke, 2023-12-25: test automation and CSV files
 * azuepke, 2024-03-28: RISC-V version
 */

#define _GNU_SOURCE	/* for sched_setaffinity() */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <linux/mman.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sched.h>
#include <linux/perf_event.h>
#include <unistd.h>
#include <asm/unistd.h>
#include <errno.h>
#include <assert.h>


/* program name */
#define PROGNAME "bench"

/* build ID, passed as string to compiler via -DBUILDID=<...> */
#ifndef BUILDID
#define BUILDID "local"
#endif

////////////////////////////////////////////////////////////////////////////////

/* hardcoded size of a cacheline */
#define CACHELINE_SIZE 64

#if defined __aarch64__
/* default mapping size */
#define DEFAULT_MB 8
#elif defined __riscv
/* default mapping size */
#define DEFAULT_MB 1
#elif defined __x86_64__
/* default mapping size */
#define DEFAULT_MB 16
#else
#errir unknown system
#endif

/* test mapping */
static char *map_addr;
static char *map_addr_end;
static size_t map_size;
/* ==0: small pages, 1: huge pages */
static int map_huge = 0;

/* global options */
static int option_perf = 0;
static int option_huge = 0;
static int option_step = 0;
/* ==0: infinite, >0: limited number of loops */
static int option_num_loops = 0;
/* print delay milliseconds, default 1s */
static int option_print_delay_ms = 1000;
static int option_all = 0;
static FILE *csv_file = NULL;
static int option_csv_no_header = 0;

////////////////////////////////////////////////////////////////////////////////

#if defined __aarch64__
#define NUM_PERF 2
static const unsigned long long perf_config[NUM_PERF] = {
	/* for Cortex-A53, A57, A72 */
	0x0017,	/* L2D refill */
	0x0018,	/* L2D write-back */
};
#elif defined __riscv
#define NUM_PERF 0
static const unsigned long long perf_config[NUM_PERF] = {
};
#elif defined __x86_64__
#define NUM_PERF 1
static const unsigned long long perf_config[NUM_PERF] = {
	/* for Intel */
	0x412e,	/* LONGEST_LAT_CACHE.MISS */
};
#endif

/* fds of perf */
static int perf_fds[NUM_PERF];

/* previous and current values and deltas */
static unsigned long long perf_prev_values[NUM_PERF];
static unsigned long long perf_curr_values[NUM_PERF];
static unsigned long long perf_delta_values[NUM_PERF];

static int perf_ok = 0;

/* perf_event_open() system call, not exported in libc */
static inline int perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu, int group_fd, unsigned long flags)
{
	return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static int perf_open(void)
{
	struct perf_event_attr attr = { 0 };
	unsigned long flags;
	unsigned int cpu;
	int group_fd;
	pid_t pid;
	int ctr;
	int err;
	int fd;

	attr.size = sizeof(attr);
	attr.type = PERF_TYPE_RAW;
	attr.config = 0;	/* see below */
	attr.read_format = PERF_FORMAT_GROUP;
	attr.disabled = 0; /* initially enabled */
	//attr.pinned = 1;	/* pinned to PMU -> EINVAL */
	//attr.exclusive = 1; /* exclusive use of PMU by this process -> EINVAL */

	pid = 0;		/* current process */
	cpu = -1;		/* any CPU */
	group_fd = -1;	/* first in group */
	flags = PERF_FLAG_FD_CLOEXEC;

	for (ctr = 0; ctr < NUM_PERF; ctr++) {
		/* 1st PMU counter is group leader */
		if (ctr == 0) {
			group_fd = -1;	/* first in group */
		} else {
			group_fd = perf_fds[0];
		}
		attr.config = perf_config[ctr];

		fd = perf_event_open(&attr, pid, cpu, group_fd, flags);
		if (fd == -1) {
			err = errno;
			if (err == EACCES) {
				printf("# perf tracing requires root permissions, rerun as root user\n");
			} else if (err == ENODEV) {
				printf("# perf tracing does not support tracing of hardware counters\n");
			} else {
				fprintf(stderr, "# perf tracing: error setting up perf counter %d type %u: ", ctr, attr.type);
				errno = err;
				perror("perf_event_open");
			}
			return err;
		}

		perf_fds[ctr] = fd;
	}

	perf_ok = 1;
	return 0;
}

static int perf_read(unsigned long long *values)
{
	unsigned long long buf[NUM_PERF + 2];
	int group_fd;
	ssize_t exp;
	ssize_t r;
	int err;

	if (!perf_ok) {
		for (int i = 0; i < NUM_PERF; i++) {
			values[i] = 0;
		}
		return 0;
	}

	/* We except to read one more 64-bit values than counters,
	 * the first value is the number of counters.
	 * The buffer has space for two more values to detect format issues.
	 */
	exp = (NUM_PERF + 1) * sizeof(buf[0]);

	group_fd = perf_fds[0];

	r = read(group_fd, buf, sizeof(buf));
	if (r == -1) {
		err = errno;
		perror("perf_read");
		return err;
	}
	if (r != exp) {
		fprintf(stderr, "perf_trace: read %zd, expected %zd\n", r, exp);
		return EINVAL;
	}
	if (buf[0] != NUM_PERF) {
		fprintf(stderr, "perf_trace: unexpected value %llu\n", buf[0]);
		return EINVAL;
	}

	for (int i = 0; i < NUM_PERF; i++) {
		values[i] = buf[i+1];
	}

	return 0;
}

static inline unsigned long long *perf_delta(const unsigned long long *curr_values, const unsigned long long *prev_values, unsigned long long *delta_values)
{
	for (int i = 0; i < NUM_PERF; i++) {
		delta_values[i] = curr_values[i] - prev_values[i];
	}

	return delta_values;
}

////////////////////////////////////////////////////////////////////////////////

/* return ts (in nanoseconds, risk of overflow) */
static inline unsigned long long timespec_ns(const struct timespec *ts)
{
	unsigned long long val;

	assert(ts != NULL);

	val = ts->tv_sec * 1000000000ull;
	val += ts->tv_nsec;

	return val;
}

/* ts += delta_ns */
static inline struct timespec *timespec_inc_by(struct timespec *ts, unsigned long long delta_ns)
{
	assert(ts != NULL);
	assert(ts->tv_sec >= 0);
	assert(ts->tv_nsec >= 0);
	assert(ts->tv_nsec < 1000000000);

	ts->tv_sec += delta_ns / 1000000000ull;
	ts->tv_nsec += delta_ns % 1000000000ull;
	if (ts->tv_nsec >= 1000000000) {
		ts->tv_nsec -= 1000000000;
		ts->tv_sec += 1;
	}

	assert(ts->tv_nsec >= 0);
	assert(ts->tv_nsec <= 999999999);

	return ts;
}

/* ts_out = ts_a - ts_b */
static inline struct timespec *timespec_sub(const struct timespec *ts_a, const struct timespec *ts_b, struct timespec *ts_out)
{
	assert(ts_a != NULL);
	assert(ts_a->tv_sec >= 0);
	assert(ts_a->tv_nsec >= 0);
	assert(ts_a->tv_nsec < 1000000000);
	assert(ts_b != NULL);
	assert(ts_b->tv_sec >= 0);
	assert(ts_b->tv_nsec >= 0);
	assert(ts_b->tv_nsec < 1000000000);
	assert(ts_out != NULL);

	ts_out->tv_sec = ts_a->tv_sec - ts_b->tv_sec;
	ts_out->tv_nsec = ts_a->tv_nsec - ts_b->tv_nsec;
	if (ts_out->tv_nsec < 0) {
		ts_out->tv_nsec += 1000000000;
		ts_out->tv_sec -= 1;
	}

	assert(ts_out->tv_sec >= 0);
	assert(ts_out->tv_nsec >= 0);
	assert(ts_out->tv_nsec <= 999999999);

	return ts_out;
}

/* test if ts_a is less than or equal to ts_b */
static inline int timespec_is_le(const struct timespec *ts_a, const struct timespec *ts_b)
{
	assert(ts_a != NULL);
	assert(ts_a->tv_sec >= 0);
	assert(ts_a->tv_nsec >= 0);
	assert(ts_a->tv_nsec < 1000000000);
	assert(ts_b != NULL);
	assert(ts_b->tv_sec >= 0);
	assert(ts_b->tv_nsec >= 0);
	assert(ts_b->tv_nsec < 1000000000);

	return ((ts_a->tv_sec < ts_b->tv_sec) ||
	        ((ts_a->tv_sec == ts_b->tv_sec) && (ts_a->tv_nsec <= ts_b->tv_nsec)));
}

////////////////////////////////////////////////////////////////////////////////

/* conversion from MB/s to MiB/s */
static inline double to_mib(double val)
{
	return val * (1000 * 1000) / (1024 * 1024);
}

////////////////////////////////////////////////////////////////////////////////

/* flush cacheline, without any fences */
static inline void flush_cacheline(void *addr)
{
#if defined __aarch64__
	__asm__ volatile ("dc CIVAC, %0" : : "r"(addr) : "memory");
#elif defined __riscv
	/* FIXME: RISC-V has nothing usable yet */
	(void)addr;
#elif defined __x86_64__
	__asm__ volatile ("clflush %0" : : "m" (*(long*)addr) : "memory");
#endif
}

static inline void memory_barrier(void)
{
#if defined __aarch64__
	__asm__ volatile ("dmb sy" : : : "memory");
#elif defined __riscv
	__asm__ volatile ("fence rw,rw" : : : "memory");
#elif defined __x86_64__
	__asm__ volatile ("mfence" : : : "memory");
#endif
}

static inline void flush_cacheline_all(void)
{
	char *ptr = map_addr;
	const char *end = map_addr_end;

	for (; ptr < end; ptr += CACHELINE_SIZE) {
		flush_cacheline(ptr);
	}
	memory_barrier();
}

////////////////////////////////////////////////////////////////////////////////

/* generate a set of non-inlined benchmark loops */
#define BENCH_CACHELINE(name)	\
	static void name ## _linear(void)	\
	{	\
		char *ptr = map_addr;	\
		char *end = map_addr_end;	\
		for (; ptr < end; ptr += CACHELINE_SIZE) {	\
			name(ptr);	\
		}	\
	}	\
	static void name ## _step(size_t step)	\
	{	\
		char *ptr = map_addr;	\
		char *end = map_addr_end;	\
		for (; ptr < end; ptr += step) {	\
			name(ptr);	\
		}	\
	}

#define BENCH_NAMES(name)	\
	.bench_linear = name ## _linear,	\
	.bench_step = name ## _step

////////////////////////////////////////////////////////////////////////////////

/* read a cache line using a load of one word */
static inline void read_cacheline(void *addr)
{
	unsigned long *p = addr;
	unsigned long tmp;
	/* the compiler barriers prevent reordering */
	__asm__ volatile ("" : : : "memory");
	tmp = p[0];
	/* consume tmp */
	__asm__ volatile ("" : : "r"(tmp) : "memory");
}

BENCH_CACHELINE(read_cacheline)

////////////////////////////////////////////////////////////////////////////////

#if defined __aarch64__

/* load a complete cache line using LDNP instructions */
static inline void ldnp_cacheline(void *addr)
{
	unsigned long long tmp1, tmp2;

	__asm__ volatile (
		"ldnp %0, %1, [%2, #0]\n"
		: "=&r"(tmp1), "=&r"(tmp2) : "r"(addr) : "memory");
}

BENCH_CACHELINE(ldnp_cacheline)

#endif

////////////////////////////////////////////////////////////////////////////////

/* write to a complete cache line using stores */
static inline void write_cacheline(void *addr)
{
#if defined __aarch64__ || defined __x86_64__
	__uint128_t *p = addr;
	p[0] = 0;
	p[1] = 0;
	p[2] = 0;
	p[3] = 0;
#else
	unsigned long long *p = addr;
	p[0] = 0;
	p[1] = 0;
	p[2] = 0;
	p[3] = 0;
#if CACHELINE_SIZE > 32
	p[4] = 0;
	p[5] = 0;
	p[6] = 0;
	p[7] = 0;
#endif
#endif
}

BENCH_CACHELINE(write_cacheline)

////////////////////////////////////////////////////////////////////////////////

#if defined __aarch64__

/* write to a complete cache line using DC ZVA instructions */
static inline void dczva_cacheline(void *addr)
{
	__asm__ volatile ("dc zva, %0" : : "r"(addr) : "memory");
}

BENCH_CACHELINE(dczva_cacheline)

#endif

////////////////////////////////////////////////////////////////////////////////

#if defined __aarch64__

/* write to a complete cache line using STNP instructions */
static inline void stnp_cacheline(void *addr)
{
	__asm__ volatile (
		"stnp x0, x1, [%0, #0]\n"
		"stnp x2, x3, [%0, #16]\n"
		"stnp x4, x5, [%0, #32]\n"
		"stnp x6, x7, [%0, #48]\n"
		: : "r"(addr) : "memory");
}

BENCH_CACHELINE(stnp_cacheline)

#endif

////////////////////////////////////////////////////////////////////////////////

/* modify a single word in a cache line, effectively a read-modify-write */
static inline void modify_cacheline(void *addr)
{
	unsigned long *p = addr;

	/* the compiler barriers prevent reordering */
	__asm__ volatile ("" : : : "memory");
	p[0] = (unsigned long) addr;
	__asm__ volatile ("" : : : "memory");
}

BENCH_CACHELINE(modify_cacheline)

////////////////////////////////////////////////////////////////////////////////

#if defined __aarch64__
/* modify a single word in a cache line, effectively a read-modify-write */
static inline void prefetch_modify_cacheline(void *addr)
{
	unsigned long *p = addr;

	/* a look-ahead of 3 cache lines yields the highest performance on A53 */
	/* prefetch cacheline to L1 cache */
	__asm__ volatile ("prfm pstl1keep, [%0]" : : "r"(p + 3*CACHELINE_SIZE) : "memory");

	/* the compiler barriers prevent reordering */
	__asm__ volatile ("" : : : "memory");
	p[0] = (unsigned long) addr;
	__asm__ volatile ("" : : : "memory");
}

BENCH_CACHELINE(prefetch_modify_cacheline)

#endif

////////////////////////////////////////////////////////////////////////////////

#if defined __aarch64__

/* write to a part of a cache line using STNP instructions */
static inline void stnp_modify_cacheline(void *addr)
{
	__asm__ volatile (
		"stnp x0, x1, [%0, #0]\n"
		: : "r"(addr) : "memory");
}

BENCH_CACHELINE(stnp_modify_cacheline)

#endif

////////////////////////////////////////////////////////////////////////////////

#if defined __aarch64__ || defined __x86_64__

/* prefetch a full cacheline to L1 for reading */
static inline void prefetch_l1_cacheline(void *addr)
{
#if defined __aarch64__
	/* prefetch cacheline to L1 cache */
	__asm__ volatile ("prfm pldl1keep, [%0]" : : "r"(addr) : "memory");
#else
	/* prefetch cacheline to L1/L2/L3 cache */
	__asm__ volatile ("prefetcht0 %0" : : "m"(*(char*)addr) : "memory");
#endif
}

BENCH_CACHELINE(prefetch_l1_cacheline)

#endif

////////////////////////////////////////////////////////////////////////////////

#if defined __aarch64__ || defined __x86_64__

/* prefetch a full cacheline to L1 for writing */
static inline void prefetch_l1w_cacheline(void *addr)
{
#if defined __aarch64__
	/* prefetch cacheline to L1 cache for store */
	__asm__ volatile ("prfm pstl1keep, [%0]" : : "r"(addr) : "memory");
#else
	/* prefetch cacheline to L1/L2/L3 cache for store */
	__asm__ volatile ("prefetchw %0" : : "m"(*(char*)addr) : "memory");
#endif
}

BENCH_CACHELINE(prefetch_l1w_cacheline)

#endif

////////////////////////////////////////////////////////////////////////////////

#if defined __aarch64__

/* prefetch a full cacheline to L1 for writing in streaming mode */
static inline void prefetch_l1ws_cacheline(void *addr)
{
	/* prefetch cacheline to L1 cache for store */
	__asm__ volatile ("prfm pstl1strm, [%0]" : : "r"(addr) : "memory");
}

BENCH_CACHELINE(prefetch_l1ws_cacheline)

#endif

////////////////////////////////////////////////////////////////////////////////

#if defined __aarch64__ || defined __x86_64__

/* prefetch a full cacheline to L2 for reading */
static inline void prefetch_l2_cacheline(void *addr)
{
#if defined __aarch64__
	/* prefetch cacheline to L2 cache */
	__asm__ volatile ("prfm pldl2keep, [%0]" : : "r"(addr) : "memory");
#else
	/* prefetch cacheline to L2/L3 cache */
	__asm__ volatile ("prefetcht1 %0" : : "m"(*(char*)addr) : "memory");
#endif
}

BENCH_CACHELINE(prefetch_l2_cacheline)

#endif

////////////////////////////////////////////////////////////////////////////////

#if defined __aarch64__

/* prefetch a full cacheline to L2 for writing */
static inline void prefetch_l2w_cacheline(void *addr)
{
	/* prefetch cacheline to L2 cache */
	__asm__ volatile ("prfm pstl2keep, [%0]" : : "r"(addr) : "memory");
}

BENCH_CACHELINE(prefetch_l2w_cacheline)

#endif

////////////////////////////////////////////////////////////////////////////////

#if defined __aarch64__

/* prefetch a full cacheline to L2 for writing in streaming mode */
static inline void prefetch_l2ws_cacheline(void *addr)
{
	/* prefetch cacheline to L2 cache */
	__asm__ volatile ("prfm pstl2strm, [%0]" : : "r"(addr) : "memory");
}

BENCH_CACHELINE(prefetch_l2ws_cacheline)

#endif

////////////////////////////////////////////////////////////////////////////////

#if defined __aarch64__ || defined __x86_64__

/* prefetch a full cacheline to L3 for reading */
static inline void prefetch_l3_cacheline(void *addr)
{
#if defined __aarch64__
	/* prefetch cacheline to L3 cache */
	__asm__ volatile ("prfm pldl3keep, [%0]" : : "r"(addr) : "memory");
#else
	/* prefetch cacheline to L3 cache */
	__asm__ volatile ("prefetcht2 %0" : : "m"(*(char*)addr) : "memory");
#endif
}

BENCH_CACHELINE(prefetch_l3_cacheline)

#endif

////////////////////////////////////////////////////////////////////////////////

#if defined __aarch64__

/* prefetch a full cacheline to L3 for writing */
static inline void prefetch_l3w_cacheline(void *addr)
{
	/* prefetch cacheline to L3 cache */
	__asm__ volatile ("prfm pstl3keep, [%0]" : : "r"(addr) : "memory");
}

BENCH_CACHELINE(prefetch_l3w_cacheline)

#endif

////////////////////////////////////////////////////////////////////////////////

#if defined __aarch64__

/* prefetch a full cacheline to L3 for writing in streaming mode */
static inline void prefetch_l3ws_cacheline(void *addr)
{
	/* prefetch cacheline to L3 cache */
	__asm__ volatile ("prfm pstl3strm, [%0]" : : "r"(addr) : "memory");
}

BENCH_CACHELINE(prefetch_l3ws_cacheline)

#endif

////////////////////////////////////////////////////////////////////////////////

/* test cases */
static struct test {
	const char *name;
	void (*bench_linear)(void);
	void (*bench_step)(size_t);
	const char *desc;
} tests[] = {
	{	.name = "read", BENCH_NAMES(read_cacheline), .desc = "read cacheline",	},
#if defined __aarch64__
	{	.name = "read_ldnp", BENCH_NAMES(ldnp_cacheline), .desc = "read cacheline using LDNP (Arm)",	},
#endif
	{	.name = "write", BENCH_NAMES(write_cacheline), .desc = "write full cacheline",	},
#if defined __aarch64__
	{	.name = "write_dczva", BENCH_NAMES(dczva_cacheline), .desc = "write full cacheline using DC ZVA (Arm)",	},
	{	.name = "write_stnp", BENCH_NAMES(stnp_cacheline), .desc = "write full cacheline using STNP (Arm)",	},
#endif
	{	.name = "modify", BENCH_NAMES(modify_cacheline), .desc = "modify cacheline",	},
#if defined __aarch64__
	{	.name = "modify_prefetch", BENCH_NAMES(prefetch_modify_cacheline), .desc = "modify cacheline with prefetching (Arm)",	},
	{	.name = "modify_stnp", BENCH_NAMES(stnp_modify_cacheline), .desc = "modify cacheline using STNP (Arm)",	},
#endif
#if defined __aarch64__ || defined __x86_64__
	{	.name = "prefetch_l1", BENCH_NAMES(prefetch_l1_cacheline), .desc = "prefetch cacheline to L1 for reading",	},
	{	.name = "prefetch_l1w", BENCH_NAMES(prefetch_l1w_cacheline), .desc = "prefetch cacheline to L1 for writing",	},
#endif
#if defined __aarch64__
	{	.name = "prefetch_l1ws", BENCH_NAMES(prefetch_l1ws_cacheline), .desc = "prefetch cacheline to L1 for writing in streaming mode (Arm)",	},
#endif
#if defined __aarch64__ || defined __x86_64__
	{	.name = "prefetch_l2", BENCH_NAMES(prefetch_l2_cacheline), .desc = "prefetch cacheline to L2 for reading",	},
#endif
#if defined __aarch64__
	{	.name = "prefetch_l2w", BENCH_NAMES(prefetch_l2w_cacheline), .desc = "prefetch cacheline to L3 for writing (Arm)",	},
	{	.name = "prefetch_l2ws", BENCH_NAMES(prefetch_l2ws_cacheline), .desc = "prefetch cacheline to L2 for writing in streaming mode (Arm)",	},
#endif
#if defined __aarch64__ || defined __x86_64__
	{	.name = "prefetch_l3", BENCH_NAMES(prefetch_l3_cacheline), .desc = "prefetch cacheline to L3 for reading",	},
#endif
#if defined __aarch64__
	{	.name = "prefetch_l3w", BENCH_NAMES(prefetch_l3w_cacheline), .desc = "prefetch cacheline to L3 for writing (Arm)",	},
	{	.name = "prefetch_l3ws", BENCH_NAMES(prefetch_l3ws_cacheline), .desc = "prefetch cacheline to L3 for writing in streaming mode (Arm)",	},
#endif
	{	.name = NULL, .desc = NULL,	},
};

static void bench_linear(const char *name, void (*bench)(void))
{
	struct timespec ts_now, ts_start, ts_end, ts_tmp;
	unsigned long long bytes_accessed;
	unsigned long long delta_t_ns;
	unsigned long long sum = 0;
	unsigned long long runs;
	int loops;
	double bw;

	printf("linear %s bandwidth over %zu MiB block", name, map_size / 1024 / 1024);
	if (map_huge != 0) {
		printf(" (huge TLB)");
	}
	printf("\n");

	flush_cacheline_all();

	clock_gettime(CLOCK_MONOTONIC, &ts_start);
	ts_end = ts_start;
	timespec_inc_by(&ts_end, option_print_delay_ms * 1000000ull);
	runs = 0;
	loops = 0;

	perf_read(perf_prev_values); //

	while (1) {
		bench();
		runs++;

		clock_gettime(CLOCK_MONOTONIC, &ts_now);
		if (!timespec_is_le(&ts_now, &ts_end)) {
			perf_read(perf_curr_values); //
			perf_delta(perf_curr_values, perf_prev_values, perf_delta_values); //

			bytes_accessed = runs * map_size;
			delta_t_ns = timespec_ns(timespec_sub(&ts_now, &ts_start, &ts_tmp));
			bw = 1000.0 * bytes_accessed / delta_t_ns;

			printf("%.1f MiB/s, %.1f MB/s", to_mib(bw), bw); //
#if NUM_PERF > 0
			if (perf_ok) {
#if NUM_PERF == 1 // if perf
				printf(", perf: %.1f MB/s", 1000.0 * perf_delta_values[0] * CACHELINE_SIZE / delta_t_ns);
#else
				char delim = ' ';
				sum = 0;

				printf(", perf:");
				for (unsigned int i = 0; i < NUM_PERF; i++) {
					printf("%c%.1f", delim, 1000.0 * perf_delta_values[i] * CACHELINE_SIZE / delta_t_ns);
					sum += perf_delta_values[i];
					delim = '+';
				}
				printf("=%.1f MB/s", 1000.0 * sum * CACHELINE_SIZE / delta_t_ns);
#endif
			}
#endif
			printf("\n");

			if (csv_file != NULL) {
				fprintf(csv_file, "%s;%d;%llu;%llu;%llu\n",
				        name, CACHELINE_SIZE, delta_t_ns,
				        bytes_accessed, sum * CACHELINE_SIZE);
				fflush(csv_file);
			}

			loops++;
			if (loops == option_num_loops) {
				break;
			}

			clock_gettime(CLOCK_MONOTONIC, &ts_start);
			ts_end = ts_start;
			timespec_inc_by(&ts_end, option_print_delay_ms * 1000000ull);
			runs = 0;

			perf_read(perf_prev_values);
		}
	}
}

static void bench_step(const char *name, void (*bench)(size_t), size_t step)
{
	struct timespec ts_now, ts_start, ts_end, ts_tmp;
	unsigned long long bytes_accessed;
	unsigned long long delta_t_ns;
	unsigned long long sum = 0;
	unsigned long long runs;
	int loops;
	double bw;

	printf("step1 %s bandwidth over %zu MiB block, step %zu", name, map_size / 1024 / 1024, step);
	if (map_huge != 0) {
		printf(" (huge TLB)");
	}
	printf("\n");

	flush_cacheline_all();

	clock_gettime(CLOCK_MONOTONIC, &ts_start);
	ts_end = ts_start;
	timespec_inc_by(&ts_end, option_print_delay_ms * 1000000ull);
	runs = 0;
	loops = 0;

	perf_read(perf_prev_values);

	while (1) {
		bench(step);
		runs++;

		clock_gettime(CLOCK_MONOTONIC, &ts_now);
		if (!timespec_is_le(&ts_now, &ts_end)) {
			perf_read(perf_curr_values);
			perf_delta(perf_curr_values, perf_prev_values, perf_delta_values);

			bytes_accessed = runs * map_size * CACHELINE_SIZE / step;
			delta_t_ns = timespec_ns(timespec_sub(&ts_now, &ts_start, &ts_tmp));
			bw = 1000.0 * bytes_accessed / delta_t_ns;

			printf("%.1f MiB/s, %.1f MB/s", to_mib(bw), bw);
#if NUM_PERF > 0
			if (perf_ok) {
#if NUM_PERF == 1
				printf(", perf: %.1f MB/s", 1000.0 * perf_delta_values[0] * CACHELINE_SIZE / delta_t_ns);
#else
				char delim = ' ';
				sum = 0;

				printf(", perf:");
				for (unsigned int i = 0; i < NUM_PERF; i++) {
					printf("%c%.1f", delim, 1000.0 * perf_delta_values[i] * CACHELINE_SIZE / delta_t_ns);
					sum += perf_delta_values[i];
					delim = '+';
				}
				printf("=%.1f MB/s", 1000.0 * sum * CACHELINE_SIZE / delta_t_ns);
#endif
			}
#endif
			printf("\n");

			if (csv_file != NULL) {
				fprintf(csv_file, "%s;%zu;%llu;%llu;%llu\n",
				        name, step, delta_t_ns,
				        bytes_accessed, sum * CACHELINE_SIZE);
				fflush(csv_file);
			}

			loops++;
			if (loops == option_num_loops) {
				break;
			}

			clock_gettime(CLOCK_MONOTONIC, &ts_start);
			ts_end = ts_start;
			timespec_inc_by(&ts_end, option_print_delay_ms * 1000000ull);
			runs = 0;

			perf_read(perf_prev_values);
		}
	}
}

static void bench_auto(const char *name, void (*bench)(size_t))
{
	struct timespec ts_now, ts_start, ts_end, ts_tmp;
	unsigned long long bytes_accessed;
	unsigned long long delta_t_ns;
	unsigned long long sum = 0;
	unsigned long long runs;
	size_t step, min_step;
	double min_step_bw;
	double bw;

	printf("worst-case %s bandwidth over %zu MiB block", name, map_size / 1024 / 1024);
	if (map_huge != 0) {
		printf(" (huge TLB)");
	}
	printf("\n");

	flush_cacheline_all();

	/* part 1 */

	min_step = -1;
	min_step_bw = 1e30;	/* very high */

	clock_gettime(CLOCK_MONOTONIC, &ts_start);
	ts_end = ts_start;
	timespec_inc_by(&ts_end, option_print_delay_ms * 1000000ull);
	runs = 0;

	perf_read(perf_prev_values);

	step = CACHELINE_SIZE;
	while (1) {
		bench(step);
		runs++;

		clock_gettime(CLOCK_MONOTONIC, &ts_now);
		if (!timespec_is_le(&ts_now, &ts_end)) {
			perf_read(perf_curr_values);
			perf_delta(perf_curr_values, perf_prev_values, perf_delta_values);

			bytes_accessed = runs * map_size * CACHELINE_SIZE / step;
			delta_t_ns = timespec_ns(timespec_sub(&ts_now, &ts_start, &ts_tmp));
			bw = 1000.0 * bytes_accessed / delta_t_ns;

			printf("step %zu: ", step);
			printf("%.1f MiB/s, %.1f MB/s", to_mib(bw), bw);
#if NUM_PERF > 0
			if (perf_ok) {
#if NUM_PERF == 1
				printf(", perf: %.1f MB/s", 1000.0 * perf_delta_values[0] * CACHELINE_SIZE / delta_t_ns);
#else
				char delim = ' ';
				sum = 0;

				printf(", perf:");
				for (unsigned int i = 0; i < NUM_PERF; i++) {
					printf("%c%.1f", delim, 1000.0 * perf_delta_values[i] * CACHELINE_SIZE / delta_t_ns);
					sum += perf_delta_values[i];
					delim = '+';
				}
				printf("=%.1f MB/s", 1000.0 * sum * CACHELINE_SIZE / delta_t_ns);
#endif
			}
#endif
			printf("\n");

			if (csv_file != NULL) {
				fprintf(csv_file, "%s;%zu;%llu;%llu;%llu\n",
				        name, step, delta_t_ns,
				        bytes_accessed, sum * CACHELINE_SIZE);
				fflush(csv_file);
			}

			if (bw < min_step_bw) {
				min_step_bw = bw;
				min_step = step;
			}
			step *= 2;
			if (step >= map_size / 8) {
				break;
			}

			clock_gettime(CLOCK_MONOTONIC, &ts_start);
			ts_end = ts_start;
			timespec_inc_by(&ts_end, option_print_delay_ms * 1000000ull);
			runs = 0;

			perf_read(perf_prev_values);
		}
	}

	printf("slowest step size: %zu\n", min_step);
}

/* map memory, preferrably using huge TLBs  */
static void *map(size_t size)
{
	char *addr;
	int flags;
	int flags_huge;

	flags = MAP_PRIVATE | MAP_ANONYMOUS;
	flags_huge = MAP_HUGETLB | MAP_HUGE_2MB;
	if (option_huge) {
		flags |= flags_huge;
		map_huge = 1;
	}

again:
	addr = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, -1, 0);
	if (addr == MAP_FAILED) {
		/* if huge TLBs fail, try again with normal pages */
		if ((flags & MAP_HUGETLB) != 0) {
			if (errno == ENOMEM) {
				printf("# mapping memory as huge TLBs failed, this impacts results due to TLB misses\n"
					   "# increase number of huge TLBs: $ sudo sysctl -w vm.nr_hugepages=%zu\n",
					   (size + 0x200000 - 1) / 0x200000);
			}
			flags &= ~flags_huge;
			map_huge = 0;
			goto again;
		}

		perror("mmap");
		exit(EXIT_FAILURE);
	}

	return addr;
}

static void version(FILE *f)
{
	fprintf(f, PROGNAME " build " BUILDID
#ifndef NDEBUG
	        " DEBUG"
#endif
	        "\n");
}

static void usage(FILE *f)
{
	struct test *t;

	version(f);
	fprintf(f, "usage: " PROGNAME " [<options>] <test>\n\n"
	        "Platform memory benchmarks.\n\n"
	        "Options:\n"
	        "  -s|--size <size>  memory size in MiB (default %d MiB)\n"
	        "  -c|--cpu <cpu>    run on given CPU ID (default any)\n"
	        "  -p|--prio <prio>  run at given priority (default current)\n"
	        "  -l|--loops <num>  stop after given number of loops (default run infinitely)\n"
	        "  -d|--delay <ms>   print bandwidth after given ms (default %d ms)\n"
	        "  --huge            enable huge pages\n"
	        "  --perf            enable perf tracing\n"
	        "  --step <bytes>    access memory with given step in bytes\n"
	        "  --auto            auto-detect worst-case memory access\n"
	        "  --all             run all tests (don't specify a test, -l 1 set implicitly)\n"
	        "  --csv <file>      export data as CSV to file\n"
	        "  --csv-no-header   do not print a header in the CSV file\n"
	        "  --version         print version info\n"
	        "  --help            show usage\n"
	        "Tests:\n",
	        DEFAULT_MB, option_print_delay_ms);
	for (t = tests; t->name != NULL; t++) {
		fprintf(f, "  %-18s%s\n", t->name, t->desc != NULL ? t->desc : "");
	}
}

int main(int argc, char *argv[])
{
	struct test *t = tests;
	const char *mode_str = NULL;
	const char *size_str = NULL;
	const char *cpu_str = NULL;
	const char *prio_str = NULL;
	const char *step_str = NULL;
	const char *loop_str = NULL;
	const char *delay_str = NULL;
	const char *csv_file_str = NULL;
	unsigned int step_size = CACHELINE_SIZE;
	unsigned int mb = DEFAULT_MB;
	int arg;

	for (arg = 1; arg < argc; arg++) {
		if (argv[arg][0] != '-') {
			if (mode_str != NULL) {
				usage(stderr);
				return EXIT_FAILURE;
			}
			mode_str = argv[arg];
			continue;
		}

		if (!strcmp(argv[arg], "--version")) {
			version(stdout);
			return EXIT_SUCCESS;
		} else if (!strcmp(argv[arg], "--help")) {
			usage(stdout);
			return EXIT_SUCCESS;
		} else if (!strcmp(argv[arg], "--size") || !strcmp(argv[arg], "-s")) {
			arg++;
			size_str = argv[arg];
		} else if (!strcmp(argv[arg], "--cpu") || !strcmp(argv[arg], "-c")) {
			arg++;
			cpu_str = argv[arg];
		} else if (!strcmp(argv[arg], "--prio") || !strcmp(argv[arg], "-p")) {
			arg++;
			prio_str = argv[arg];
		} else if (!strcmp(argv[arg], "--perf")) {
			option_perf = 1;
		} else if (!strcmp(argv[arg], "--huge")) {
			option_huge = 1;
		} else if (!strcmp(argv[arg], "--step")) {
			option_step = 1;
			arg++;
			step_str = argv[arg];
		} else if (!strcmp(argv[arg], "--loops") || !strcmp(argv[arg], "-l")) {
			arg++;
			loop_str = argv[arg];
		} else if (!strcmp(argv[arg], "--delay") || !strcmp(argv[arg], "-d")) {
			arg++;
			delay_str = argv[arg];
		} else if (!strcmp(argv[arg], "--auto")) {
			option_step = 2;
		} else if (!strcmp(argv[arg], "--all")) {
			option_all = 1;
			/* changes the number of loops to 1 by default */
			option_num_loops = 1;
		} else if (!strcmp(argv[arg], "--csv")) {
			arg++;
			csv_file_str = argv[arg];
		} else if (!strcmp(argv[arg], "--csv-no-header")) {
			option_csv_no_header = 1;
		} else {
			fprintf(stderr, "unknown option '%s'\n", argv[arg]);
			usage(stderr);
			return EXIT_FAILURE;
		}
	}

	if ((arg != argc) || ((option_all == 0) && (mode_str == NULL))
	                  || ((option_all != 0) && (mode_str != NULL))) {
		usage(stderr);
		return EXIT_FAILURE;
	}

	/* check mode */
	if (option_all == 0) {
		for (t = tests; t->name != NULL; t++) {
			if (strcmp(mode_str, t->name) == 0) {
				break;
			}
		}
		if (t->name == NULL) {
			fprintf(stderr, "error: invalid test\n");
			usage(stderr);
			return EXIT_FAILURE;
		}
	}

	/* check CPU */
	if (cpu_str != NULL) {
		cpu_set_t cpuset = { 0 };
		int cpu_id;
		int err;

		cpu_id = atoi(cpu_str);
		CPU_SET(cpu_id, &cpuset);
		err = sched_setaffinity(0, sizeof(cpuset), &cpuset);
		if (err != 0) {
			perror("sched_setaffinity");
			exit(EXIT_FAILURE);
		}
	}

	/* check prio */
	if (prio_str != NULL) {
		struct sched_param param = { 0 };
		int prio;
		int err;

		prio = atoi(prio_str);
		param.sched_priority = prio;
		err = sched_setscheduler(0, prio > 0 ? SCHED_FIFO : SCHED_OTHER, &param);
		if (err != 0) {
			perror("sched_setscheduler");
			exit(EXIT_FAILURE);
		}
	}

	/* check size */
	if (size_str != NULL) {
		mb = atoi(size_str);
		if (mb == 0) {
			fprintf(stderr, "error: invalid memory size in MiB\n");
			exit(EXIT_FAILURE);
		}
	}

	/* check step size */
	if (step_str != NULL) {
		step_size = atoi(step_str);
		if (step_size < CACHELINE_SIZE || ((step_size & (step_size - 1)) != 0)) {
			fprintf(stderr, "error: invalid step size, must be >=%d and power of two\n", CACHELINE_SIZE);
			exit(EXIT_FAILURE);
		}
	}
	if (step_size >= mb * 1024 * 1024) {
		fprintf(stderr, "error: invalid step size, must be < memory size\n");
		exit(EXIT_FAILURE);
	}

	/* check loops */
	if (loop_str != NULL) {
		option_num_loops = atoi(loop_str);
		if (option_num_loops < 0) {
			fprintf(stderr, "error: negative number of loops\n");
			exit(EXIT_FAILURE);
		}
	}

	/* check delay */
	if (delay_str != NULL) {
		option_print_delay_ms = atoi(delay_str);
		if (option_print_delay_ms <= 0) {
			fprintf(stderr, "error: invalid delay\n");
			exit(EXIT_FAILURE);
		}
	}

	map_size = mb * 1024 * 1024;
	map_addr = map(map_size);
	map_addr_end = &map_addr[map_size];
	/* prefault memory */
	memset(map_addr, 0x5a, map_size);

	if (option_perf) {
		perf_open();
	}

	/* open CSV file */
	if (csv_file_str != NULL) {
		csv_file = fopen(csv_file_str, "w+");
		if (csv_file == NULL) {
			perror("fopen");
			exit(EXIT_FAILURE);
		}
		if (!option_csv_no_header) {
			fprintf(csv_file, "#test;step;time_nanoseconds;bytes_accessed;bytes_perf\n");
			fflush(csv_file);
		}
	}

	if (option_step == 0) {
		if (option_all != 0) {
			for (t = tests; t->name != NULL; t++) {
				bench_linear(t->name, t->bench_linear);
			}
		} else {
			assert(t->name != NULL);
			bench_linear(t->name, t->bench_linear);
		}
	} else if (option_step == 1) {
		if (option_all != 0) {
			for (t = tests; t->name != NULL; t++) {
				bench_step(t->name, t->bench_step, step_size);
			}
		} else {
			assert(t->name != NULL);
			bench_step(t->name, t->bench_step, step_size);
		}
	} else if (option_step == 2) {
		if (option_all != 0) {
			for (t = tests; t->name != NULL; t++) {
				bench_auto(t->name, t->bench_step);
			}
		} else {
			assert(t->name != NULL);
			bench_auto(t->name, t->bench_step);
		}
	}

	if (csv_file != NULL) {
		fclose(csv_file);
	}

	return EXIT_SUCCESS;
}
