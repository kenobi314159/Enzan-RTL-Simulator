# `bench/` — performance benchmarks

The requirement this directory meets is **not** a performance number: it is that
cycles/sec are recorded per commit, from the first simulation onwards. A
performance goal without continuous measurement is decoration, and performance
regressions are invisible without a baseline.

## Status

`run-bench.sh` measures simulated clock cycles per second and appends a row to `results.tsv`:

```sh
cmake --preset gcc-release
cmake --build --preset gcc-release
./bench/run-bench.sh ./build/gcc-release/bin/enzan
```

CMake also exposes it as a target: `cmake --build --preset gcc-release --target enzan-bench`.

The designs are each a source under `designs/`, or a file list there naming several:

| Design | What it is | What it measures |
|---|---|---|
| `pipeline` | a chain of registered stages | data through registers, the commonest operators |
| `pipeline-vhdl` | the same chain written as VHDL RTL is, on `std_logic_vector` through `numeric_std` | VHDL's operators on vectors |
| `router` | the router of `tests/golden/sv/router.sv`, faults and starvation injected on a schedule | control: enumerations, case statements, asynchronous resets, three levels of hierarchy |
| `counters` | the 124 counters of `tests/golden/sv/large_design_00`, 64 of them six generate levels deep | many small processes, and values carried up through many ports |
| `fifo-vhdl` | the FIFO of `tests/golden/vhdl/fifo.vhd`, pushed and popped by an LFSR | a VHDL memory indexed at run time |
| `fifo-mixed` | the same FIFO and stimulus, the testbench in SystemVerilog | crossing the language boundary on every port and cycle |

A file list reads a golden case's sources where they are, under a testbench of the benchmark's
own in `designs/tops/`, so a change to one of those sources moves the benchmark too, and its
reference with it. `fifo-vhdl` and `fifo-mixed` print the same checksum, and every design
prints what Icarus Verilog or GHDL prints for it. A design in a language the build has no
frontend for is skipped, as `enzan --version` lists the frontends.

Each design runs three times, and the fastest run is recorded: whatever else slows a run down,
another process or a cold cache, only ever slows it down. Each row records the timestamp, the
git revision, the design, the cycle count, the elapsed seconds, cycles per second, and the
instruction and delta counts the kernel reports. A measurement taken from a dirty working tree
is marked `-dirty`, because a number that cannot be attributed to a commit is worse than no
number: it makes the history look trustworthy when it is not.

The elapsed time covers analysis, elaboration and the run, which is what a user waits for.
To measure their contributions separately, add `--phase-timings` to a simulation. It prints
nanoseconds to standard error for analysis, elaboration, bytecode compilation, image preparation
and kernel execution. Elaboration excludes bytecode and image preparation. The report excludes
library opening outside analysis, waveform setup and flushing, so its sum is not end-to-end time.
For a served session, simulation time accumulates over runs and the report appears when it ends.
With `--backend jit`, a separate line reports native-code preparation; image preparation excludes
that time. The five-column phase file does not capture the additional JIT preparation field.
To collect those timings for the benchmark designs, run:

```sh
./bench/run-bench.sh --phases /tmp/enzan-phases.tsv ./build/gcc-release/bin/enzan
```

The phase file records the five nanosecond counts of the same run selected for its best
end-to-end time. It is separate from `results.tsv`, so existing benchmark references stay
comparable. Timed and untimed runs should not be mixed when measuring small differences.

`--activity-stats` prints an opt-in kernel activity report on standard error. `process_runs`
counts process activations, including resumptions; `connection_runs` is the subset explicitly
scheduled in the port-connection queue, not every process materialising a port expression.
`enqueues` counts accepted wake/resume requests through the kernel, excluding timed
events released directly by the scheduler. `signal_changes` counts actual value changes, rather
than writes that leave a value unchanged. `nba_updates` counts nonblocking updates taken for
application, and `transactions` counts applied delayed or delta transactions; either can include
a write that leaves the signal unchanged. `resolution_calls` counts kernel-invoked resolution
functions. Counts include initialization and accumulate across runs and restarts of one kernel;
a new elaboration creates new counters. Instrumentation can affect timing, so compare ordinary
timings with ordinary timings and use the counters to explain the work.

For a real design, `./bench/run-neorv32.sh <enzan> <ghdl> <work-directory>` runs the pinned
NEORV32 regression with phase timings. It reports the VHDL and mixed-language testbenches only
after both match GHDL's reports and CPU/UART logs. It uses the regression's fixed simulation
window and work directory, including the output files that regression replaces. Each result is
one run, rather than the best of three; repeat it explicitly when comparing performance.

`./bench/run-cocotb.sh <enzan> <python-with-cocotb> <work-directory>` measures a separate
10,000-cycle counter workload with Python clock and edge callbacks. It checks the expected
counter value with Icarus Verilog first and with Enzan, then reports Enzan's end-to-end and
phase times. Each invocation makes a fresh directory under the work directory and keeps both
simulators' logs; it records one run. The supplied Python must have the project's pinned cocotb
version installed, and Icarus Verilog must be on `PATH`.

