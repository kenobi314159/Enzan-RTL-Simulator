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

## Implications and limits

Simulation accounts for 97% or more of end-to-end time in five cases. The short
VHDL pipeline spends a larger fraction in analysis, so extra JIT preparation
could have a visible effect there. This supports measuring process execution
and kernel work independently; it does not establish how much JIT can accelerate
either of them.

These results have no real-design, waveform-capture, cocotb, JIT compilation,
or 100k-LUT measurement. They do not establish a Verilator comparison, a
break-even run length, or an acceptance target. The next measurement uses the
`gcc-profile` preset with frame pointers and representative real designs before
those decisions are made.
