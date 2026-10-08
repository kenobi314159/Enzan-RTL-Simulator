# `bench/` — performance benchmarks

DESIGN_GUIDE.md section 15.1 states the binding requirement plainly: for M0–M2
the requirement is **not** the performance number, it is that this directory
exists and records cycles/sec per commit from the very first milestone. A
performance goal without continuous measurement is decoration, and performance
regressions are invisible without a baseline.

## Status

**Active since M1.** `run-bench.sh` measures simulated clock cycles per second and appends a
row to `results.tsv`:

```sh
cmake --preset gcc-release
cmake --build --preset gcc-release
./bench/run-bench.sh ./build/gcc-release/bin/enzan
```

CMake also exposes it as a target: `cmake --build --preset gcc-release --target enzan-bench`.

Each row records the timestamp, the git revision, the design, the cycle count, the elapsed
seconds, cycles per second, and the instruction and delta counts the kernel reports. A
measurement taken from a dirty working tree is marked `-dirty`, because a number that cannot be
attributed to a commit is worse than no number: it makes the history look trustworthy when it
is not.

The elapsed time covers analysis, elaboration and the run, which is what a user waits for.
Separating them is worth doing once there is something to tune; conflating them at least never
flatters the result.

It is a target rather than a test. CI runs it as a step of its own (section 15.3), and a
measurement taken on a loaded machine alongside three hundred other tests is not comparable
with anything.

`results.tsv` is not committed: a number from one machine does not compare with one from
another, and the file would be a merge conflict on every run. CI publishes its own series.


## What goes here, and what the rules are

- A **fixed** set of pinned designs with **fixed** stimulus. A benchmark whose
  input changes measures nothing across commits.
- Designs are *fetched* at pinned revisions by scripts, exactly as under
  `tests/designs/`. External RTL is never committed to this repository
  (DESIGN_GUIDE.md section 17.5).
- Results are recorded per commit, as cycles/sec, so that a regression is
  attributable to a change rather than noticed months later.
- No optimisation without a measurement (section 13.4). This directory is what
  makes a performance claim checkable rather than asserted.

## The provisional target

On the order of 10,000 simulated clock cycles per second on a design of roughly
100k FPGA LUTs on a single core, and within about 5× of Verilator on the purely
synchronous subset.

The number is **provisional** and will be replaced with a measured, committed
figure once M1 produces real ones. A tree-walking interpreter would not approach
it; the bytecode VM is a large step up and the LLVM JIT (M8) is what is expected
to reach it.
