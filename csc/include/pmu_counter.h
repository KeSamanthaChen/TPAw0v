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

#define MAX_EVENT_NAME 50

typedef struct {
    char name[MAX_EVENT_NAME];
    int number;
} PmuEvent;

extern int perf_ok;

int perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu, int group_fd, unsigned long flags);
int perf_open(int event_num, PmuEvent *pmu_events, int *perf_fds);
int perf_read(unsigned long long *values, int event_num, int *perf_fds);
unsigned long long *perf_delta(const unsigned long long *curr_values, const unsigned long long *prev_values, unsigned long long *delta_values, int event_num);

#endif /* PMU_COUNTER_H */