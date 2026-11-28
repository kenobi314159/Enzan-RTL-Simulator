# M8 JIT experiment

The optional LLVM ORC backend on this branch is an experiment, not an M8 exit
requirement. The interpreter remains the default. The JIT has incomplete opcode
and operand coverage, rejects unsupported operations before simulation and does
not fall back to interpretation. It has been tested with LLVM 18; other LLVM
versions accepted by the build have not been similarly validated. See
`src/jit/README.md` for its architecture and `README.md` for how to enable it.

## Final comparison

The experiment evaluated three goals: at least 1.5× interpreter/JIT simulation
speed on both the one-million-cycle SystemVerilog pipeline and the 160 µs VHDL
NEORV32 regression; at most 2 s additional JIT preparation on NEORV32; and peak
RSS no greater than the larger of 128 MiB or twice the interpreter's peak.
The rules used the best simulation time and the largest RSS in three fresh
processes per backend, with fixed inputs and output checks. These were criteria
for this experiment, not commitments for the remaining M8 work.

The latest measurements used the batched backend on an Intel Core i5-8500T in
Docker `e-t-0`, as `ubuntu`, with GCC 13.3.0, LLVM 18.1.3 and release builds.
The backends used the same binary and no waveforms; runs were interleaved and
no build or other benchmark ran concurrently. CPU affinity and other host
activity were uncontrolled. Times and memory are therefore indicative of these
workloads, not guaranteed bounds for other systems or designs.

| Workload and backend | Simulation seconds | Maximum RSS KiB |
|---|---|---:|
| Pipeline interpreter | 2.995, 3.010, 3.004 | 55,628 |
| Pipeline JIT | 2.083, 2.070, 2.070 | 68,548 |
| NEORV32 interpreter | 33.335, 32.985, 33.128 | 99,516 |
| NEORV32 JIT | 28.707, 29.201, 29.025 | at least 213,804 |

The best simulation ratios were 1.45× and 1.15× respectively, both below 1.5×.
Pipeline JIT preparation took 17.6–18.9 ms. NEORV32 JIT preparation took
31.952–32.332 s, exceeding the 2 s goal. Pipeline RSS met its limit. The third
NEORV32 JIT RSS measurement was lost; the first two already exceeded the
199,032 KiB limit, so the memory goal also failed.

All processor runs matched GHDL's reports and four CPU/UART traces and executed
1,220,054,088 instructions and 117,372 delta cycles. This establishes correctness
for those runs, not for unsupported opcodes or arbitrary programs. The pipeline
runs produced the same checksum and work counts. On the measured processor rates,
the approximately 32 s preparation cost would break even only after roughly
1.2 ms of simulated time, assuming constant performance and one compilation;
this crossover is an extrapolation, not a measured result.

## Repeating the comparison

Build with both frontends and `-DENZAN_ENABLE_JIT=ON`, setting `LLVM_DIR` to a
supported LLVM package if necessary. For the pipeline, run each backend three
times in alternating order, replacing `jit` with `interpreter`:

```sh
./build/gcc-release/bin/enzan --backend jit --phase-timings --dump-stats \
  --top bench --timescale ns -GCYCLES=1000000 bench/designs/pipeline.sv
```

For NEORV32, `bench/run-neorv32.sh` supplies the reference checks and phase
timings. A small executable wrapper selects the backend for the simulator calls
while leaving its version query intact:

```sh
export ENZAN_REAL="$(realpath ./build/gcc-release/bin/enzan)"
cat >/tmp/enzan-jit <<'WRAPPER'
#!/bin/sh
if [ "${1:-}" = --version ]; then exec "$ENZAN_REAL" "$@"; fi
for arg do
  case "$arg" in
    */neorv32_tb.v) exec "$ENZAN_REAL" --backend interpreter "$@" ;;
  esac
done
exec "$ENZAN_REAL" --backend jit "$@"
WRAPPER
chmod +x /tmp/enzan-jit
./bench/run-neorv32.sh /tmp/enzan-jit /usr/bin/ghdl ./build/bench-neorv32
```

Use a corresponding interpreter wrapper, alternate three fresh runs of each,
and measure each Enzan process's peak RSS separately. The harness checks both
VHDL and mixed-language testbenches; the figures above use the VHDL case, and
the wrapper uses the interpreter for the mixed testbench. The JIT preparation
line is separate from the five recorded phase columns.

## Findings that informed the experiment

An earlier three-run pipeline comparison measured the interpreter before and
after introducing the backend boundary: best simulation times were 2.775 and
2.836 s, respectively, about 2.2% slower. An inlined per-activation context
was later within ordinary run-to-run variation in a separate comparison. These
are historical intermediate configurations, not results for the final branch.

Earlier unbatched NEORV32 runs had 23.926 s best JIT simulation against 24.042 s
best interpreter simulation, 28.503–29.641 s JIT preparation and 444,688 KiB
maximum RSS. The runs used the same 160 µs regression, reference checks and
three interleaved processes per backend. Host variation prevents treating the
change between these and the final measurements as a controlled speedup.

A 26,109-sample preparation profile attributed 86.5% of user samples to LLVM,
with no reported lost samples. In one run, LLVM's least optimized machine-code
setting reduced preparation to 5.761 s but increased simulation to 27.541 s
and peak RSS to 449,312 KiB. It still missed the preparation goal and was not
retained. Separately, batching LLVM modules reduced peak RSS from 443,676 to
211,708 KiB in one paired comparison, with preparation of 33.341 and 31.915 s
and simulation of 28.264 and 28.433 s. Both versions matched GHDL. Batching is
retained, but the final comparison still missed the memory goal.

One profile of the final NEORV32 simulation captured 14,989 samples with no
reported loss; the profiler was terminated as the simulator exited. Generated
native code was not symbolized. The largest named samples included event
suspension (6.1%), signal driving (5.8%) and part selection (5.6%). They suggest
shared runtime work to investigate but cannot account precisely for native work.

The results do not justify treating the JIT as the route to M8 completion. The
experiment preserves a working subset and the measured tradeoffs for a future
long-running workload that may make native preparation worthwhile. Independent
interpreter and preparation findings are in `performance_findings.md`.
