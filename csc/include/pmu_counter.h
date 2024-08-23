#ifndef PMU_COUNTER_H
#define PMU_COUNTER_H

#include <sys/types.h>
#include <stdint.h>
#include <linux/perf_event.h>

#if defined __aarch64__
#define NUM_PERF 2
#elif defined __riscv
#define NUM_PERF 0
#elif defined __x86_64__
#define NUM_PERF 1
#endif

static const unsigned long long perf_config[NUM_PERF];
extern int perf_fds[NUM_PERF];
extern unsigned long long perf_prev_values[NUM_PERF];
extern unsigned long long perf_curr_values[NUM_PERF];
extern unsigned long long perf_delta_values[NUM_PERF];
extern int perf_ok;

int perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu, int group_fd, unsigned long flags);
int perf_open(void);
int perf_read(unsigned long long *values);
unsigned long long *perf_delta(const unsigned long long *curr_values, const unsigned long long *prev_values, unsigned long long *delta_values);

#endif /* PMU_COUNTER_H */