`./bench/compare-verilator.sh <enzan> <verilator> <iverilog> <work-directory>` compares the
SystemVerilog pipeline over one million cycles with Verilator. Before timing, it holds settled
pipeline states and the final result to Icarus Verilog under all three simulators. Verilator runs
single-threaded without tracing. The script measures its translation and native build separately,
then alternates three Enzan and Verilator runs and reports the best single-invocation times. The
ratio compares Enzan's analysis and run against the already-built Verilator executable; use the
separate build time when comparing a fresh build and one run. Its work directory retains compiler
logs, outputs and generated code for inspection.

`./bench/run-scale.sh <enzan> <iverilog> <width> <work-directory>` runs a synthetic network of
explicit `LUT6` instances for 20 active clock cycles. The width must be a multiple of 32; each
bit is one retained cell fed by five neighbouring state bits and a shared phase bit. The script
compares the low 32 bits at 20 settled states in a 1,024-cell case with Icarus and checks every
bit of Enzan's final state against the repeated 32-bit state, then reports elapsed time, phase
times and peak RSS for one run. It retains the logs in a fresh work directory. This is a
structural stress test, not a representative FPGA design or a post-place device-utilisation
figure. It needs Python 3 as well as Icarus and its `vvp` runner.

Yosys can independently count the explicitly instantiated cells after RTL optimisation, for
example at width 1,024:

```sh
yosys -p "read_verilog -sv -D SYNTHESIS bench/scale/cells.sv; \
  hierarchy -top network -chparam WIDTH 1024; proc; opt; stat"
```

The LUTs are black boxes for this count, so it measures retained structural cells and does not
predict the LUT utilisation of a synthesised RTL design. The timing includes `$countones` once
at the end, and the short run is intended to expose preparation and memory costs as well as
simulation cost; it is not a sustained-throughput measurement.

It is a target rather than a test. CI runs it as a job of its own, and a measurement taken on a
loaded machine alongside three hundred other tests is not comparable with anything.

`results.tsv` is not committed: a number from one machine does not compare with one from
another, and the file would be a merge conflict on every run.

### Holding a change to the reference

CI's benchmark job runs `run-bench.sh --check reference.tsv`, which fails when a design does
more work or runs more slowly than its row of `reference.tsv` allows:

- **Its instructions and delta cycles may grow by `work_margin` percent at most**, as
  `run-bench.sh` sets it. The kernel counts them, so they are the same on every machine and
  every run of one build, and a change that makes a design take more of them is seen whatever
  machine it is measured on. Two runs of one design counting differently fail, since that would
  be a simulation that is not deterministic.
- **Its rate may fall to `rate_floor` percent of the reference at most.** It depends on the
  machine, GitHub's runners are not all one model of processor, and the best of three runs on
  one machine still varies by a tenth or more, so the bound is loose. It is there for a slowdown
  the counts cannot see, such as one in the dispatch loop.
- **A design with no row fails**, so that a design added is given its row in the same commit.

A change that moves a design on purpose replaces its row in the same commit, with the row the
job printed for it, which the job's summary shows. A design whose counts fall by more than
`work_margin` percent is named, so that its row is lowered and what the change won is held from
then on, as a conformance suite's pass list is ratcheted. The counts can be checked on any
machine:

```sh
./bench/run-bench.sh --check bench/reference.tsv ./build/gcc-release/bin/enzan
```

where a rate below the reference's says only that the machine is slower than CI's.

### Finding a value in the wave window

`find-value/FindValueBench.cpp` measures what the wave window's search for a value costs per
change of a trace, which is what a reader waits for: no index answers the search, so its cost
grows with the trace. A kernel runs a counter for as many changes as are asked for, the window's
session fetches them all, and the model is searched for a value the counter never takes, so that
every change is examined, for a row of the signal, a slice of it and a group holding it, which is
searched as every row with children is:

```sh
cmake --build --preset gcc-release --target enzan-bench-find-value
./build/gcc-release/bin/enzan-bench-find-value 10000000
```

It is built only when asked for, and it records nothing: it is what the search's design was
decided by, kept so that the decision can be checked again. A trace past the database's memory
budget spills to the temporary directory on both ends of the socket, so a large one wants
`TMPDIR` on a disk with room, and short enough for the socket's path.


## What goes here, and what the rules are

- A **fixed** set of pinned designs with **fixed** stimulus. A benchmark whose
  input changes measures nothing across commits.
- Designs are *fetched* at pinned revisions by scripts, exactly as under
  `tests/designs/`. External RTL is never committed to this repository.
- Results are recorded per commit, as cycles/sec, so that a regression is
  attributable to a change rather than noticed months later.
- No optimisation without a measurement. This directory is what makes a
  performance claim checkable rather than asserted.

## The provisional target

On the order of 10,000 simulated clock cycles per second on a design of roughly
100k FPGA LUTs on a single core, and within about 5× of Verilator on the purely
synchronous subset.

The number is **provisional** and will be replaced with a measured, committed
figure. The LLVM JIT remains an experiment; its results are recorded in
`docs/development/m8_jit_experiment.md`.
