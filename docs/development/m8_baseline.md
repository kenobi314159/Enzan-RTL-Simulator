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

## Implications and limits

Simulation accounts for 97% or more of end-to-end time in five cases. The short
VHDL pipeline spends a larger fraction in analysis, so extra JIT preparation
could have a visible effect there. This supports measuring process execution
and kernel work independently; it does not establish how much JIT can accelerate
either of them.

These results have no cocotb, JIT compilation, or 100k-LUT measurement. They
do not establish a Verilator comparison, a break-even run length, or an
acceptance target. The next measurement uses the `gcc-profile` preset with
frame pointers and representative real designs before those decisions are made.
