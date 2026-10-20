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
- Before any of it, connect each port crossing between SystemVerilog and VHDL through
  conversions (`Boundary.h`, DESIGN_GUIDE.md section 3.5): an object of the port's own
  representation in the instantiating definition and a process each way the port's mode needs,
  each costing one delta cycle. A port no conversion covers is refused (ENZ3016), and one whose
  encoding still differs from its object's is refused at binding (ENZ3015).
- Produce the list of processes to schedule, as (instance, process) pairs.
- Find bits with more than one driver, per bit rather than per signal (DESIGN_GUIDE.md
  section 3.7), and refuse those the language does not allow: a bit of a variable or a
  `uwire` with two continuous drivers, or with a continuous driver and a procedural writer.
- Give each package whose variables something refers to an instance at the root, `/<package>`,
  and bind each hierarchical reference — an object with a path rather than storage — to the
  slot of the object it names, once the whole tree is expanded (DESIGN_GUIDE.md section 5.1).
  A package with the top level's name is refused (ENZ3012): its variables would sit at the top
  level's path.
- Give each continuous write of a net that resolves — one with a bit driven twice, or a
  pulled or supply net with any driver — a **driver** with its own value in the arena, and
  split the net into **segments** with one set of drivers each. Every other signal has no
  driver state and is written straight into its slot.
- Bind every call a process can reach to a **subprogram instance** (DESIGN_GUIDE.md section
  4.1): a module's subprogram to the instance whose process calls it, a package's to the
  package's one instance, which a call reaching a package that holds objects is enough to
  create, and one holding no objects to none. Record what each process's callees read and
  write, and refuse a callee that writes a bit something drives, since its writes go through no
  driver.
- Reject the cases that would need features later milestones bring, such as port width
  conversion, rather than let them simulate to a plausible wrong answer.

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

**A port may drive another slot than it reads.** A connection that names a separate object
for the port's drivers (`eir::PortConnection::driveObjectIndex`) binds the child's port to two
slots: the one it reads, `objectSlots`, and the one its drivers write, `objectDriveSlots`. A
port collapsed onto it below takes both, so whatever drives it at any depth drives the one
slot. Everything about drivers — overlap, resolution, segments, a driver's starting value —
is of the slot written, and everything about reading — values, sensitivity, events — of the
slot read. A VHDL inout port associated through a conversion or with part of a signal is the
one case: its driving value and its effective value differ (DESIGN_GUIDE.md section 3.7).

## Port collapsing across a language boundary

Collapsing does **not** apply across a language boundary. Section 3.5 defines a
cross-language port connection as a zero-delay continuous assignment that consumes one
delta cycle, and that is a mixed-language matter for M7.
