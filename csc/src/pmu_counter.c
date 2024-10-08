#include "pmu_counter.h"
#include <errno.h>
#include <stdio.h>
#include <unistd.h>


int perf_ok = 0;

#define __NR_perf_event_open 241 // for __aarch64__

/* perf_event_open() system call, not exported in libc */
int perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu, int group_fd, unsigned long flags)
{
	return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

int perf_open(int event_num, PmuEvent *pmu_events, int *perf_fds)
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

	for (ctr = 0; ctr < event_num; ctr++) { // for every events
		/* 1st PMU counter is group leader */
		if (ctr == 0) {
			group_fd = -1;	/* first in group */
		} else {
			group_fd = perf_fds[0];
		}
		// attr.config = perf_config[ctr];
		attr.config = pmu_events[ctr].number; // number

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


int perf_read(unsigned long long *values, int event_num, int *perf_fds)
{
	unsigned long long buf[event_num + 2];
	int group_fd;
	ssize_t exp;
	ssize_t r;
	int err;

	if (!perf_ok) {
		for (int i = 0; i < event_num; i++) {
			values[i] = 0;
		}
		return 0;
	}

	/* We except to read one more 64-bit values than counters,
	 * the first value is the number of counters.
	 * The buffer has space for two more values to detect format issues.
	 */
	exp = (event_num + 1) * sizeof(buf[0]);

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
	if (buf[0] != event_num) {
		fprintf(stderr, "perf_trace: unexpected value %llu\n", buf[0]);
		return EINVAL;
	}

	for (int i = 0; i < event_num; i++) {
		values[i] = buf[i+1];
	}

	return 0;
}


unsigned long long *perf_delta(const unsigned long long *curr_values, const unsigned long long *prev_values, unsigned long long *delta_values, int event_num)
{
	for (int i = 0; i < event_num; i++) {
		delta_values[i] = curr_values[i] - prev_values[i];
	}

	return delta_values;
}