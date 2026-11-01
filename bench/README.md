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

The designs are `designs/pipeline.sv`, a chain of registered stages, and
`designs/pipeline.vhd`, the same chain written as VHDL RTL is, on `std_logic_vector`
through `numeric_std`, recorded as `pipeline-vhdl`. A design in a language the build has no
frontend for is skipped, as `enzan --version` lists the frontends.

Each row records the timestamp, the git revision, the design, the cycle count, the elapsed
seconds, cycles per second, and the instruction and delta counts the kernel reports. A
measurement taken from a dirty working tree is marked `-dirty`, because a number that cannot be
attributed to a commit is worse than no number: it makes the history look trustworthy when it
is not.

The elapsed time covers analysis, elaboration and the run, which is what a user waits for.
Separating them is worth doing once there is something to tune; conflating them at least never
flatters the result.

It is a target rather than a test. CI runs it as a step of its own, and a measurement taken on a
loaded machine alongside three hundred other tests is not comparable with anything.

`results.tsv` is not committed: a number from one machine does not compare with one from
another, and the file would be a merge conflict on every run.

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
figure. A tree-walking interpreter would not approach it; the bytecode VM is a
large step up and the LLVM JIT (M8) is what is expected to reach it.
