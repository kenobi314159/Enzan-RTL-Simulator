# M8 initial benchmark baseline

This is an initial measurement of the existing benchmark set, not the M8 exit
target. The designs are small and do not establish performance at the intended
100k-LUT scale. They do establish whether startup or simulation dominates these
particular runs before the JIT is added.

## Environment

- Measured on 2026-11-20 in the `e-t-0` Docker container, as `ubuntu`, on an
  Intel Core i5-8500T, six cores, with 7.5 GiB of host memory. Ubuntu 24.04.5,
  Linux host kernel 7.2.5-3-omarchy, x86-64.
- GCC 13.3.0, CMake `gcc-release`, `-O3 -DNDEBUG`, both frontends enabled,
  GUI and JIT disabled. NVC revision `cba10a2e0e37ae1c1ece982b6cbe5c65423800d3`;
  slang revision `7ddf4059f79eff508dd486eb42fd650cdf320d52`.
- Simulator binary reports `98b1c944`. The benchmark harness and checkout were
  at `f328c9b`, which adds the phase recording harness and its test without
  changing the simulator. The tracked tree was clean during measurement.
- No waveform files or VPI applications. The build was idle during measurement;
  CPU affinity was not fixed and other host activity was not controlled.

The command was:

```sh
./bench/run-bench.sh --phases /tmp/phases.tsv \
  ./build/gcc-release/bin/enzan /tmp/results.tsv
```

Each row below is the fastest of three complete runs, with phase times from that
same run. End-to-end time has millisecond resolution. The phase columns are
separate wall times from `--phase-timings`; their sum excludes process startup,
library opening outside analysis, waveform setup and file flushing.

| Design | Cycles | End-to-end s | Cycles/s | Analysis ms | Elaboration ms | Bytecode ms | Image ms | Simulation ms | Simulation / total |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| pipeline | 100,000 | 0.293 | 341,297 | 1.907 | 0.073 | 0.029 | 0.028 | 284.339 | 97.0% |
| pipeline-vhdl | 5,000 | 0.091 | 54,945 | 13.631 | 0.061 | 0.112 | 0.049 | 70.395 | 77.4% |
| counters | 25,000 | 1.242 | 20,129 | 15.956 | 0.793 | 0.131 | 0.396 | 1,219.689 | 98.2% |
| fifo-mixed | 200,000 | 1.400 | 142,857 | 24.885 | 0.100 | 0.162 | 0.036 | 1,368.261 | 97.7% |
| fifo-vhdl | 200,000 | 1.206 | 165,837 | 25.334 | 0.091 | 0.321 | 0.079 | 1,174.568 | 97.4% |
| router | 400,000 | 1.482 | 269,906 | 3.946 | 0.098 | 0.121 | 0.048 | 1,473.265 | 99.4% |

The instruction and delta counts matched `bench/reference.tsv` exactly. Rates
should not be compared directly with CI's reference: they were taken on
different machines. Timer output was enabled for these runs, so comparisons with
untimed runs should allow for its overhead.

## NEORV32 processor regression

On the same machine and container, three isolated repetitions of the pinned
NEORV32 regression were measured at Enzan checkout `ceee0a6`, with the same
simulator binary `98b1c944` as above. The processor source was at revision
`84743cdbeefccb531f836d66933c8478f1d9bf71`; GHDL was 4.1.0. Each repetition
passed the comparison of reports and CPU/UART logs with GHDL, for both the VHDL
and mixed-language testbench. No source compilation ran concurrently; other
host activity and CPU affinity were not controlled.

The regression's fixed window is 160 µs with a nominal 100 MHz clock. Its
testbench enables a dual-core processor, trace logging and external memory models.
It writes CPU and UART logs but no waveform file. This is a processor RTL
workload, not a measured 100k-LUT netlist, and the two testbenches do not perform
identical work. Their times should not be interpreted as a language-overhead
comparison.

```sh
./bench/run-neorv32.sh ./build/gcc-release/bin/enzan /usr/bin/ghdl \
  ./build/bench-neorv32
```

The harness reports one run of each testbench. These are the three end-to-end
times observed, in seconds; the fastest row's phase times follow. The elapsed
time includes starting the simulator, not GHDL or fetching the design.

| Testbench | Run 1 | Run 2 | Run 3 | Analysis s | Elaboration s | Bytecode s | Image s | Simulation s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| VHDL | 35.215 | 35.738 | 35.098 | 1.166 | 0.335 | 0.021 | 0.020 | 33.534 |
| Mixed | 35.635 | 35.773 | 35.407 | 1.506 | 0.009 | 0.021 | 0.020 | 33.821 |

Simulation was about 95.5% of each fastest run. These timings include the
testbench's CPU trace and file I/O, so they do not isolate process execution.

## Selected waveform capture

On the same machine and simulator binary, at checkout `4c68bb8`, the
SystemVerilog pipeline was run three times without a waveform, interleaved with
three runs recording the `/bench/u_stage0` scope to FST. The simulation printed
the same result and instruction/delta counts in every run. `fst2vcd` decoded the
file and showed one 32-bit signal, `mixed`, with 100,001 time markers. Each FST
file was 500,684 bytes. This is selective capture of one signal, not a measure
of recording the entire design.

```sh
./build/gcc-release/bin/enzan --top bench --timescale ns --dump-stats \
  --phase-timings bench/designs/pipeline.sv
./build/gcc-release/bin/enzan --top bench --timescale ns --dump-stats \
  --phase-timings --fst /tmp/pipeline.fst --wave-scope /bench/u_stage0 \
  bench/designs/pipeline.sv
```

