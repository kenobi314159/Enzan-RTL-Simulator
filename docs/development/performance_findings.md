# Performance investigation

These measurements were made in November 2026 on an Intel Core i5-8500T in
an Ubuntu 24.04 Docker container, with GCC 13.3.0 release builds. Runs were
not CPU-pinned and other host activity was uncontrolled. They describe the
stated workloads, not general simulator throughput. The benchmarks and their
checks are described in `bench/README.md`.

## Preparation at structural scale

The checked `bench/run-scale.sh` workload has explicitly instantiated LUTs and
20 active cycles. At 512 cells, a single before/after comparison of the object
name index reduced SystemVerilog analysis from 2.421 s to 0.0298 s. Both runs
passed the final-state check.

A 16,384-cell profile attributed 63.21% of whole-process user samples to linear
compiled-body lookup. The table compares single release runs before and after
replacing that scan with binary search. The runs were on different days, with
no build or other benchmark running concurrently; the memory limit was 6 GiB.

| Cells | Version | Total s | Image and kernel preparation s | Simulation s | Peak RSS MiB |
|---:|---|---:|---:|---:|---:|
| 16,384 | Before | 7.508 | 4.812 | 1.059 | 364 |
| 16,384 | After | 2.770 | 0.125 | 1.037 | 364 |
| 100,000 | Before | 238.653 | 218.240 | 6.603 | 2,192 |
| 100,000 | After | 21.310 | 0.748 | 6.360 | 2,196 |

This artificial network measures scaling, not post-place FPGA utilization or
sustained throughput. These single samples are evidence for the preparation
change, not a general speedup claim.

## Comparison and instrumentation

The one-million-cycle synchronous pipeline agreed with Icarus and Verilator on
settled samples and the final result. Enzan's best end-to-end time was 2.856 s;
Verilator's best already-built execution was 0.223 s, after a separate 5.676 s
translation and build. Three runs were alternated. Verilator used one thread,
no waveform, and version 5.020. This small two-state-compatible workload does
not represent general performance.

On that pipeline, three interleaved runs with activity counters enabled took
3.194, 3.229 and 3.249 s of simulation; runs with counters disabled took
2.951, 3.202 and 3.151 s. Their outputs and work counts matched. The enabled
runs recorded 22 million process activations, 20 million enqueues, about 20
million signal changes and 10 million nonblocking updates. The overlap in
samples prevents a precise overhead estimate.
