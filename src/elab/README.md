# `src/elab` — elaboration

Takes a named top level plus the analysed library, expands the instance tree, binds
ports and computes the flat state layout (DESIGN_GUIDE.md section 2.1).

## Responsibility

- Resolve the top module, or refuse to guess when the choice is not forced.
- Expand the instance tree, detecting recursive instantiation.
- Assign every object of every instance a **slot**: a piece of state with an offset
  in the single contiguous arena of section 4.3.
- Bind ports by collapsing a connected port onto the parent object's slot, so that a
  port costs nothing at run time.
- Produce the list of processes to schedule, as (instance, process) pairs.
- Reject the cases that would need features later milestones bring — port width
  conversion, driver resolution — rather than let them simulate to a plausible wrong
  answer.

## What elaboration is not

- **It does not evaluate parameters or expand generate constructs.** Those happen in
  the frontend, which emits one EIR definition per distinct parameterisation. That is
  what keeps an analysed unit reusable without re-evaluating anything (section 2.1).
- **It is not cacheable.** Elaboration is per-design by nature: the same library
  elaborated against a different top level produces a different image, so nothing here
  is stored in a library.
- **It does not compile expressions.** A port connection names an object; the frontend
  normalises anything more complicated into an extra object plus a continuous
  assignment.

## Allowed dependencies

`common` and `eir`.

Not `codegen`, `runtime`, `wavedb` or `pli` — they consume elaboration's output.
Optimisation passes that section 2.1 places in this phase — inlining, selective
flattening, process merging, partitioning — belong here when they arrive, and none of
them may reach into the runtime.

## Port collapsing

A connected port of the same width as the object it binds to becomes **the same
storage under two names**. Both hierarchical paths resolve to one slot, and the user
may use either.

This is what commercial tools do, and the alternative — an implicit continuous
assignment per port — would add a process and a delta cycle to every port in the
design. It also means an object's storage may live in an ancestor's part of the arena,
which is why an instance holds a slot index per object rather than a base offset plus
a fixed displacement. The interpreter pays one extra load per state access; a JIT
specialises a process per instance and burns the absolute offsets in as constants,
which is what section 4.3 anticipates.

## Known limitation: the driver check is per signal, not per bit range

Driving disjoint parts of one vector from several continuous assignments — `assign x[7:4] =
a; assign x[3:0] = b;` — is legal SystemVerilog and needs no resolution at all, because no bit
has two drivers. Enzan's check counts drivers per *signal*, so it refuses that design.

The check is deliberately conservative: the alternative to refusing is letting the last write
win, which produces a plausible wrong answer rather than an error, and section 1.2 ranks that
as the worst outcome available. Making it per bit range is real work that belongs with driver
resolution at M5, where the same bit-level bookkeeping is needed anyway.

## Port collapsing across a language boundary

Collapsing does **not** apply across a language boundary. Section 3.5 defines a
cross-language port connection as a zero-delay continuous assignment that consumes one
delta cycle, and that is a mixed-language matter for M7.
