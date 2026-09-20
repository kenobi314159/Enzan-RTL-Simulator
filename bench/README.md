# `bench/` — performance benchmarks

DESIGN_GUIDE.md section 15.1 states the binding requirement plainly: for M0–M2
the requirement is **not** the performance number, it is that this directory
exists and records cycles/sec per commit from the very first milestone. A
performance goal without continuous measurement is decoration, and performance
regressions are invisible without a baseline.

## Status

**Empty by design at M0.** There is no simulator yet, so there is nothing to
measure and no number that would mean anything. Populating this directory with a
harness that reports zero would create the appearance of measurement without the
substance, which is worse than an honest gap.

The first real content arrives with M1, at the same time as the bytecode VM and
the scheduler, because M1's exit criteria include a recorded cycles/sec figure.

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
