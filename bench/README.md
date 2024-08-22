# Memory Performance Benchmark

This benchmark for Linux allows to quickly assess a platform's memory subsystem.
The benchmark lets the CPU emit an endless stream of memory transaction
and prints the current bandwidth each second, including PMU counters.

The benchmark iterates on a memory buffer of given size cacheline by cacheline
in an architecture-specific way by using *read*, *write* or *modify* operations:
- *Read operations* include the CPU's `load` and `prefetch` instructions.
  These stress the read performance of the memory subsystem.
  Using `prefetch` instructions can often achieve higher performance/stress,
  as they do not block the CPU's pipeline.
  The cacheline remains in clean (non-dirty) state. Prefetches for writing
  additionally claims exclusive ownership of the cachelines (not shared).
- *Write operations* always write to the whole cachelines, preventing
  the CPU to first fetch a cacheline from memory before overwriting its content.
  This stresses the write performance of the memory subsystem.
  If supported, `clean cacheline` instructions can be used.
- *Modify operations* change a part of each cacheline.
  The CPU has to read the cacheline from memory and eventually write it back.
  Running this in a loop creates a stream of alternating read and write
  transactions to the memory subsystem.

Another specialty is to evaluate the platforms sustainable memory bandwidth.
This is the memory bandwidth that the memory controller can sustain
under worst-case memory workload, e.g. due to row misses in the same DRAM bank.
The benchmark tries to find worst-case memory access patterns automatically.

The benchmark is licensed under MIT license, see [LICENSE.TXT](LICENSE.TXT).
Copyright (c) 2023-2024 Alexander Zuepke.


## Supported Architectures
- 64-bit ARM: Cortex-A53, A57, A72
- 64-bit x86: Intel Core and Atom
- 64-bit RISC-V: any rv64g, but no PMU counters supported yet


## Compile

To compile the benchmark for x86, just type:
```
$ make
```

For cross compilation to Arm, use:
```
$ CROSS=aarch64-linux-gnu- make
```

For cross compilation to RISC-V, use:
```
$ CROSS=riscv64-linux-gnu- make
```


## Usage

For a *read*, *write* or *modify* test, simply pass `read`, `write` or `modify`
to the bench:
```
$ ./bench read
```
The benchmark allocates a memory buffer and starts reading.

Calling the benchmark without any parameters lists the available test cases:
```
$ ./bench
bench build local
usage: bench [<options>] <test>

Platform memory benchmarks.

Options:
  -s|--size <size>  memory size in MiB (default 16 MiB)
  -c|--cpu <cpu>    run on given CPU ID (default any)
  -p|--prio <prio>  run at given priority (default current)
  -l|--loops <num>  stop after given number of loops (default run infinitely)
  -d|--delay <ms>   print bandwidth after given ms (default 1000 ms)
  --huge            enable huge pages
  --perf            enable perf tracing
  --step <bytes>    access memory with given step in bytes
  --auto            auto-detect worst-case memory access
  --all             run all tests (don't specify a test then)
  --csv <file>      export data as CSV to file
  --csv-no-header   do not print a header in the CSV file
  --version         print version info
  --help            show usage
Tests:
  read              read cacheline
  write             write full cacheline
  modify            modify cacheline
```

The memory size can be adjusted with the `-s` parameter,
for example `-s 64` let the benchmark use a 64 MiB memory region.

The benchmark can be pinned to a specific CPU using
either the Linux `taskset` utility or the `-c <cpu>` parameter.

Likewise, `-p <priority>` allows to select a real-time scheduling priority level
other than Linux default scheduling with `SCHED_OTHER` at priority level 0.
Linux supports priorities from 1 to 99 with `SCHED_FIFO` real-time scheduling.
Be aware that most Linux distributions configure throttling of real-time tasks
by default. This prevents stuck real-time tasks to monopolize 100% of the CPU.
To disable real-time throttling, use:
```
# echo -1 > /proc/sys/kernel/sched_rt_runtime_us
```
Also, real-time scheduling likely requires `root` user permissions.

The `--huge` parameter let the benchmark use huge pages for the memory buffer.
Mappings with larger TLB sizes of 2 MiB reduce the number of TLB misses compared
to 4 KiB-sized TLBs and can increase the stress on the memory subsystem.
To configure a specific number of 2 MiB memory blocks, e.g. 16 MiB, use:
```
# sysctl -w vm.nr_hugepages=8
```

The `--perf` parameter includes PMU counters in the benchmark output.


## Sustainable Memory Bandwidth

The sustainable memory bandwidth is the memory bandwidth
that the memory subsystem, and in particular, the memory controller,
can sustain under worst-case memory workload.
This is an important metric for timing analysis of multicore real-time systems,
where concurrent memory accesses interfere with tasks on other cores.