| Repeat | No waveform: end-to-end s | FST: end-to-end s | No waveform: simulation s | FST: simulation s |
|---|---:|---:|---:|---:|
| 1 | 0.307 | 0.375 | 0.302 | 0.361 |
| 2 | 0.311 | 0.376 | 0.302 | 0.362 |
| 3 | 0.306 | 0.379 | 0.299 | 0.364 |

The best FST end-to-end time was 22.7% higher than the best without capture.
The end-to-end measurement includes opening and closing the file; the reported
simulation phase excludes those operations. No build ran during these tests;
CPU affinity and other host activity were not controlled.

## Cocotb callbacks

At checkout `4ae1472`, the same simulator binary and container ran the fixed
10,000-cycle Python clock and rising-edge callback workload in `bench/cocotb/`.
The run used cocotb 2.1.0, CPython 3.12 and Icarus Verilog 12.0 as the reference.
Every repetition passed the expected counter value under both simulators; no
waveform was recorded. No build or profiling ran concurrently, but CPU affinity
and other host activity were not controlled.

```sh
./bench/run-cocotb.sh ./build/gcc-release/bin/enzan .venv/bin/python \
  ./build/bench-cocotb
```

| Repeat | Enzan make invocation s | Analysis s | Elaboration s | Bytecode s | Image s | Simulation s |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 2.136 | 0.000686 | 0.000024 | 0.000010 | 0.349 | 0.545 |
| 2 | 2.133 | 0.000888 | 0.000027 | 0.000011 | 0.342 | 0.565 |
| 3 | 1.723 | 0.000979 | 0.000031 | 0.000014 | 0.291 | 0.471 |

The elapsed measurement covers the Enzan `make` invocation, including its
configuration and simulator startup; it excludes preparing the harness, the
Icarus run and the subsequent result-file check. The image phase includes kernel
initialisation and start-of-simulation callbacks. The fastest invocation spent
about 27% of its elapsed time in the simulation phase. This is one short
callback-heavy workload; it does not isolate VPI overhead or predict a longer
cocotb regression's performance.

## Frame-pointer profiles

The same container and hardware were sampled with the `gcc-profile` preset,
which uses GCC 13.3.0, `RelWithDebInfo`, `-O2 -g -DNDEBUG`,
`-fno-omit-frame-pointer` and `-mno-omit-leaf-frame-pointer`. Enzan's internal
assertions are enabled in this configuration. The simulator binary reports
`98b1c944`; its runtime sources are the same as in the measurements above.
Both frontends were enabled, with GUI and JIT disabled.

Host `perf` 7.2.3 sampled `task-clock:u` at 999 Hz with frame-pointer call
stacks. The profiled process ran as `ubuntu` in the container. For each small
benchmark, perf attached to a stopped container shell before it ran four copies
of the unchanged benchmark, inheriting their processes. Their reported results
matched across repetitions. The NEORV32 profile is one run of its VHDL testbench
with the same fixed window as above; its reports and four logs matched GHDL.
No waveform files were recorded, and no build ran concurrently.

The table reports **self samples**, as `perf report --no-children` attributes
them, over the entire process lifetime. It includes frontend and subprocess
user time and excludes kernel time; the short VHDL pipeline has relatively few
samples. Enzan's debug symbols and the container's shared libraries were made
available to perf through a symbol root. System-library frame-pointer support
was not verified, so call stacks through those libraries may be incomplete.
No samples were reported lost.

Where `perf` is available in the same environment as the simulator, the counters
profile can be repeated directly from the repository root:

```sh
perf record -e task-clock:u -F 999 --call-graph fp -o /tmp/counters.data -- \
  bash -c 'for n in 1 2 3 4; do build/gcc-profile/bin/enzan --top bench \
    --timescale ns -F bench/designs/counters.f || exit; done'
perf report --stdio --no-children -g none -i /tmp/counters.data
```

For the measurements above, host perf attached to the container process, so its
`--symfs` mapped `/w` to the host's mounted home directory and contained copies
of the container libraries named by `perf buildid-list`.

| Workload | Samples, approximately | `Interpreter::execute` self | Other notable self samples |
|---|---:|---:|---|
| pipeline | 1,000 | 22.2% | `changeSignal` 6.9%; `notifyChange` 5.1% |
| pipeline-vhdl | 376 | 29.5% | `present` 8.8%; `encode` 3.7% |
| counters | 6,000 | 29.4% | `notifyChange` 7.4%; `wake` 4.1% |
| fifo-mixed | 6,000 | 29.3% | bit `set` 10.6%; bit `get` 8.7% |
| fifo-vhdl | 5,000 | 38.5% | VHDL `drive` 3.4%; event suspension 2.7% |
| router | 7,000 | 35.7% | `changeSignal` 4.2%; `notifyChange` 3.0% |
| NEORV32 VHDL | 37,000 | 43.0% | event suspension 5.8%; VHDL `drive` 3.2% |

`Interpreter::execute` contains inlined value and control-flow work as well as
dispatch, and its separately compiled helpers are attributed to other symbols.
The percentages neither measure dispatch alone nor predict JIT speedup. They do
show that native process execution is worth investigating, while bit access,
signal updates, waits and transactions remain candidates for separate profiling
and improvement. The small workloads and user-space-only samples do not set the
M8 target.

## Implications and limits

Simulation accounts for 97% or more of end-to-end time in five cases. The short
VHDL pipeline spends a larger fraction in analysis, so extra JIT preparation
could have a visible effect there. This supports measuring process execution
and kernel work independently; it does not establish how much JIT can accelerate
either of them.

These results have no JIT compilation or 100k-LUT measurement. They do not
establish a Verilator comparison, a break-even run length, or an acceptance
target. Larger designs and a comparable synchronous Verilator workload are
still needed before those decisions are made.