The sustainable memory bandwidth is lower than the linear memory bandwidth,
e.g. due to row misses in the same DRAM bank. The benchmark tries to find
sensitive patterns by iterating over the memory buffer
with increasing step sizes, e.g. 64 bytes, 128 byte, 256 bytes and so on.
This effectively probes each bits of a physical memory address.

Probing the sustainable memory bandwidth should use the `--huge` parameter
to enable huge pages for greater stability. Experiments show that
step sizes above 256 KiB need to be combined with larger buffer sizes
to cause reasonable stress, otherwise the memory accesses hit in the caches.
Step sizes above 1 MiB become unreliable, as each step accesses a different huge page.
Using 2 MiB huge pages, the approach can effectively probe the lowest 16
to 20 bits of physical addresses, but becomes somewhat unreliable beyond that.
Also, the test run should be pinned to a specific CPU for reliable results.

The `--auto` parameter enables the sustainable memory bandwidth measuring mode.
Example:
```
$ ./bench --size 32 --huge --cpu 0 --auto modify
worst-case modify bandwidth over 32 MiB block (huge TLB)
step 64: 5571.2 MiB/s, 5841.8 MB/s
step 128: 5607.7 MiB/s, 5880.1 MB/s
step 256: 5912.4 MiB/s, 6199.6 MB/s
step 512: 6627.0 MiB/s, 6948.9 MB/s
step 1024: 6451.6 MiB/s, 6765.0 MB/s
step 2048: 5932.2 MiB/s, 6220.3 MB/s
step 4096: 4371.5 MiB/s, 4583.8 MB/s
step 8192: 4233.9 MiB/s, 4439.5 MB/s
step 16384: 2246.2 MiB/s, 2355.3 MB/s
step 32768: 810.2 MiB/s, 849.5 MB/s
step 65536: 494.0 MiB/s, 518.0 MB/s
step 131072: 464.3 MiB/s, 486.8 MB/s
step 262144: 324.9 MiB/s, 340.7 MB/s
step 524288: 331.7 MiB/s, 347.8 MB/s
step 1048576: 345.2 MiB/s, 362.0 MB/s
step 2097152: 2292.8 MiB/s, 2404.2 MB/s
slowest step size: 262144
```
We can observe a huge performance drop for step sizes of 16 KiB and beyond.
The minimum is reached at 256 KiB step size. Greater step sizes, e.g 2 MiB,
show increased performance due to hits in the last-level cache.

Using a larger memory buffer of 256 MiB improves the stability of the test:
```
./bench --size 256 --huge --cpu 0 --auto modify
worst-case modify bandwidth over 256 MiB block (huge TLB)
...
step 524288: 335.9 MiB/s, 352.2 MB/s
step 1048576: 347.9 MiB/s, 364.8 MB/s
step 2097152: 348.7 MiB/s, 365.6 MB/s
step 4194304: 349.4 MiB/s, 366.4 MB/s
step 8388608: 347.8 MiB/s, 364.7 MB/s
step 16777216: 2291.1 MiB/s, 2402.4 MB/s
```

Note that `--auto` can be combined with `--all` to run all memory access tests,
see below.


## Automated Testing

For automated testing, the `-l <loops>` and `-d <delay_in_ms>` parameters
change the number and timing of test runs, and `--all` allows to run all tests.

Example: To run all memory access tests (`--all`) for five times (`-l 5`)
for 2000 ms (`-d 2000`) each on a 32 MiB memory buffer (`-s 32`), use:
```
$ ./bench -s 32 -l 5 -d 2000 --all
```

Another example: To assess the sustainable memory bandwidth of all access tests
for 10 seconds on CPU 0 on a 64 MiB memory buffer using huge pages, use:
```
$ ./bench --delay 10000 --size 64 --huge --cpu 0 --auto --all
```

### CSV Output

The benchmark can record CSV files from the test runs:
```
$ ./bench -s 64 --huge --all --csv test.csv
```
This runs all access tests using a 64 MiB buffer using huge pages
for one second and stores the results in `test.csv` (`--csv test.csv`).

The CSV file uses semicolons as separator and emits results in five columns:
```
#test;step;time_nanoseconds;bytes_accessed;bytes_perf
read;64;1001415437;11341398016;0
write;64;1002500600;6174015488;0
modify;64;1002515281;5754585088;0
```

Here, `bytes_accessed` shows the amount of bytes accessed from the CPU,
while `bytes_perf` shows the related values observed by PMU counters.
In the example above, `--perf` was not set and PMU counters were not used.

The resulting memory bandwidth is `bytes / time_nanoseconds`.

## Related Publication

- A. Zuepke, A. Bastoni, W. Chen, M. Caccamo, R. Mancuso:
  *MemPol: Polling-based Microsecond-scale Per-core Memory Bandwidth Regulation*.
  Real-Time Systems Journal, June 2024.
  [DOI 10.1007/s11241-024-09422-8](https://doi.org/10.1007/s11241-024-09422-8)
