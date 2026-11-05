# Enzan RTL Simulator — Design and Development Guide

**Status:** Living specification. Revised as the project evolves.
**Audience:** everyone working in this repository, people and AI coding agents alike.
**Authority:** This document is the single normative specification for Enzan. Documentation for
users lives separately and is derived from it, never the reverse.

---

## 0. How to use this document

Anyone working in this repository, a person or an agent, MUST read sections 0, 12, 13 and 17 before
making any change, and MUST read the section covering the component they are modifying.

The keywords **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT** and **MAY** are used in their RFC
2119 sense. A **MUST** is an invariant: code that would violate one is a conflict to raise, not to
work around.

**The update rule.** A change that alters behaviour this document describes updates the
corresponding section in the same commit. A commit that makes this document wrong is a defect,
whether or not the code works.

**The escalation rule.** A decision this document leaves open is marked *(open)*. A task that
depends on one states the options and asks rather than picking one silently.

**What this document holds** is the design: its decisions, the reasons for them and the rules that
follow from them. How a finished feature works in detail is for the code and its comments to say;
this document is not a second copy of them.

**What a change adds** to this document is a decision it makes, and nothing else: a choice of
scope, a deviation from a standard or from the reference simulators, a rule a later change could
break unknowingly, or the reason that keeps a decision from being reopened. What the change does,
and how, is for its commit message to record, and what a standard already says is not restated
here. The document grows by its decisions, not by its features.

**Citing this document.** A comment states its own reason. It also cites this document, as
`DESIGN_GUIDE.md section N.M`, only where the code enforces a rule made here and a change made
locally could break it unknowingly. Documentation, and anything a user reads, never cites it; the
one exception is `CONTRIBUTING.md`, which sends developers here. Section numbers are stable: a
change that renumbers a section updates every citation of it in the same commit.

---

## 1. Project overview

### 1.1 What Enzan is

Enzan is an event-driven, mixed-language RTL simulator for SystemVerilog and VHDL, with an
interactive graphical debugging environment. The name is Japanese and carries two readings: *distant
peaks* (遠山), for the horizon of a screen full of waveforms, and *calculation* (演算), for the work the
simulator performs.

Commercial simulators are excellent and prohibitively expensive. In the open-source world, Verilator
is a fast cycle-based translator with no debugging GUI, Icarus Verilog is event-driven but slow, and
GHDL and NVC simulate VHDL alone. Nothing offers *fast, event-driven, mixed-language simulation
behind a usable interactive GUI*, and that combination is Enzan's reason to exist.

### 1.2 Design priorities, in order

When two goals conflict, resolve the conflict in this order:

1. **Correctness of simulation semantics.** A wrong answer is worse than none, and semantic bugs are
   the hardest defects for users to detect. Language semantics are not traded for speed, and
   anything unsupported is refused by name rather than mistranslated.
2. **Usability and debuggability.** The GUI, the diagnostics and the command line are the product as
   much as the kernel is.
3. **Performance.** Enzan must handle real designs, but never by guessing.
4. **Language breadth.** Exotic language features come last.

### 1.3 Explicit non-goals

The following are **out of scope** and MUST NOT be implemented without an explicit decision to
change this list:

- Gate-level timing simulation: SDF back-annotation, timing checks and VITAL. The Verilog that
  vendors' cell libraries are written in is in scope, read for what a functional simulation needs
  of it: gate and user-defined primitives, drive strengths and specify blocks, whose module paths
  delay outputs as the standard says, since the vendors' own simulators show those delays.
- Power-aware (UPF/CPF) and mixed-signal (Verilog-AMS, VHDL-AMS) simulation.
- IP encryption (IEEE 1735). Decrypting needs a vendor to wrap its keys for Enzan's, which takes
  an agreement with each vendor, and a private key in a GPL binary is no secret. A source holding
  an encrypted envelope is refused by one error saying so, never read around.
- Windows and macOS support. Enzan targets Linux only.
- Synthesis, formal verification, equivalence checking.
- UVM, constrained-random stimulus and functional coverage, until M9+ (section 18).

### 1.4 Licensing

Enzan is licensed **GPLv3**, as a consequence of linking NVC, which is GPLv3, for the VHDL frontend.
The licence follows from the choice of 5.2 and does not justify it: GHDL, the only serious
alternative, is copyleft too. A re-examination of the VHDL frontend is argued on the grounds of 5.2,
and the licence follows.

| Dependency | License |
|---|---|
| NVC (VHDL frontend) | GPLv3, which sets the project's licence |
| slang (SystemVerilog frontend) | MIT |
| Qt 6 | LGPLv3 |
| Tcl (GUI theme and signal lists) | BSD-style |
| GoogleTest | BSD-3 |
| FST | permissive |

Every source file MUST begin with an SPDX identifier, in its language's comment syntax:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
```

Third-party code vendored into `third_party/` MUST keep its original license text and MUST be
recorded, in the user documentation, with its license, upstream URL and pinned revision. So MUST
a file translated from another project's, such as a testbench rewritten in the other language: a
translation is that project's work as well as Enzan's, so it keeps that project's notice and
licence rather than taking Enzan's.

---

## 2. Architecture

### 2.1 The three phases

**Analysis** turns one design unit at a time — a module, package, interface, entity or architecture
— into Enzan's internal representation (EIR, section 4), and may store the result in a library. Its
output is independent of any particular top level, and so is reusable across runs and designs.

**A parameterised unit is analysed once per set of values the design uses.** slang is an elaborating
frontend: it type-checks a body in the context of the values it is given, because a type can depend
on a parameter, and offers no body generic over them. The VHDL adapter folds generics the same way
(5.2). So a unit becomes one unit per set of values, and per set of places its hierarchical
references lead (5.1), named for them (8.2): the same values and places make the same unit wherever
it is produced, but a library is re-runnable rather than reusable at values it has never seen.
Rebuilding a generic unit out of slang's elaborations was rejected: it would be checked against
slang only at the values slang saw, and trusted at the others.

**Elaboration** takes a top level and the analysed units, binds each instance to the unit for its
values, builds the instance tree, binds ports, computes the flat state layout and produces an
executable image. It is per-design and cannot be cached across top levels. Optimisation passes —
inlining, selective flattening, process merging, partitioning — belong here.

**Execution** runs the elaborated image under the simulation kernel.

Analysis MUST remain per-unit and modular. Flattening at elaboration MUST remain available, because
a large fraction of achievable performance lives there.

### 2.2 Process separation

The simulation kernel and the GUI are **separate processes**, and MUST NOT be collapsed into one.
The kernel must be testable and scriptable with no display attached; a stalled or slow GUI must
never stall the kernel; and a local GUI must be able to drive a kernel on a remote compute server.

```
   ┌──────────────────────┐        Unix domain socket        ┌──────────────────┐
   │   enzan (kernel)     │ <──────────────────────────────> │   front end      │
   │  headless, batch     │      versioned binary protocol   │   Qt 6 widgets   │
   │  links no Qt         │                                  │   attach/detach  │
   └──────────────────────┘                                  └──────────────────┘
```

The GUI MUST be able to attach to an already-running kernel and to detach without terminating it.

**The front end pulls what it shows; the kernel never pushes a value.** A design records changes
faster than any window draws them, and a kernel sending every change would either queue them on the
socket without bound or wait for the window, which is the stall forbidden above. So while it runs
the kernel only says how far it has recorded, and the front end asks for the changes it lacks of the
signals it shows, one request at a time, each answered with a bounded number of bytes. It asks by a
change's number in its signal's record rather than by time, because a signal can change twice at one
time. What the window has not taken yet waits in the waveform database (7.1).

**The user does not invoke the two separately.** `enzan --gui` starts the front end (8.4). Two
processes is the architecture; two commands was never the point.

### 2.3 Component map

```
   HDL sources
        │
        ├── slang ──────────┐
        │                   ├──> frontend adapters ──> EIR (analysis output)
        └── NVC ────────────┘                               │
                                                            ▼
                                                  library store (optional)
                                                            │
                                                            ▼
                                            elaboration ──> instance tree
                                                            + state arena
                                                            + bytecode
                                                            │
                                                            ▼
                                     ┌──────────── simulation kernel ────────────┐
                                     │  scheduler · bytecode VM · value engine   │
                                     └───────────────────────────────────────────┘
                                        │             │              │
                                        ▼             ▼              ▼
                                   waveform DB    VPI / VHPI        IPC
                                        │             │              │
                                   VCD / FST       cocotb           GUI
```

---

## 3. Simulation semantics

**This section is normative and is the highest-risk part of the project.** A simulator whose event
scheduling does not match the language standards produces plausible-looking but wrong results.
Changes here MUST be accompanied by tests that pin the behaviour.

### 3.1 Time model

Simulated time is a **64-bit unsigned integer** counting units of a single global time precision.
Time MUST NOT be represented in floating point anywhere in the kernel.

The precision is the finest one the design asks for — SystemVerilog's `timeprecision` and
`` `timescale ``, and femtoseconds for a design with VHDL in it — with a floor of 1 fs, and never
coarser than the unit of a SystemVerilog module stating no time scale: `--timescale`'s, the
libraries', or else picoseconds. `--timescale` or a library fixes it instead (8.2), and a delay that
is no whole number of a fixed unit is rounded to the nearest and reported. Delays are converted as
they are translated, so the kernel never converts units at run time. A SystemVerilog delay counts
the unit of the scope stating it, rounded to that scope's precision first (IEEE 1800 section
3.14.2.1), and `$time` counts the calling module's unit.

At 1 fs the counter covers about 5.1 hours of simulated time; at 1 ps, about 213 days. Overflow MUST
be reported as an error rather than wrap silently.

### 3.2 Delta cycles

A delta cycle is a **zero-duration** iteration ordering evaluations at one simulated time, and any
number of them may occur at one time. A delta-cycle limit MUST be enforced, 10,000 by default and
configurable. On exceeding it the kernel MUST stop with a diagnostic naming the processes and
signals still active: a delta loop with no diagnostic is one of the most frustrating failures a user
can hit.

### 3.3 SystemVerilog event regions

Enzan implements the IEEE 1800 stratified event queue. The regions, in order within a time slot:

```
  Preponed  →  Active  ⇄  Inactive  ⇄  NBA  →  Observed  →  Reactive  ⇄  Re-Inactive  ⇄  Re-NBA  →  Postponed
```

The kernel drains Active fully; when Active is empty it promotes Inactive (`#0`) events to Active
and repeats; when both are empty it promotes NBA events. Only when Active, Inactive and NBA are all
empty does the slot move to Observed. The Reactive family iterates by the same rule, and the slot
ends after Postponed.

Preponed, Active, Inactive, NBA, Observed, Reactive and Postponed MUST be implemented. The
PLI-specific regions MAY be callback hook points rather than separate queues, provided the callbacks
fire at the correct point in the ordering (9.3).

A delayed write is scheduled for its time, not for a process. `a <= #d b` joins the NBA region of
the time step `d` later, and a continuous assignment with a delay writes at the start of that step's
Active region. That delay is inertial: a newer write from the same assignment replaces one still
pending, so a pulse shorter than the delay leaves no time step behind (IEEE 1800 section 10.3.3).

### 3.4 VHDL simulation cycle

Per IEEE 1076, a VHDL simulation cycle is:

1. Current time `Tc` is set to the next time `Tn`.
2. Every active signal is updated; events occur where the new value differs.
3. Processes sensitive to those events resume, as do processes whose `wait` timeout has expired.
4. Each resumed process executes until it suspends.
5. `Tn` is computed as the minimum of the next driver activation time and the next process timeout.
6. If `Tn == Tc`, the next cycle is a delta cycle.

**A signal assignment is a transaction of its process's driver.** A process has one driver for each
scalar subelement it assigns, and each driver a projected waveform of transactions (IEEE 1076
section 14.7.2), which the kernel keeps, because the language defines an assignment by what it does
to that waveform (section 10.5.2.2). The driver's transactions at or after the new one's time are
deleted, so of a process's assignments to one signal in one run the last is what happens; and an
inertial assignment also deletes those within its rejection limit before it, so a pulse shorter than
the limit never reaches the signal. An assignment to an element or a slice affects only the bits it
assigns. Treating each assignment as a SystemVerilog non-blocking update instead would give every
one of them its own event.

**A cycle begins with each batch of updates**, a delta cycle's or a time step's. A signal's `'event`
is whether it last changed in the running cycle, and its `'last_value` is what it held before that
change, which the kernel keeps only for the signals something reads it of.

A transaction for the current time takes effect in the next delta cycle, after every process of this
one has run. A later one takes effect when its time step begins, before any process resumes in it,
so a process woken by a timeout sees the values signals take at that time.

### 3.5 Mixed-language interleaving

**This is the single hardest correctness problem in the project, and it is under-specified in every
public source.** The rules below are Enzan's definition and are normative; where they prove to
disagree with commercial tools on real designs, this section is what gets updated.

- There is **one** kernel and **one** event queue. VHDL and SystemVerilog are not simulated by
  separate engines that synchronise.
- VHDL's signal update (3.4 step 2) is the beginning of the SystemVerilog **Active** region, and
  VHDL processes run within Active. A VHDL delta cycle is one full Active/Inactive/NBA iteration.
- A VHDL `wait for 0 ns` is not `#0`: it resumes in the next delta cycle, after that cycle's signal
  update, where `#0` resumes in Inactive before the NBA updates.
- A VHDL wait with a timeout and a sensitivity or a condition arms both: whichever comes first
  resumes the process and makes the other stale, and a timeout taken back is not a time step the run
  stops at.
- A VHDL postponed process resumes in **Postponed**. At initialisation it runs after the other
  processes and before the first delta cycle (IEEE 1076 section 14.7.5.2).
- **A port connection crossing the language boundary is a zero-delay continuous assignment that
  converts the value and consumes one delta cycle**, in each direction its mode needs: into VHDL by
  a transaction, into SystemVerilog through the NBA region and then a continuous assignment, so that
  a net it drives still resolves its drivers. Elaboration makes it from what the VHDL side's type is
  made of, so neither frontend converts anything. The delta is observable and will surprise people,
  so it MUST be documented for users.
- Values convert as 3.6 maps them: `std_ulogic` to four-state bits and back, `bit` and `boolean` to
  two-state bits, vectors of these left element to most significant bit, integers to `int` or
  `longint` by their range, arrays to unpacked arrays and records to packed structs field by field
  where the shapes agree. A port no rule covers is refused by name.
- **A value SystemVerilog drives weakly drives a VHDL signal weakly**, a 0 as `L` and a 1 as `H`,
  so that a pull-up yields to a pad's other drivers as it does on a net. A value is weak when every
  driver of the port in its module drives it weakly; one an instance inside drives, or a procedural
  write, crosses as strong.
- **An `inout` port across the boundary is two conversions**, split as 3.7 splits a port's driving
  value from its effective value: the inner port's driving value goes out as one driver of the outer
  signal, which resolves it by its own language's rule, and the outer signal comes back as the inner
  port's effective value, not as a driver. Were each side's value a driver of the other, a value
  driven once would be held for ever.

Mixed-language behaviour MUST be covered by tests against a reference simulator before it is
considered working. No free simulator runs both languages, so a design is checked by moving the
boundary: the DUT in one language driven by a testbench in the other must match the DUT under its
own language's testbench in Icarus Verilog or GHDL, transcript for transcript, the boundary's delta
aside (14.3).

### 3.6 Value representation

**The value representation MUST be four-state-shaped from the first line of code.** Retrofitting a
second value plane onto a single-plane implementation is a rewrite of the runtime, not an extension.

**Packed bit vectors** use two parallel bit planes, `aval` and `bval`, stored as arrays of
`uint64_t`:

| `aval` | `bval` | value |
|---|---|---|
| 0 | 0 | `0` |
| 1 | 0 | `1` |
| 0 | 1 | `Z` |
| 1 | 1 | `X` |

This is deliberately the encoding of VPI's `s_vpi_vecval`, which makes `vpi_get_value` and
`vpi_put_value` close to free.

**Two-state is an optimisation of the four-state representation, never a separate engine.** A vector
whose `bval` words are all zero is two-state, and the runtime SHOULD branch to specialised
operations in that case, as the interpreter does for the commonest operations on one-word values.

**VHDL's values are codes in the same vectors.** An enumeration, `std_ulogic` among them, is its
position in as many whole bytes as its values need — one byte for `std_ulogic`, `bit`, `boolean` and
`character` — and an integer or a physical value such as `time` is signed, 32 or 64 bits as its
range needs. A real, of either language, is its IEEE 754 double's 64 bits. Records and arrays are
flat (4.3), so a `std_logic_vector(7 downto 0)` is a 64-bit vector of eight codes, and everything
built on bit ranges — drivers, part stores, change detection, waveforms, VPI — serves VHDL
unchanged. What the bits mean is the object's encoding, which travels with it.

Conversion to four-state form happens at language boundaries, in the waveform database and in the
PLI, with `L`→`0`, `H`→`1` and `W`, `U`, `-`→`X`: a `std_logic_vector(7 downto 0)` shows in a
waveform as eight four-state bits. A separate kind of storage for VHDL was rejected, because each
mechanism above would have needed a second path kept in step with the first.

### 3.7 Drivers and resolution

**A driver owns a bit range of a signal, not the whole signal.** Each process that assigns a signal
owns a driver covering exactly the bits it assigns. A bit with one driver takes that driver's value;
a bit with more than one computes its value with a resolution function. The pattern that forces this
granularity is the ordinary one:

```systemverilog
logic [N-1:0] ready;
for (genvar i = 0; i < N; i++)
    channel u (.ready(ready[i]));
```

Every instance drives one bit and no other. Were a driver to own the whole signal, a parameterised
number of instances connected to an array could not be expressed at all.

**Overlap is what makes a signal multiply driven, not sharing a signal.** Drivers of one signal
whose ranges are disjoint are not in conflict and MUST NOT be treated as though they were.

- A **variable** (`logic`, `bit`, `reg`) has at most one continuous driver *per bit*. Disjoint
  part-drivers are legal; overlapping ones are an error rather than something to resolve.
- A **net** may have any number of overlapping drivers, resolved per bit by the table for its kind.
- A bit of a net that nothing drives holds its kind's default — `z`, or the pull value of `tri0`,
  `tri1`, `supply0` and `supply1` — not what a neighbouring bit resolves to. A driven bit is X until
  its driver first runs.
- A driver's range is fixed at elaboration. A part-select at a position that is not constant writes
  through the driver of every bit it could reach.

**Strength is two classes, not the standard's eight.** A driver's strength for each value is
*strong* (`supply`, `strong`) or *weak* (`pull`, `weak`, and a `tri0` or `tri1` net's own pull).
Strength is declared, so it is fixed at elaboration and held by the driver in the image, never by
its value in the arena. A bit resolves among the drivers of the strongest class that contributes to
it, by its net kind's table. **What the classes cannot tell apart is refused, not guessed**:
drivers whose strengths the standard orders and one class holds, pull against weak or supply
against strong, would resolve to `x` where the standard has a winner, and are an elaboration error.
A `highz` strength, the resistive and bidirectional switches and `trireg` are refused, and so is
`vpiStrengthVal`, rather than reporting a strength Enzan does not hold. The eight-level model was
rejected because nothing in the vendors' libraries Enzan reads needs it.

**An override outranks what drives a signal, and is not a driver of it.** A procedural `assign`
or a `force` holds its target until `deassign` or `release`, every other write of it discarded
meanwhile, a deposit's included, and a force stands over an assign (IEEE 1800 section 10.6). A
process the frontend adds beside the statement writes the value again whenever what it reads
changes. The objects a process overrides are declared beside its write set rather than in it, since
a forced net's drivers are still its drivers. They keep values of their own only where a `force` in
the design names the signal, so after a force from outside a lone driver takes its net back when it
next writes.

**VHDL resolution functions are user code**, compiled to EIR like any subprogram, and the runtime
MUST NOT assume resolution is a table. A function's parameter is an array as long as the bit has
drivers, which only elaboration knows, so elaboration counts the drivers and the frontend then
specialises the function for each count. The kernel calls it with the drivers' values, the first
driver's leftmost; a VHDL driver starts at the initial value of the signal its process names. A
function of composite values resolves whole values, so a process driving part of one is refused.

**Resolution is bypassed for a lone driver only where it is known to change nothing**: the built-in
table of every net kind but the pulled and supply ones, and a VHDL function proven to return a lone
driver's value by calling it once with each value of an element type of sixteen values or fewer, as
`ieee.std_logic_1164.resolved` is. Single-driver bits are the hot path (13.4), and a signal all of
whose bits bypass resolution MUST bypass it as a whole. **Any other function is called for a lone
driver too**: VHDL defines a resolved signal's value as its function applied to its sources, however
many there are (IEEE 1076 section 14.7.3.2).

**Drivers are known before anything runs.** A process's write set records each object and the bits
it writes, so elaboration finds where drivers overlap, and the runtime keeps no record of which
driver owns which bit.

- **A signal with no bit driven twice carries no driver state**, unless its resolution applies to a
  lone driver: a write goes straight into the signal.
- **A signal with a bit driven more than once is split into segments**, maximal runs of bits with
  one set of drivers. Each of its drivers has its own value in the arena over its range. A write
  through a driver resolves the segments it covers and no others, and writes the result through the
  ordinary change-detected path, so a contended bit wakes nothing unless its resolved value moves.
- **Whether a store goes through a driver depends on the instance, not the body.** One compiled body
  serves every instance, and only some of them may land on a resolved net, so a store names its
  target by its object's position among the objects the process writes, and each bound process maps
  that position to the signal or to its drivers of it.
- **What a port's drivers write may differ from what it reads.** A connection may name a second
  object for the instance's drivers of a port, and elaboration binds the port's reads to one slot
  and its drivers to the other. That is how a VHDL `inout` associated through a conversion or with
  part of a signal keeps its driving value apart from its effective value (5.2).

A per-bit table of owning drivers was rejected because it holds at run time a fact fixed at
elaboration and pays a lookup on every write. Routing every write through a driver, whether or not
anything overlaps it, puts a step on the path this section requires to be bypassed.

### 3.8 Determinism

**Simulation results MUST be bit-identical across runs of the same build on the same inputs.**
Without it, waveform diffing and regression triage are impossible and intermittent failures cannot
be reproduced.

- Process execution order within a region MUST be a stable insertion order (FIFO). It MUST NOT
  depend on pointer values, allocation addresses, or the iteration order of hash-based containers.
- Code MUST NOT iterate `std::unordered_map` or `std::unordered_set` in any way that affects
  simulation results, diagnostic ordering or generated output. Use ordered containers or sort
  explicitly where order is observable.
- All randomisation MUST draw from an explicit seeded generator, and the seed MUST be settable from
  the command line.
- The kernel is single-threaded. Any later parallelism MUST preserve determinism.

### 3.9 Initialisation and X-propagation

At time zero, four-state variables and signals start at `X` (SystemVerilog) or `U` (VHDL
`std_logic`) unless explicitly initialised. X-propagation follows the language standards:
pessimistic in expressions, with the standard-defined exceptions for `case`, `casex`, `casez` and
the conditional operator.

A two-state simulation mode, where `X` is replaced by a deterministic `0`, MAY be offered as an
explicit user-selected mode. It MUST NOT be the default and MUST be announced in the transcript when
active, because silent X-suppression hides real design bugs.

---

## 4. The Enzan Internal Representation (EIR)

EIR is Enzan's own language-neutral representation, the contract between the frontends and
everything downstream. A shared representation such as hdlConvertor was considered and rejected: it
is a parser, with no elaboration, type resolution or generic evaluation, and elaboration is the
difficult part of a frontend.

### 4.1 Layers

**EIR-S (structural)** describes design units and hierarchy: modules and entities, ports, parameters
and generics, instances, signals and nets, processes, continuous assignments, subprograms.

**EIR-B (behavioural)** describes process and subprogram bodies as a control-flow graph of
three-address instructions over typed values, including signal reads, blocking and non-blocking
drives, and the suspension primitives (`wait` on an event, a time or a condition).

**A subprogram is a body that is called rather than scheduled**: the same control-flow graph a
process is, over registers of its own, with parameters, a result, and the read and write sets of
what it reaches beyond its parameters (4.2). It belongs to the definition declaring it, and a call
names it by that definition and its name. Nothing in it is particular to VHDL.

- **A subprogram is specialised by the lengths of its unconstrained parameters**, and by the scalar
  values its own widths are computed from, so that every width in EIR stays fixed: `numeric_std."+"`
  on 8-bit and on 32-bit operands is two bodies. A string whose length only the run knows is passed
  in scratch, to one body for every such length.
- **A call's frame lives on one stack the kernel owns.** A subprogram never suspends — one that
  waits, or takes a signal parameter, is inlined into its caller instead — so every frame is popped
  before its process suspends, and one stack serves the single-threaded kernel. It is sized at
  elaboration for the deepest chain of calls the design can make; a recursive design is given a
  configurable depth limit, past which the run stops with a diagnostic, as the delta limit does.
- **A caller's read and write sets include its callees'**, bound to the instances they touch.
- **A string whose length is known only when it is computed** — a subprogram's `string(1 to n)`, a
  slice `s(1 to i)` — lives in scratch the kernel owns: a register holds its handle, and the kernel
  its bounds and characters. A subprogram releases what it made as it returns, a statement what it
  made for a call once the call is done, and the kernel all of it whenever a process suspends, so a
  subprogram holding one cannot wait. Only strings are held so yet.

### 4.2 The core invariant

**Every EIR process MUST be expressible as a pure function over an explicit state arena, with a
declared read set and write set.**

This is the single most important structural rule in the project. It keeps later optimisation open:
static partitioning for threading, JIT compilation, activity-based scheduling and checkpointing all
depend on knowing exactly what a process touches. Code that lets a process reach arbitrary global
state closes those doors permanently and MUST be rejected.

### 4.3 State layout

The elaborated design's entire mutable state lives in a **single contiguous arena**. Every signal,
variable and driver is given a byte offset at elaboration, and EIR and bytecode refer to state by
offset, never by pointer. Checkpoint and restore become a memcpy; the waveform database reads values
in place; a JIT burns offsets in as constants; and locality can be tuned by reordering the layout.

**Aggregates are stored flat.** A fixed-size unpacked array or struct, like a VHDL array or record,
is the vector of all its bits, the left element and the first field highest, exactly as its packed
equivalent would be. An element or a field is a window of those bits, so every rule about bits
applies to it unchanged and an element is read in place. The declared dimensions are kept beside the
object, down to a packed array's element, for VPI, the waveform database and the wave window.

**Driver state** occupies the arena only where 3.7 gives a driver a value of its own. The segment
tables and the store routing are fixed at elaboration and belong to the image rather than to the
arena. Nothing about drivers is sized at run time.

**An access value is a handle.** What VHDL's `new` allocates has a number and size known only as the
design runs, so it lives in a heap the kernel owns beside the arena, and an access value is an
integer handle into it, numbered in allocation order with the last freed reused first, so that a run
allocates the same handles every time (3.8). The heap is state a process reaches, so a process that
dereferences a handle has it in its read or write set (4.2).

**std.textio runs natively.** Its body grows lines through strings of run-time lengths, which
fixed-width registers cannot hold, so textio's subprograms and VHDL's predefined file operations are
system functions the runtime runs over its heap and its files. A file of a type other than text
holds its values' bits in Enzan's own format, as each simulator has its own. A line is indexed from
1 after a `read` takes characters off it, as NVC indexes one; GHDL keeps the indexes of what
remains, so a design reading a line's bounds after a `read` is no case to hold to GHDL.

### 4.4 Libraries

Analysis output may be serialised into a library directory (8.2).

**A library has a logical name as well as a path**, because a design refers to a library by name:
`library ieee;` resolves against a name. The name is fixed when the library is created and stored in
the library's own index, so that the two ends of a build cannot disagree about it.

**An entry is named for the source it holds, and its key lives inside it.** A path that were a hash
of the source could not be named by a Makefile, and a build that cannot predict its own outputs
cannot be incremental; a key that were only the path could not tell a stale entry from a current
one. So the name is what `make` depends on, and the key is what Enzan checks.

**Everything that changes what analysis produces belongs in the key**: the source text, the files it
included, the macro definitions, the include path, the time unit, the format version and Enzan's own
version. A define outside the key is a change the library cannot see, and the result is a simulation
that runs and is wrong.

**The format version MUST be bumped when the frontend's translation changes**, not only when the
encoding does: a unit from an older translator decodes perfectly and simulates differently. A
version mismatch, or a truncated or corrupted entry, which a checksum detects, MUST cause clean
re-analysis, never a crash or a half-decoded result.

**Using a library is opt-in, and there is no implicit one.** Nothing is written or consulted until
the user names a library. A cache that can change a simulation result must be one a user can stop
using without first having to find it.

**VHDL's standard libraries are part of the tool, not a cache.** `std`, `ieee` and NVC's own `nvc`
are analysed from NVC's sources when Enzan is built and found from the executable's own directory
(11.5), without being named, since `library ieee;` has to resolve with no flag. Nothing a user does
writes to them. A library the user names `ieee` replaces them.

**A library holds VHDL as NVC's analysed units, not as EIR.** A VHDL unit is analysed against the
units analysed before it, which only NVC's units can serve, and a design is translated into EIR for
the generic values its run gives it (5.2), which no library can know. An entry holds a source's key
and the units it declares. As in NVC and GHDL, a library accumulates: analysing a source again
replaces its units, and a unit no source declares any more stays until `lib clean`.

**A SystemVerilog hit is all or nothing**, a property of the frontend rather than of the store.
Every source goes to one slang compilation, because a module cannot be type-checked without the
packages it imports, so either every entry is current and nothing is analysed, or everything is.
Entries are kept per source file nonetheless, the shape a per-unit frontend would want; only the hit
rule would change.

---

## 5. Frontends

A frontend's only job is to turn source text into EIR. Frontends MUST NOT flatten hierarchy, make
scheduling decisions, or allocate runtime state.

### 5.1 SystemVerilog via slang

[slang](https://sv-lang.com/) is MIT-licensed, designed to serve as a frontend for simulators, and
the most standards-compliant open-source SystemVerilog frontend. It lexes, preprocesses, parses,
type-checks and elaborates; Enzan's adapter walks its elaborated AST and emits EIR, and everything
from EIR onwards is Enzan's own work. slang requires **C++20**, which sets the project's language
floor.

- **A generate block is part of the name of everything inside it**, joined with a dot:
  `/tb/gen[0].u_dut/count`, the block named as IEEE 1800 section 27.6 names it. Without it, two
  iterations instantiating `u_dut` would be two instances with one path. Changing how a block is
  named changes the EIR produced, so it bumps the format version (4.4).
- **A hierarchical reference is an object with a path and no storage**, bound by elaboration to the
  object it names, so reads, writes, sensitivity and drivers all see the real signal. The path is
  relative — `core/state`, `../u_peer/ready` — so that one definition serves every instance of a
  parameterisation whose references lead to the same places; one whose references lead elsewhere
  is a definition of its own (2.1). **A reference into another top level is absolute**, as a
  package variable's is, so that it is the same from every instance.
- **A package's variables are a definition of their own, `<package>::`**, with one instance at the
  root, `/<package>`, created when something refers to one, and are referred to by the absolute path
  `/<package>/<variable>`. A package's constants, types and subroutines need no instance.
- **An interface instance is an instance**, its signals stored under its own path. A member reached
  through an interface port is a reference through that port, and each instance records what its
  interface ports are connected to, so two instances of one definition on two interfaces each reach
  their own. A modport restricts what a module may touch and changes nothing about where a signal
  is.
- **A function or task is inlined into the process that calls it**, so a task's delays suspend the
  caller and the caller's read and write sets are exactly what the call touches. A recursive call is
  refused, unless slang folds it to a constant. Calling a SystemVerilog function as an EIR
  subprogram instead would be a change to this frontend alone.
- **A gate is a continuous assignment of its outputs**, at the strength its instance declares
  (3.7) and with its delay as a continuous assignment's, so EIR holds no primitive. Where the
  standard gives `L` or `H`, as a tristate buffer with an unknown control does, it drives `x`,
  which four states cannot avoid. **A MOS switch is one too**, at the strength of its input where
  the input's module fixes it, every driver of it of one class; any other input is refused, since
  what the switch passed would be whichever driver decides the input at the time. **A user-defined
  primitive is one too**, by its table, which the frontend encodes as a constant and the runtime
  matches by the layout `eir/Udp.h` gives both. Inputs changing together are a race the standard
  leaves open, and Enzan takes them in port order.
- **A real is a double**, `shortreal` without its reduced precision.
- **A named event is a bit its trigger toggles**, so that what waits on it sees it change.
- **An event control on an expression waits on an object a continuous assignment keeps at the
  expression's value**, so it resumes when the value changes or makes its edge (IEEE 1800 section
  9.4.2), as `@(w)` with `assign w = expr` would: a value held only between two writes of one run
  of a process is no event. Icarus Verilog resumes on any change of an operand instead, so it is no
  reference for these. An expression of a subroutine's own variable is refused.
- **A specify block is read for what a functional simulation needs of it.** A timing check is not
  run (1.3), and a run instantiating one says so once.
- **A module path delays its output as Icarus Verilog's `-gspecify` does**, by the standard's rule
  (IEEE 1800 sections 30.4 to 30.7), an unknown condition holding no path, but a path to some bits
  of an output is theirs alone, where Icarus applies it to the whole output. The frontend gives each
  run of an output's bits with the same paths a process of its own, which writes the port from a
  copy the body writes, so a body whose paths have no delay pays nothing. Delays by transition on a
  run of more than one bit are refused, since its bits may make different transitions at once, as
  are `ifnone` with a delay, pulse limits other than zero and pulse styles.
- **Two module paths the standard refuses are warnings**, since models written for other
  simulators rely on them and what they mean is plain: a parameter in a path's condition (IEEE
  1800 section 30.4.4), as Icarus Verilog takes it, and a parallel path from one bit to a vector
  (section 30.4.5), which can only join the bit to every bit of the output.
- **A `defparam` on an instance declared beside it**, in the same module or generate block,
  **becomes that instance's parameter value assignment** before slang elaborates. An untyped
  parameter takes the type of its final value (IEEE 1800 section 6.20.2), which slang gives an
  override on the instance but not a `defparam`, so a longer string would be cut to the default's
  width. Any other `defparam` whose value would be cut so is refused.

### 5.2 VHDL via NVC

[NVC](https://github.com/nickg/nvc) provides the VHDL frontend. It supports almost all of VHDL-2008
and is actively maintained.

**Why NVC and not GHDL.** GHDL exposes its analysis as a library with third-party consumers, and its
risk of API drift is genuinely lower. NVC wins on everything else. GHDL is Ada, which brings GNAT
into every build and the Ada runtime into a C++20 binary, while NVC is C11. NVC's analysis can be
built in part, without LLVM, where `libghdl` is built whole or not at all. NVC's `tree_t` has
uniform, checked accessors, where GHDL's `Iir` is an untyped handle over hundreds of aliased node
kinds. And NVC covers nearly all of VHDL-2008, with a maintainer responsive on frontend defects.

**The integration.** NVC is a complete simulator, not a frontend library, with no stable API for
"give me the elaborated VHDL". So:

- NVC is a git submodule pinned to a revision, and Enzan builds the subset of its files that
  analysis needs with its own CMake rather than NVC's autotools.
- Its lexer, parser and semantic analysis are used, and the `tree_t` is consumed after its
  simplification and bounds checking. Its lowering, code generation, JIT, runtime and scheduler are
  not.
- **Its constant evaluator is replaced.** NVC folds locally static expressions by running them on
  its JIT; Enzan supplies the two functions the simplifier calls instead, so the trees keep those
  expressions and the adapter folds them as it translates.

Two costs follow, and neither is an API problem. NVC's runtime implements much of the standard
packages as builtins and foreign functions, so **Enzan supplies its own implementation of whatever
the trees leave predefined or foreign**. And **semantics that settle at elaboration** — generic
folding, subtype resolution, arrays sized by generics — are Enzan's work rather than NVC's.

**The adapter folds generics.** An entity and its architecture are translated once per set of
generic values a design gives them, and of the bounds its unconstrained ports take from their
actuals, into a unit named for them as a SystemVerilog specialisation is (8.2): subtypes sized,
generate statements expanded and constant functions run as it goes, from the top down the hierarchy.
EIR holds no expression over a generic. The alternative, units carrying expressions over their
generics for elaboration to evaluate, would have made every width an expression and generate
statements conditional structure — the largest change to EIR, elaboration and VPI anything could
make — to buy libraries reusable at values no design asked for. A VHDL library keeps NVC's units
instead (4.4).

**NVC runs in a child process**, forked for each analysis. NVC keeps its state in globals and ends
the process on a fatal error, which a design naming a library nobody built is enough to cause; in a
child that is a diagnostic, and every analysis starts from NVC's initial state. **The child
translates as well**, and sends the parent EIR in its serialised form (4.4), so NVC's trees never
reach the process that simulates. It translates the hierarchy under the top, so a unit nothing
instantiates cannot stop a design; with no top named it is the one entity nothing instantiates, and
with several it names them and translates nothing.

**Sources are analysed in dependency order**: each after the sources declaring the primary units it
names, in the order given otherwise, so a file list need not put a package before its users.

What a VHDL design becomes in EIR, in outline:

- **A process** loops for ever, its variables registers. It is a **driver** of each signal it
  assigns (3.7), so two processes assigning one unresolved signal are refused.
- **Arrays and records are flat** (4.3), and an index known only at run time is checked against the
  bounds, as VHDL's index check is.
- **A function is a subprogram translated once per shape** it is called with (4.1); a procedure, a
  subprogram taking a signal, and a pure function that is a single `return` are inlined.
- **Generate statements and blocks are flattened** into their architecture and named inside it,
  `g(0).u`. **A component binds by default** (IEEE 1076 section 7.3.3), or as a configuration
  specification says.
- **A port associated with a whole signal is that signal's storage.** Any other association goes
  through an object of the instantiating unit, kept equal to its actual by a process the kernel runs
  as part of the signal update. An `inout` so associated has two such objects, its driving value and
  its effective value (3.7). A port of mode out, inout or buffer is always a source of its actual.
- **Constant functions run in the adapter.** A call that does not fold where a static value is
  needed is refused, naming the call and the statement its run stopped at.
- **The IEEE packages are translated from their own bodies.** `report` and a failed `assert` report
  their severity, and failure, or the severity `--assert-level` names, ends the run.

**Across the language boundary.** Both frontends analyse into one design. An instance of the other
language's unit names a placeholder and asks the other frontend for the unit its values make, and
the session links the two by port name.

- SystemVerilog instantiates a VHDL entity by its name, the entity's interface given to slang as the
  module it would otherwise not find, so that the instance type-checks. The architecture is the
  entity's default one, since choosing another takes a `config`, which Enzan does not read.
- VHDL instantiates a SystemVerilog module through a component of the module's name, bound to the
  module when no entity of that name exists — the default binding of IEEE 1076 extended to the other
  language, as commercial tools extend it. The component's generics are the module's parameters,
  but for one the module lacks and no instance maps, which a vendor's component may declare, and a
  boolean meeting a parameter that takes a string is `"TRUE"` or `"FALSE"`, which is how vendors'
  models write a boolean.
- Each side specialises for the values the other gives, so the session alternates between the
  frontends until neither asks for a unit the other has not given, with a bound on the rounds so
  that a hierarchy recursing through both languages is reported rather than followed for ever. A
  VHDL port whose width depends on generics takes the width the translation for the instance's
  values gives it.

**Fallback.** If the adapter proved untenable, the alternative would be co-simulation with NVC as a
separate process over VHPI. That is slower and makes the delta semantics of 3.5 considerably harder,
so it is a fallback and not a plan.

### 5.3 Adding a frontend

A new language frontend is exactly one directory under `src/frontend/`, exposing a single entry
point that takes source files plus options and returns EIR design units or diagnostics. A frontend
that needs anything else from the rest of Enzan is a design smell, to be raised rather than worked
around.

---

## 6. Execution engine

### 6.1 Bytecode virtual machine

EIR-B is compiled to a **register-based bytecode**, not a stack-based one: it takes fewer
instructions per operation and maps directly onto both real registers and LLVM IR.

- Each process has its own register file, which holds wide values too.
- A call pushes the callee's register file onto the kernel's one call stack (4.1), and a return pops
  it. The lowering is an LLVM function per subprogram and a `call` to it, which is why a subprogram
  never suspends: an LLVM function cannot return to the scheduler half-way and be resumed.
- A register file starts as its body's image: zero, but for each register whose one writer is a
  constant load, which holds the constant from the start.
- The dispatch loop SHOULD use computed goto rather than a `switch`.
- **Every opcode MUST have an obvious, documented lowering to LLVM IR.** The JIT (6.3) is meant to
  be a mechanical translation of the interpreter, and an opcode that cannot be lowered mechanically
  blocks it. An opcode added without its lowering sketch is incomplete work.
- Opcodes address state by arena offset (4.3).

### 6.2 Scheduler

The scheduler implements section 3:

- A time wheel for future events, hierarchical or a calendar queue once a benchmark justifies one.
- Per-region FIFO queues within the current time slot.
- **Activity-based** signal update: a process is scheduled only when a signal it is sensitive to
  actually changes value, not merely when it is assigned. This is one of the largest single
  performance wins available and MUST be present.

### 6.3 JIT (deferred to M8)

An LLVM ORC-based JIT backend is the intended path to the performance target. The interpreter comes
first, both to validate semantics and to be the reference the JIT is differentially tested against.
The JIT is a backend behind a flag, never a replacement for the interpreter.

### 6.4 Parallelism — realistic expectations

Thread-parallel event-driven RTL simulation has a thirty-year record of disappointing results:
synchronisation costs more than the work performed per event. What pays, in order of confidence:

1. **Parallel analysis and elaboration.** Design units analyse independently.
2. **Static partitioning of the synchronous subset**, as Verilator's threaded mode does: perhaps
   2–4× on large designs, viable once the read and write sets of 4.2 exist.
3. **Thread-parallel event-driven simulation** (Chandy–Misra, Time Warp): research-grade, rarely
   pays on RTL, not planned.

**The kernel is single-threaded.** A cache-friendly single-threaded loop outperforms a naively
parallel one, and determinism (3.8) comes free. The invariant of 4.2 exists to keep option 2 open,
not to justify threading the kernel now.

---

## 7. Waveform database

### 7.1 Requirements

- **Selective capture.** Only signals in the logging scope are recorded: on a large design,
  recording everything exhausts memory within seconds of simulated time. The scope is a *list* of
  paths, each a subtree or a single signal. A signal the GUI shows is recorded from when it is first
  shown, whatever the scope, because the GUI reads what it shows from the database (2.2).
- **Live reading.** The GUI displays a waveform while the simulation progresses, so the database
  MUST support concurrent append and read. It is where a change waits until the GUI takes it.
- **Random access by time**, to redraw any zoom window without replaying from the start.
- **A configurable memory budget**, with spill to disk rather than unbounded growth.
- **Only the names the reader wrote.** An object a frontend introduces is named with a `$` and never
  declared; a signal is recorded under the names the source gave it, and storage no written name
  reaches is not recorded at all.
- **A width limit.** A change records the signal's whole value, so no signal wider than a limit is
  recorded, 65536 bits unless `--wave-max-bits` says otherwise, and whatever opened the file or the
  database warns of what it left out: a signal missing from a waveform otherwise looks like one
  nobody asked for.
- **Edge navigation.** The next and the previous transition of a signal from a time are one query
  each, not a scan: snapping a cursor and stepping it from edge to edge (10.7) are what a reader
  does most.
- **Finding a value is a scan, done where the values lie.** No index says when a signal holds a
  value, since one would cost memory for every change of every signal to answer a question asked
  now and then. So the database compares its stored values in place under a mask, a few
  nanoseconds a change, and steps the signals a search asks about together through their changes
  in time order: a search of ten million changes takes tens of milliseconds, where reading them out
  one at a time took two seconds, and many times that for a row joined from many.
- **A search keeps nothing.** It holds a cursor a signal while it runs, and reads chunks through
  the memory budget like any other query, so searching a group of a thousand signals stays inside
  the budget and writes nothing to disk.

### 7.2 Structure

Per-signal append-only chunks of value changes, plus a time index for random access. Values are read
from the state arena (4.3) at capture time, so the changes within a time slot collapse into the
value it settles on.

### 7.3 Export formats

- **VCD**: universal interchange, human-readable, and what differential testing compares (14.3).
- **FST**: compressed, far more practical for large designs, readable by GTKWave and Surfer.

Export is not an afterthought: VCD and FST remain the interchange path permanently, whatever the GUI
can do. A waveform file leaves memories out, as VCD does.

---

## 8. The command line

**The command line is Enzan's control interface, and there is no other.** No embedded scripting
language and no command vocabulary of the tool's own: what composes invocations is the shell,
`make`, or whatever the user already builds with. An interface only the tool can speak has to be
kept in step with everything the tool can do, while a parameterised command is composable the moment
it exists. So there is no way to stop a running simulation and poke at it from a script: interactive
inspection is the graphical front end's job (8.4), and programmatic control is VPI's (section 9).

### 8.1 Shape

One executable with subcommands, rather than one per phase:

| Invocation | Does |
|---|---|
| `enzan analyse [options] <sources…>` | analyses sources into design units |
| `enzan elaborate [options] <top>` | elaborates and reports; simulates nothing |
| `enzan run [options] <top>` | elaborates and simulates |
| `enzan lib create\|list\|clean <dir>` | manages an analysis library |
| `enzan [options] <sources…>` | analyses and runs in one process |

The last form is the shorthand every small case wants. The first word is read as a subcommand only
when it is one of the names above, so a source file called `run.sv` is still a source file.

- **`elaborate` persists nothing.** It answers "does this design build", and MUST NOT be a
  prerequisite of `run`: an elaboration on disk would be a second serialised format to version and
  invalidate, and elaboration is not the expensive phase.
- **An option is refused where it means nothing.** A simulation's options given to `analyse` or
  `elaborate`, `--wave-list` without `--gui`, and a command line that gives nothing to work on are
  usage errors, each with a message saying which, rather than taken and dropped: a waveform asked
  for and never written looks exactly like one written somewhere else.
- **`--top` is repeatable, and several top levels are several roots**, side by side as a
  package's instance sits beside the top (5.1), the first the design's own. With none named,
  several candidates are refused rather than all taken, since a module nothing instantiates is as
  often one left over as one meant.
- **A top level's parameters are set with `-G NAME=VALUE`**, or `-GNAME=VALUE`. slang applies an
  override while analysing, so it is in a library entry's key. One that names no parameter is
  reported, since a misspelt override is otherwise invisible.
- **Random numbers are drawn from `--seed N`**, 0 by default (3.8). `$random` is IEEE 1364's
  reference generator, bit for bit, so a seed draws in Enzan what it draws elsewhere. `$urandom` and
  `$urandom_range` draw from a generator per process, derived from the seed and the process's name,
  so what one process draws does not change when another starts drawing (IEEE 1800 section 18.14).
- **`--run` takes a length of simulated time**: a count of the run's time unit, `--run 100`, or a
  time with its own unit, `--run 10ns`. The run's unit is the design's to settle (3.1), so a time
  with a unit is converted once it is known, and one that is no whole number of it is refused rather
  than rounded. `--run all` runs until the design finishes, as no `--run` does.

**Exit statuses are distinct enough to branch on**, because a Makefile and a CI job need to tell a
broken invocation from a broken design:

| Status | Meaning |
|---|---|
| 0 | success |
| 1 | the command line was wrong |
| 2 | analysis or elaboration failed |
| 3 | the simulation ran and the design failed |
| 4 | the window `--gui` started crashed or exited with an error |

Conflating 2 and 3 is the mistake to avoid: a regression script that cannot tell "the tool could not
compile this" from "the testbench reported an error" reports both as the same red square. 4 is a
case of its own, since a window that crashed is no broken invocation, analysis or design, and 0
would say the session worked when its reader may never have seen it.

### 8.2 Analysis libraries

A library is a directory of analysed units (4.4). This section is about naming and reaching them.

```
enzan lib create [--name <n>] <dir>   creates a library and fixes its logical name
enzan lib list <dir>                  what a library holds, and what wrote it
enzan lib clean <dir>                 empties it, keeping the library

--out-lib <dir>                       the library analysis is written into
--in-lib <dir>                        a library to read; repeatable
--in-lib <name>=<dir>                 the same, overriding the name it carries
--context <file>                      analysed so references resolve; not stored
```

- **Libraries are optional and none is implicit** (4.4). VHDL's standard libraries are found without
  being named, and `--in-lib ieee=<dir>` replaces them.
- **The logical name lives in the library, not on the command line**, so that the two ends of a
  build cannot disagree. `--out-lib` on a directory that does not exist creates a library named
  after it; `<name>=<dir>` covers what a stored name cannot, such as two vendors each shipping a
  `work`.
- **A library carries an index**: its name, its format version and the time unit it was analysed in.
  A directory with no index is not a library. A run, or an analysis against or into a library, takes
  its time unit from the libraries that hold one, and a `--timescale` that disagrees, or two
  libraries in different units, is an error.
- **Libraries are searched in the order given, and a name two libraries both define is an error**,
  resolved by qualifying it, `uart.fifo`, so **EIR carries the logical library on every unit**.
  Copies of one unit, identical but for their source locations, are one unit; anything that differs
  is another, so a stale library is refused rather than chosen.
- **For SystemVerilog, `--in-lib` is an elaboration-time facility**: slang type-checks from syntax
  and EIR does not feed back into it, so a library records the sources it was analysed from and a
  design analysed against it reads them again. **VHDL is the exception**: a VHDL library holds NVC's
  units, so `use uart.uart_pkg.all` resolves against `--in-lib` with no source read again.
- **A specialisation is created by the design that needs it**, from the sources it read, and stored
  in its own library unless an input library holds it. **Its name MUST be a function of its
  parameter values**, and of the interfaces its ports connect to, never a counter, which would give
  two different modules one name. A parameterised top is named by its source name, and so is its
  instance.
- **A split is for names, not for speed.** Crossing a library boundary re-reads the other library's
  sources, so a design is built as one library unless names have to be kept apart.
- **`--context` names a source analysed for resolution whose units are not stored**, such as a
  package shared by libraries that do not depend on each other. It is still a dependency of the
  library that names it, and MUST appear in `--depfile` (8.3).

### 8.3 File lists

`.f` files are the de-facto industry standard. Enzan reads them and does not write them: generating
a list is what the user's own build already does. Supported: `-f` and `-F` including nested, `-v`,
`-y`, `+incdir+`, `+define+`, `+libext+`, `-sv`, `//` and `/* */` comments, double-quoted arguments,
and `$VAR` and `${VAR}` from the environment.

- **Relative paths follow the convention existing lists were written against**: inside a list read
  with `-f`, against the working directory; with `-F`, against the list's own directory. The two
  spellings MUST differ on the command line as well as when nested.
- **An option the reader does not recognise is an error**, reported with its position. File lists
  carry tool-specific switches that change what is compiled, and skipping one silently analyses a
  different design. `-sv` is accepted and dropped.
- **An unset variable is an error**, not an empty expansion that turns `$RTL_ROOT/cpu.sv` into
  `/cpu.sv`.
- **`-v` and `-y` supply only what nothing else defines**, read once the sources are analysed and
  only where a file holds a module wanted, whose other modules are no top levels: a vendor's five
  hundred models cost a design the three it uses. What they supply is analysed, stored and named
  in `--depfile` as a source is.
- **`--dry-run`** prints the expanded list, in order, and exits.
- **`--depfile <file>` writes a `make` dependency file naming every file the analysis read** — the
  sources a list expanded to, the headers they included, and the context files — so that a Makefile
  names the `.f` file as its only prerequisite and learns the rest after the first build.
  `--depfile-target` names the target, by default the `--out-lib` directory; with neither,
  `--depfile` is refused, since `make` refuses a rule for nothing.

### 8.4 The graphical front end

**`--gui` is an option of `enzan`, not a second program to learn.** A window exists because a run
was started, so the user says `enzan run --gui <top>`, and the socket and the attaching are arranged
out of sight.

- **It remains two processes** (2.2), and **the kernel binary MUST NOT link Qt**: a headless server
  and a CI runner would otherwise install a GUI toolkit to run a batch simulation. The front end is
  a separate executable in `libexec`, which `enzan` starts from its own installation and no other,
  and a package of its own (11.7).
- **The kernel parses the command line; the window only attaches.** `--gui` analyses and elaborates
  here and hands the design over, so the front end knows nothing about libraries or file lists. Its
  own options travel through `enzan`: `--wave-list` (10.8), and `--run`, which the window asks the
  kernel for once the kernel has said what unit the run counts. Its theme is the reader's and is
  found in the configuration directory (10.9).
- **With `--gui` the design is handed over, not run**, and the front end drives every run. `--vcd`
  and `--fst` are allowed, so that a session debugged in a window also writes a file.
- **The lifetime of the kernel follows how the session was started**:
  - **`enzan run --gui <top>`**: the window started the kernel, so closing the window ends the
    kernel too, and the exit status is the simulation's. A window that crashed or exited with an
    error is reported, and the status is 4 when the run gave no other. A front end that cannot be
    started is status 1.
  - **`enzan --gui --attach <socket>`**: the window did not start the kernel, so closing it only
    detaches. `--attach` requires `--gui`. `enzan run --listen <socket>` is the other half: it
    elaborates and waits without starting a window. Together they cover a kernel that outlives its
    window and a window on another machine.
- **A served kernel removes its socket when it is signalled**, and binding reclaims a path nothing
  is listening on, so a kernel killed with `SIGKILL` leaves nothing a user has to clear by hand.

### 8.5 One tool, one spelling

- **An option two executables share MUST be spelled the same in both**, short forms included.
- **An option that takes one value takes it once.** Given again with the same value, as a Makefile
  appending its own flags can give it, it is accepted; with another, it is refused rather than the
  last one silently winning.
- **A value the window and the command line both take is read by one function**, so `10ns` and `all`
  mean the same in each.
- **`--version` is `-V`, and `--help` is `-h`, everywhere.** `-v` names a library file in `.f` lists
  (8.3) and nothing else. What `--version` prints is defined once, for every executable.
- **A generated usage text is not automatically the right one.** Where Qt's generated help would
  differ from the rest of the tool's, the text is written out.

---

## 9. External API: VPI, VHPI and cocotb

**cocotb support is a higher priority than advanced SystemVerilog verification features**: it gives
Enzan a full Python verification environment without classes, randomisation, coverage or UVM. cocotb
drives a simulation through **VPI** (IEEE 1364/1800) for SystemVerilog and **VHPI** (IEEE 1076-2019)
for VHDL. The VPI subset is cocotb's: handle lookup and iteration, properties, values, time, and the
callbacks of 9.3. **cocotb's own regression suite is the acceptance test** (14.6).

### 9.1 Loading an application

`--vpi <library>[:<entry>]` loads an application, repeatably and in the order given, calling the
entry or else the library's `vlog_startup_routines`. Libraries are opened `RTLD_NOW | RTLD_GLOBAL`:
*now*, so that a routine the kernel lacks fails at load and is named; *global*, so that an
application can load further libraries against itself, as cocotb loads Python.

- **`enzan` exports the `vpi_*` routines and nothing else**, so that slang's and the standard
  library's symbols cannot collide with an application's own.
- **Every routine the standard declares is defined.** One Enzan does not support fails and says why
  through `vpi_chk_error`, rather than leaving the application unloadable.
- **Loading precedes analysis**, so that a library that cannot load is reported before a long
  analysis. Everything an application does to the design starts at `cbStartOfSimulation`.
- A command-line word beginning with `+` that is not an analysis option is a plusarg, passed on
  through `vpi_get_vlog_info`. `analyse` and `elaborate` refuse `--vpi`: nothing they do could be
  observed.

### 9.2 The object model

What VPI shows is the elaborated design, named the way the standard names it: `tb.u_dut.count`.

| Enzan | VPI type |
|---|---|
| instance | `vpiModule` |
| generate block | `vpiGenScope`, one per index (`gen[0]`); there is no `vpiGenScopeArray` |
| net | `vpiNet` |
| four-state variable | `vpiReg` |
| two-state variable | `vpiBitVar` |
| one bit of a vector | `vpiNetBit` or `vpiRegBit`, from `vpi_handle_by_index` or by iterating `vpiBit` |
| unpacked array | `vpiRegArray`, or `vpiNetArray` for nets, with `vpiSize` its element count |
| element of an array | as a vector, or an array again while dimensions remain, from `vpi_handle_by_index` by declared index or by iterating |
| parameter, localparam | `vpiParameter`, with `vpiLocalParam` telling them apart |

- **Objects a frontend introduced are not visible**, as in a waveform (7.1). One storage may have
  several names, a collapsed port's and its actual's, and a value-change callback on either fires
  when it changes.
- **A bit or an element is a signal of its own**: its value-change callback fires when its bits
  change and not when others of the signal do, or `RisingEdge(dut.bus[0])` would be wrong in exactly
  the testbenches that use it.
- **Handles to design objects are interned, and every handle is checked before it is used**, a bad
  one reported through `vpi_chk_error`: a defect in a plugin must not become a crash that the user
  reports as one in the simulator.

### 9.3 Callbacks and the region ordering

The PLI regions are hook points rather than queues (3.3). These are the points:

| Reason | Fires |
|---|---|
| `cbStartOfSimulation` | once, after elaboration and before anything at time zero runs |
| `cbNextSimTime` | when time advances, before anything at the new time runs |
| `cbAfterDelay`, `cbAtStartOfSimTime` | at the start of their time slot, after `cbNextSimTime` and before the Active region is drained |
| `cbValueChange` | at the change: once the new value is in the arena, the processes it wakes are scheduled and listeners have recorded it |
| `cbReadWriteSynch` | when Active, Inactive, NBA, Observed and the Reactive family are all empty; whatever it wakes then runs, and it may fire again in the same slot |
| `cbReadOnlySynch` | after the last read-write pass of a slot, before Postponed |
| `cbEndOfSimulation` | once, when the simulation ends, after `final` blocks |

- **A callback registered for the phase that is running runs at its next occurrence**, never in the
  pass that registered it, so the order cannot depend on where in a list a callback was appended.
- **A read-write pass counts as a delta cycle** (3.2), and **a pending timed callback keeps the
  simulation running**: a testbench written in Python schedules nothing of its own.
- **Value-change callbacks are delivered synchronously and may re-enter**, as in every other
  simulator. Callbacks of one reason fire in registration order (3.8).

### 9.4 Writing values

`vpi_put_value` is the kernel's write operations of 10.4, not a second implementation of them:

| Flag | Effect |
|---|---|
| `vpiNoDelay` | written now, waking whatever watches the signal: a deposit |
| `vpiInertialDelay`, `vpiTransportDelay`, `vpiPureTransportDelay` with no delay | written in the NBA region of the current slot, as a non-blocking assignment is |
| the same, with a delay | written at the start of the slot the delay reaches, before that slot's `cbAfterDelay` |
| `vpiForceFlag` | a force |
| `vpiReleaseFlag` | a release; `value_p` receives the value the signal keeps |

The three delay modes are not told apart yet: each schedules its write, and none cancels one already
pending. `vpiCancelEvent`, `vpiReturnEvent` and any write during `cbReadOnlySynch` are refused, as
the standard requires.

### 9.5 Ending

`vpi_control(vpiFinish)` is `$finish`. `vpi_control(vpiStop)` stops the run at the next time-slot
boundary, the only one a run can be resumed from with the same result: a served session then waits
for its client, and a batch run ends. **`cbEndOfSimulation` fires once, whichever way the simulation
ends**, a batch run's `--run` limit included, since cocotb writes its results from it. A design
cannot be restarted while an application is loaded: the application would hold the state of a
simulation that no longer exists.

### 9.6 cocotb

cocotb chooses a simulator by the name of a makefile in its own directory, so Enzan ships
`Makefile.enzan` and a script that adds it to a copy of cocotb's makefiles, for `make SIM=enzan`.
cocotb's own suite runs the same way, so what is tested is what a user runs.

**The VPI library is cocotb's build with no simulator-specific workaround in it**, so that Enzan is
held to what cocotb expects of a standard simulator. The VHPI library is the build for NVC, whose
one workaround, comparing names without their case, answers the one way Enzan's VHPI differs from
the standard (9.7). `COCOTB_HDL_TIMEPRECISION` becomes `--timescale`. In a design of both languages,
the toplevel's language's library is loaded with `--vpi` or `--vhpi` and the other through
`GPI_EXTRA`, as cocotb does elsewhere.

### 9.7 VHPI

VHPI is how cocotb reaches a VHDL toplevel. What it names that VPI does not is the VHDL type of
everything — base types, bounds, element types, record fields, enumeration literals — which cocotb
reads to decide what a handle is before it reads a value.

- **The types live in EIR**, in a table of each definition's, filled by the VHDL adapter and
  serialised with the definition. They change nothing about how a design behaves. The analysis that
  knows them runs in a child process (5.2), so EIR is the only place they can be kept.
- **VHPI is VPI's context with a second set of routines.** `--vhpi` loads an application into the
  same context, so that an application using both interfaces sees one simulation, and the two share
  one dispatch of callbacks. The header is the IEEE working group's, which NVC carries, so VHPI is
  built only with the VHDL frontend.
- **A declared object's case name** is read from its declaration, since NVC's analysis keeps no
  case; a region's name is in upper case, since EIR keeps no position of a block's label.

**Each interface shows its own language.** VPI finds no VHDL object by name, and returns a VHDL
instance it meets in an iteration as a named `vpiUnknown`; VHPI shows a SystemVerilog region as one
of class `vhpiVerilog` with nothing in it. cocotb requires this — its mixed-language test asserts
that VPI alone does not find the VHDL instance — and a VHDL region shown through VPI would lose its
types.

---

## 10. GUI

### 10.1 Technology

**Qt 6 Widgets**, not QML: QML is a poor fit for dense, data-heavy desktop tooling and for the
custom painting the waveform view needs. Qt 6 is used under LGPLv3 via dynamic linking.

### 10.2 Windows

The layout follows Modelsim, which is what users expect:

| Window | Contents |
|---|---|
| Structure | design hierarchy tree |
| Objects | signals and variables in the selected scope |
| Wave | the waveform view |
| Source | HDL source with current execution line and breakpoints |
| Transcript | command entry, `$display`/`report` output, diagnostics |
| Processes | active processes and their state |

Docking uses `QDockWidget`, and the layout is kept in the configuration directory (10.9). Wave and
Source share the central area as tabs, since each wants the whole width.

**A served kernel's own output is part of its transcript.** A VPI application prints to standard
output, never through Enzan, so while serving a front end the kernel captures its standard output,
and its standard error when that is the same file, and gives the window a copy. The terminal loses
nothing. A terminal is replaced by a pseudo-terminal rather than a pipe, so an application keeps its
line buffering. Enzan's own diagnostics stay out of the capture, since they reach the window as
diagnostics. A thread that touches no simulation state drains the capture, because one callback can
write more than a pseudo-terminal holds. A batch run captures nothing.

**A command is one action wherever it is offered**, with one name and one key in the menu bar, the
toolbar and a pane's own menu. A pane's keys act only while the focus is in that pane. F5 runs and
Shift+F5 stops; F1 lists every key, read from the actions themselves, and the mouse's gestures.

### 10.3 Waveform view

A custom `QWidget` with manual painting.

**The rendering cost MUST be a function of visible pixels, not of the number of value changes in the
visible time range.** Zoomed out, one pixel column may span millions of transitions, so a trace is
drawn from one summary per column, answered from the change index, rather than by iterating
transitions. Getting this wrong makes the viewer unusable on exactly the large designs Enzan
targets, and it is difficult to retrofit.

**One exception is a defect to be paid off, not a design**: a row showing a bit range of a signal is
summarised from the values, because the index counts changes of the whole signal and would draw a
bit that never moved as busy wherever its bus moved. A wrong picture is worse than a slow one (1.2).
The way back to the pixel bound is a per-bit change index.

**A vertical scroll MUST NOT repaint what it has not changed.** Summaries and drawn traces are kept
between repaints, and the canvas moves the pixels it has by a distance measured from the list
itself. Anything the geometry cannot account for repaints instead: traces beside the wrong names
would be a window that looks plausible and lies.

Required features: multiple named cursors, cursor locking, zoom and pan, grouping, extracting a bit
range into its own row, and a radix per row — hexadecimal for multi-bit values by default, a line
high or low for one bit, and ASCII, in which a byte with an X or Z bit is U+FFFD rather than a known
character. The ruler's labels are in the coarsest unit in which the step between ticks is at least a
tenth of a unit.

### 10.4 Writing signals from outside

Run controls, and `examine`, `force` and `deposit` on signals, are required alongside the waveform.
**These operations live on the kernel**, and the GUI and VPI both use them there: a second
implementation of writing a signal from outside would be a second set of semantics.

**A force outranks the design; a deposit does not.** Every write to a forced signal is discarded
until it is released, while a deposit is overwritten by whatever drives the signal next. A force
from outside is the design's own `force` of a constant, and its release the same release (3.7).
Forcing costs an ordinary run nothing: the table of overrides is allocated by the first one (13.4).

### 10.5 Theme

**Every colour MUST come from a theme**; no colour literals in widget code. Dark by default, and the
waveform view a high-contrast black background with light green traces.

- **Two themes are built in**, `dark` and `light`, each setting every colour itself, so that a
  forgotten key cannot leave a dark patch in a light window. A test holds both to every key, and
  both meet WCAG's contrast for text and for what is not text.
- **A theme file is the default theme with a Tcl script over it, applied whole or not at all.** A
  script that sets three colours changes three; one that fails changes nothing, and names its file
  and line. *View → Theme* chooses a theme, writes a copy of the one in use, and reloads it when
  asked. A new theme reaches everything already drawn.
- **The colours reach the widgets through a `QPalette`, not a style sheet.** A style sheet that
  gives a scrolling surface a background or a border makes Qt draw it through the style sheet's box
  model, which cannot scroll by moving the pixels it has: a signal list scrolled four times slower.
  **The style sheet MUST NOT style a scrolling surface**, in background or in border, and a test
  asserts it, because the breakage is invisible in the result.
- **Font sizes are the desktop's** unless a theme gives one, `font <area> <points>`. The signal list
  and its traces share one size, so that each name stays level with its trace.

### 10.6 Diagnostics as a feature

slang's diagnostics are dramatically better than most commercial tools', and preserving that quality
end to end is a selling point. **A single diagnostic engine serves the whole tool**: a diagnostic
carries a severity, a file, line and column, a source snippet with a caret, optional notes and a
stable code (`ENZ1234`), and slang's and NVC's diagnostics are translated into it, so output is
uniform whatever the source language.

- **Error codes are permanent.** A code appears in documentation, in suppression lists and in bug
  reports, so a retired code is deleted and its number never reused, and codes are never renumbered.
  They are allocated in ranges by the emitting component: 1–999 driver and source handling, 1000s
  SystemVerilog frontend, 2000s VHDL, 3000s elaboration, 4000s codegen, 5000s runtime, 6000s
  session, 7000s waveform database, 8000s PLI, 9000s internal errors.
- **A diagnostic whose default severity is `Fatal` MUST NOT be downgradable or suppressible**:
  turning one into a warning converts a clean stop into wrong results (1.2).
- **A degenerate construct in code a constant condition rules out is a warning.** A parameter can
  make a construct degenerate — a part-select of width 0, a negative replication — in a branch of a
  procedural `if` or an arm of a `?:` that its constant condition never selects. IEEE 1800 makes
  each an error even there; Enzan, as Icarus Verilog, warns when it is dead in every instance, and
  never translates it.

### 10.7 Waveform view interaction

**These gestures are normative.** A wave window is driven by hand for hours at a time; both choices
of most pairs feel natural, and a tool that swaps them between releases is worse than either, so
they are decided here.

- **The traces stop where the window's knowledge stops**: where the kernel has recorded to, or short
  of it while a signal's changes are still being fetched (2.2), with the axis beyond shaded. A trace
  drawn past that point would claim a value nobody knows yet.
- **The wheel** scrolls the signal list, wherever the pointer is; **Ctrl+wheel** zooms the time axis
  about the pointer; **Shift+wheel** pans it. One wheel, one meaning, across the whole window. Ctrl
  never pans, and dragging to pan is the middle button's.
- **Dragging with the right button** right to left zooms to the range covered, left to right to the
  whole run. A drag of a few pixels is no gesture, so a plain right-click costs nothing.
- **A left click** on the traces moves the active cursor, unless it is locked, selects the row, and
  snaps to a transition of it within a radius in **pixels**, so that it behaves the same at every
  zoom. Selecting a row in the list is the same act.
- **Cursors** show their times on their labels. The keyboard acts on the active cursor: Ctrl+L locks
  it, `,` and `.` change which one is active, Insert adds one. **Tab** and **Shift+Tab** step the
  active cursor to the next or previous transition of the selected row — a change in what the row
  *displays* — wherever the focus is.
- **Finding a value** moves the active cursor, unless it is locked, to the next or the previous
  time the selected row *starts* to show a value: shows it, and an instant earlier did not, so
  that finding again goes on to the next. **F3** and **Shift+F3** find wherever the focus is, as
  Tab steps, and so do Enter and Shift+Enter in the toolbar's field; F3 with nothing typed goes to
  the field. F3 is find-next's key in most programs, and Ctrl+F, the other candidate, is Zoom
  Fit's and stays so. What a search found, or why it found nothing, is said in the status bar, since it is
  a step of the cursor and not something the design said.
- **A value is read as the row shows one** — digits in its radix, a name of its enumeration, the
  list of its children or of its elements — so that whatever a row shows can be typed back, and a
  literal, `8'hff` or `'x`, means what it says in any row. A value too wide for the row is refused
  rather than cut down, since a value cut down is another value. There are no wildcards, save the
  one the row writes itself: a digit in capitals, `X` or `Z`, is partly unknown, some of its bits
  and not all, and is read as that, since a row showing `1X` shows it for any of several values.
  In a literal, case means nothing, as in SystemVerilog.
- **A search is a scan** (7.1): the row is taken apart into the bits of the signals it shows, as
  its value joins them, and the database searches those. So it runs a slice at a time on the event
  loop, never blocking the window, and moving the cursor or the selection abandons it.
  It finds nothing past where the traces stop, which is not known yet, and does not wrap.
- **The keys follow the wheel**: Ctrl zooms, Shift pans, and a key without a modifier is the signal
  list's own. **Typing the start of a name finds a row**, so a command MUST NOT take a key that
  types a letter or a digit in the Wave tab.

**The signal list.**

- Rows are reordered by dragging, renamed in place, and packed and unpacked by one toggling entry.
  Nothing re-sorts them, and **adding or pasting a row changes nothing else**: nothing already shown
  expands or collapses. A copy keeps its radix, highlight and unpacking.
- **Unpacking** shows a vector's elements as rows, outermost dimension first, each named by the
  index its source declares: `logic [0:7] v` unpacks into `v[0]` first, its most significant bit.
- **A row with children shows what its children show, joined, the first most significant.** Children
  of one bit make one number, so a bus with bits removed shows what is left of it and a group of
  one-bit signals reads as a vector; wider children make a list, `{001122}{112233}`, which nests. An
  unpacked vector is a group in every respect.
- **An enumeration shows the name of a whole value of its type**, `S_SEND` rather than `3'h2`, and
  the number for a slice, a group, or a value it does not name. Choosing a radix shows the number.
- **Parameters are rows**, constant, so drawn across the whole window and never clipped where the
  traces stop.
- **A scope lists what the reader wrote**, without the objects a frontend introduced (7.1).
- A row may be **highlighted** with one of five colours from the theme, covering its name and trace.

### 10.8 Signal list files

What the wave window shows can be saved and loaded again, from the window or with `--wave-list`
(8.4). The commands and their options are user documentation; these are the rules the format keeps.

- **The syntax is Tcl's; the language is not.** A list is split into commands by Tcl's own parser
  and never evaluated, and it has two commands, `add wave` and `group`. A list is a list, not a
  program: loading one someone sent you reads it rather than running their code, and the window can
  write one back out.
- **`add wave` on a scope is defined as** *Add to Wave*, and with `-r` as *Add to Wave Recursive*,
  so that the two cannot come to disagree.
- **Saving writes what the window shows**, signals by path, and nothing that varies between saves,
  so an unchanged window saves the same bytes. **Loading is pasting**: the whole file is read and
  every path looked up before any row is added.
- **Which rows are open is part of what the window shows.** A list marks every open group and
  signal `-expand`, and loading opens those and nothing else, so a list reloads as it was saved and
  a long one opens no more than its reader had open. ModelSim's `-expand` means the same.
- **A mistake in the file is an error; a path the design lacks is not.** A syntax error loads
  nothing, while a path the design lacks is a warning that skips its row, since a list outlives the
  version of the design it was saved from.
- Adding signals from the design echoes the matching `add wave` command into the transcript.
  ModelSim's `wave.do` files are not loaded: they are Tcl programs, and loading some of them would
  promise all of them.

### 10.9 The configuration directory

The window keeps what its reader has chosen in `$XDG_CONFIG_HOME/enzan`, `~/.config/enzan` when that
is unset, and `enzan --help` says where.

- **Nothing in it may change what a simulation computes or records.** A run is reproduced from its
  command line and its files (3.8), and a setting in a home directory that changed a result would be
  a cache found only after it had been trusted (4.4). The kernel reads nothing from it; only the
  front end, on the machine its window is on, reads or writes it.
- **The window makes it the first time it opens**, with a `config` that names no theme, which is
  the default, and never replaces a file that exists. No theme is written out under `themes/`: a
  copy of the built-in theme would be a second one in the menu, keeping the colours of the release
  that wrote it, and *New Theme* writes one when it is asked for. When the directory cannot be
  made, the window opens with the default theme and says why.
- **`config` has Tcl's syntax and is not Tcl.** Its one command names the theme,
  `theme themes/mine.tcl` or `theme -builtin light`. A command this build does not know is a warning
  and the rest applies, so that a directory shared with a newer Enzan keeps working in an older one.
  The window rewrites the `theme` command in place and keeps every other byte of the file.
- The dock layout is kept beside it, in Qt's own `layout.ini`.

---

## 11. Build system and dependencies

### 11.1 Toolchain

- **C++20** (required by slang). GCC 13+ or Clang 17+.
- **CMake 3.24+**, with `CMakePresets.json` providing reproducible configurations.
- Linux only (1.3).
- `ccache` supported when present.

### 11.2 Dependency policy

**System packages are the primary acquisition method**, with `FetchContent` as the fallback when a
dependency is absent or too old. Every dependency MUST be found through a `find_package` call that
fails with an error naming the package to install. Vendoring in `third_party/` is reserved for what
cannot be acquired otherwise, such as NVC, which is consumed at a pinned revision of its internals.

Adding a dependency requires an entry in 11.3, a `find_package` path with a good error message, a
`FetchContent` fallback where one is possible, installation in CI, and a license check against 1.4.

### 11.3 Current dependencies

| Dependency | Purpose | Acquisition |
|---|---|---|
| slang | SystemVerilog frontend | system, FetchContent fallback |
| NVC | VHDL frontend (M6) | vendored submodule, pinned |
| flex | generating NVC's lexer when the VHDL frontend is built; nothing linked | system |
| zstd | the analysed units NVC writes into a library, which it compresses (M6) | system |
| Tcl 8.6+ | GUI theme language (M2); signal list syntax (M2.2) | system |
| Qt 6 | GUI (M2) | system |
| GoogleTest | unit tests | system, FetchContent fallback |
| LLVM | JIT backend (M8, optional) | system, off by default |
| zlib | FST compression | system |
| cocotb | the cocotb golden test and regression suite (M4); never linked | `pip`, pinned in one requirements file |

### 11.4 Build configurations

`Debug`, `Release` and `RelWithDebInfo`, plus an `ASan+UBSan` configuration that CI runs; a `TSan`
configuration arrives with threading.

**Performance is measured in an optimised build, never in `Debug`.** What dominates a debug build's
profile is what the optimiser removes, so tuning against it tunes the wrong code. Profiling uses
`RelWithDebInfo` with frame pointers kept throughout, the dependencies included.

### 11.5 Installation

**`cmake --install` installs Enzan**, and every package of it is made from what that installs
(11.7). An installation is laid out so, under its prefix:

```
bin/enzan                      the one program a user runs
libexec/enzan/enzan-gui        the front end, started only by `enzan --gui` (8.4)
lib/enzan/vhdl/                VHDL's standard libraries (4.4)
include/enzan/                 vpi_user.h, sv_vpi_user.h and vhpi_user.h (9.1, 9.7)
share/enzan/cocotb/            Makefile.enzan and makefiles.sh (9.6)
```

**Nothing else is installed**: not the tool that builds the standard libraries, not the themes,
which are compiled into the front end, and not a dependency built in the tree, whose own install
rules would put a library's headers into every package. A test fails on any installed file this
table does not list.

**`enzan` finds the rest of Enzan from where it is itself**, by paths relative to its own directory
that are compiled in, so an installation works wherever it is moved and two versions side by side
each run their own parts. A directory outside the prefix is refused when CMake configures. **The
build tree is laid out as an installation is**, so the one lookup serves both and a build runs its
own parts rather than an installed copy's. The standard libraries are in `lib/enzan`, not the
distribution's library directory, which is for libraries other programs link.

**An installation is never written to.** A library a user names is where they name it (8.2) and
settings are in the configuration directory (10.9), so an installation its user cannot write to
works as one they can.

### 11.6 Releases

- **A release is a tag, `vX.Y.Z`, on a commit whose version is `X.Y.Z` with no suffix.** The commit
  after it moves to the next version with `-dev` again.
- **Its source tarball is Enzan's own, not GitHub's archive**, which leaves submodules out. A
  distribution's builder has no network, so the tarball carries every submodule at the commit the
  tag pins and the sources a build would otherwise download, each standing where its download would;
  a system package is still preferred where one is found. They are found by configuring with every
  download forced, so no revision is restated.
- **The tarball records the revision it was made from**, and `--version` reports that rather than
  asking git. **The same tag gives the same tarball**, byte for byte, so it can be checked by making
  it again.
- **A release is made by pushing its tag.** The release workflow (16) makes the tarball, refuses a
  tag that is not the tarball's version, builds and tests every distribution's packages, and
  attaches them with their checksums to a draft release. A maintainer reads it over and publishes
  it: nothing is published unattended, because a published release is never replaced.

### 11.7 Packages

- **Enzan is packaged for the distributions its toolchain floor reaches**: Ubuntu 24.04, Debian 13,
  Fedora, RHEL 9 and its rebuilds with gcc-toolset-13 and EPEL, and Arch Linux.
- **The recipes are each distribution's own**, not CPack's: the distribution's tools build with its
  flags, split the debug symbols and work out the dependencies, and only a recipe in its format can
  later be built into a repository. The packages are attached to a release; serving them from a
  repository is a later step.
- **Every recipe builds from the release tarball** (11.6), with no network and with downloads turned
  off, so a download is an error rather than an attempt. It runs the test suite and installs with
  `cmake --install`. The version is set in one place, `project()`.
- **The front end is a package of its own**, depending on the simulator's at the same version and
  suggested rather than recommended by it, so that a compute server installs Enzan without Qt (8.4).
- **What a distribution packages, Enzan is built against**; the tarball's sources stand in only
  where the distribution's package is missing or too old.
- **Each distribution's packages are built and tested in a container of it**: the build with no
  network, then the packages installed into a fresh container, the simulator's alone first, and run
  there.

---

## 12. Repository layout

```
Enzan-RTL-Simulator/
├── CMakeLists.txt            top-level build
├── CMakePresets.json         reproducible configurations
├── LICENSE                   GPLv3
├── README.md                 short orientation for users
├── CONTRIBUTING.md           building from source and checking a change; points here
├── SECURITY.md               how to report a vulnerability, and what is one
├── DESIGN_GUIDE.md           this document — the normative specification
├── .clang-format
├── .clang-tidy
├── .github/
│   ├── dependabot.yml        keeps the workflows' actions current
│   └── workflows/            GitHub Actions CI
├── cmake/                    find modules and helper functions
├── include/
│   └── enzan/                ONLY headers third parties compile against
│                             (vpi_user.h, sv_vpi_user.h; NVC's vhpi_user.h is
│                             installed beside them)
├── src/
│   ├── common/               diagnostics, source manager, small utilities
│   ├── eir/                  the internal representation and its type system
│   ├── frontend/
│   │   ├── sv/               slang AST -> EIR
│   │   └── vhdl/             NVC tree -> EIR
│   ├── elab/                 elaboration, instance tree, state layout
│   ├── codegen/              EIR-B -> bytecode
│   ├── runtime/              VM, scheduler, value engine, drivers
│   ├── wavedb/               waveform database, VCD and FST writers
│   ├── session/              analysis, elaboration and the kernel, per process
│   ├── ipc/                  kernel <-> GUI protocol
│   ├── pli/                  VPI and VHPI
│   └── gui/                  Qt 6 application code
├── tools/
│   ├── enzan/                headless kernel CLI
│   ├── enzan-gui/            Qt 6 front end, started by `enzan --gui`
│   ├── vhdl-libraries/       builds VHDL's standard libraries at build time (4.4)
│   └── cocotb/               cocotb's simulator makefile for Enzan (9.6)
├── tests/
│   ├── unit/                 GoogleTest, mirrors src/ layout
│   ├── golden/               self-checking HDL cases and expected output
│   ├── conformance/          harnesses for cocotb, sv-tests and the VHDL suites
│   └── designs/              scripts fetching external RTL — NEVER vendored
├── bench/                    performance benchmarks and result tracking
├── docs/                     documentation for users
│   └── development/          documentation for developers and maintainers
├── packaging/                each distribution's recipe, and the scripts that build them (11.7)
├── scripts/                  repository checks and release tooling, runnable locally and by CI
└── third_party/              vendored dependencies where unavoidable
```

Rules:

- Headers live **beside** their sources inside each `src/` component. `include/enzan/` is reserved
  for the headers that code outside this repository compiles against.
- `tests/unit/` mirrors the `src/` layout, so the test for a file is at the corresponding path.
- `tests/designs/` holds *scripts that fetch* external RTL at pinned revisions. External designs
  MUST NOT be committed: they are large, and they carry their own licenses.
- Each `src/` component has a `README.md` describing its responsibility and its allowed dependencies
  on other components. The build enforces its presence.
- `scripts/` holds checks that CI runs and that a contributor MUST be able to run locally with no
  arguments: a check that exists only inside a CI workflow is discovered as a red default branch
  instead of as a local failure.

---

## 13. Coding standards

### 13.1 Naming

Conventions follow slang's, since slang code and Enzan code sit side by side and mixing two styles
in one translation unit is worse than either alone.

| Element | Convention | Example |
|---|---|---|
| Namespace | lowercase | `enzan`, `enzan::runtime` |
| Types | `PascalCase` | `class EventScheduler` |
| Functions, methods | `camelCase` | `scheduleProcess()` |
| Variables, parameters | `camelCase` | `deltaCount` |
| Data members | `camelCase`, no prefix | `activeQueue` |
| Constants, enumerators | `PascalCase` | `Region::Postponed` |
| Macros | `SCREAMING_SNAKE`, `ENZAN_` prefix | `ENZAN_UNREACHABLE` |
| Source files | `PascalCase`, named for the principal type | `EventScheduler.cpp` |
| Directories | lowercase | `src/runtime/` |

### 13.2 Style

- `.clang-format` is authoritative. Formatting is not a matter of judgement and is enforced in CI.
- `.clang-tidy` is enforced in CI. Suppressions require a comment giving the reason.
- Prefer `enum class` over plain enums, `std::span` over pointer and length, `std::string_view` over
  `const char*`, and RAII over manual cleanup everywhere.
- Exceptions are for genuinely exceptional conditions only, never for control flow in the kernel's
  hot path.
- No raw `new` or `delete` outside arena and allocator implementations.

### 13.3 Documentation comments

Every public class and every non-trivial public method MUST carry a Doxygen `///` comment.

A good comment explains the **contract and the reasoning** — preconditions, invariants, ownership,
why this approach was chosen — and states its reason itself; section 0 says when it cites this
document as well. A comment restating the signature in prose is noise and should not be written:

```cpp
/// Schedules `process` into `region` for the current time slot.
///
/// Insertion is strictly FIFO: execution order within a region is
/// observable and must be reproducible across runs (see DESIGN_GUIDE.md
/// section 3.8). Callers must not rely on any other ordering.
void schedule(Process& process, Region region);
```

### 13.4 Performance discipline

- No allocation on the simulation hot path. Pre-allocate at elaboration.
- Prefer contiguous structures over pointer-chasing ones; the state arena (4.3) exists for this.
- Do not optimise without a measurement. `bench/` exists so that performance claims are checked
  rather than asserted.

---

## 14. Testing

Testing is continuous, not a phase. **Every feature ships with its tests in the same commit.**

### 14.1 Unit tests

GoogleTest, under `tests/unit/`, mirroring `src/`. Cover the feature and its error paths, not just
the happy path.

### 14.2 Golden tests

Small HDL cases under `tests/golden/`. **Self-checking testbenches are strongly preferred**: a
testbench that calls `$fatal` or `report failure` on a mismatch needs no golden file to drift out of
date. Where output comparison is genuinely needed, compare transcripts or waveforms.

### 14.3 Differential testing

Running the same design under Enzan and under a reference simulator and comparing the results is
**the single highest-value testing strategy available**, and MUST be part of the suite.

- Two correct simulators may differ in delta-level ordering, so **comparisons MUST be made at
  settled time boundaries**, not at every delta, or the suite produces constant false failures and
  is switched off.
- **The VHDL references are not interchangeable.** NVC supplies Enzan's VHDL frontend, so a shared
  analysis defect gives the same wrong answer on both sides and the comparison passes. **GHDL is
  therefore the primary VHDL reference and MUST be part of the suite**; NVC remains a meaningful
  reference for everything downstream of EIR. The same reasoning applies to any future frontend
  dependency.
- SystemVerilog is compared with Icarus Verilog. VHDL is compared with GHDL on what the design
  reported, in order, and on every signal GHDL records, as the value it settled on at each time
  step, GHDL's nine values mapped as 3.6 maps them.

### 14.4 Conformance suites

Public suites are run against Enzan and **the pass rate is tracked as a number over time**: sv-tests
for SystemVerilog; VESTs and GHDL's and NVC's own regression suites for VHDL, both of the latter for
the reason given in 14.3; NVC's tests of VHDL with Verilog; and cocotb's suite (14.6).

- **Each suite runs by its own rules**, through its own driver where it has one, so that a pass is
  the suite's definition of one. A case that tests what only another tool has is in neither count. A
  case that passes without exercising Enzan — one expecting a failure, or skipping itself — counts,
  being the suite's definition, and is reported as a second number.
- **The pass rate MUST NOT regress.** The ratchet is a list per suite rather than a number, so that
  one case starting to pass cannot hide another that stopped. A case that newly passes is added to
  the list in the same commit as the change that made it pass.
- **A suite's revision is named in one place**, and the harness fetches the suite at it.

### 14.5 Design regression

Real designs, fetched at pinned revisions and never committed: secworks `aes`, fpganinja `taxi`,
PULP `common_cells` and Yosys's models of AMD's cells for SystemVerilog; NEORV32 for VHDL and,
through its testbench rewritten in Verilog, for both languages; and later the processors picorv32,
VexRiscv, Ibex and CVA6, SLAC `surf`, and the VUnit and OSVVM suites, which need M9+.

- A testbench passes when it reports success and, where the reference simulator can run it, when its
  transcript is the reference's line for line: a design checking its own answers and two simulators
  agreeing on them. NEORV32 is held to GHDL's reports and to the CPU trace and UART logs its
  testbench writes.
- A testbench that cannot pass for a reason outside Enzan is named, with the reason, beside the
  design; any other failure fails the run.
- **A testbench written here can be wrong as easily as the simulator.** Before it is committed it is
  run under a second simulator, and it must pass there too.

### 14.6 cocotb regression

cocotb's own regression suite runs with `SIM=enzan`, exactly as cocotb's CI runs it, and its pass
list is ratcheted as 14.4 describes. **The cocotb version is named in one file**, which CI installs
from, and the suite is fetched to match what is installed rather than restating the version. Most of
cocotb's cases have a VHDL design beside the Verilog one, so the suite runs once for each toplevel
language, each with its own list. A golden cocotb test for each language pins the VPI and VHPI
behaviour cocotb relies on, whatever the pass rate is.

### 14.7 Sanitisers and fuzzing

ASan and UBSan run over the unit and golden suites in CI. TSan is added when threading arrives.
Frontend fuzzing with mutated HDL input is added once the frontends are stable.

---

## 15. Performance methodology

### 15.1 Target

**The target is provisional**: on the order of 10,000 simulated clock cycles per second on a design
of roughly 100k FPGA LUTs on a single core, and within about 5× of Verilator on the purely
synchronous subset. It will be replaced with a measured, committed figure (M8).

**The binding requirement is continuous measurement, not the number**: `bench/` records cycles per
second per commit. A performance goal without continuous measurement is decoration, and regressions
are invisible without a baseline.

### 15.2 Honest expectations

A tree-walking interpreter will not approach the target. The bytecode VM is a large step up, and the
JIT (6.3) is what is expected to reach it. This is why the execution model was decided before
implementation began: it constrains EIR, and discovering the constraint later means a rewrite.

### 15.3 Benchmarks

`bench/` holds a fixed set of designs with fixed stimulus, which CI runs as a step of its own. Once
a threshold is agreed, a regression beyond it should fail CI, in a nightly job if one is added (16).

---

## 16. Continuous integration

**GitHub Actions.** The workflow is the record of which job runs what; these are the rules it keeps.

- **Every job runs on every push and pull request**, the long ones included. The repository is
  public, so its runners cost nothing, and a check that ran on `main` alone would be found failing
  after a merge, leaving `main` red, rather than on the pull request before it.
- **Code compiled only with a component, or only without one, is compiled somewhere with warnings as
  errors.** One job builds every optional component, as a contributor's plain configure does, and is
  the only one to build the SystemVerilog frontend, since one is enough to prove it; another builds
  the GUI with neither language frontend.
- **The release workflow** runs on a tag or by hand (11.6). Run by hand on a branch, it tries the
  packaging and releases nothing.
- **There is no scheduled job.** A nightly run would be worth adding once there is somewhere to
  publish its results: conformance pass rates over time, the benchmark series (15.3) and the design
  regressions too long for every commit.

Dependency builds and `ccache` are cached. **The default branch MUST stay green**; a red default
branch is fixed or reverted before other work continues.

---

## 17. Working agreements

### 17.1 Commit messages

Every commit carries a category prefix. When a change is confined to a particular directory or file,
the path follows the category as further colon-separated segments:

```
<category>: [<dir1>: [<dir2>: [<file>: ]]]<Subject line>
```

Path segments are lowercase and mirror the source tree, even though source files are named in
PascalCase. The path prefix is used only when the change genuinely touches just that area; a change
spanning several components carries the category alone.

The category is one lowercase word for the kind of change or for the component it is in. These are
the usual ones, not a closed list: `build`, `ci`, `design`, `docs`, `eir`, `elab`, `frontend`,
`gui`, `session`, `ipc`, `wavedb`, `pli`, `runtime`, `codegen`, `test`, `bench`, `fix`, `perf`,
`refactor`, `license`.

Examples:

```
build: Add CMake presets for gcc and clang
runtime: scheduler: Drain the NBA region before Observed
fix: frontend: sv: Resolve parameters before port binding
test: golden: picorv32: Add differential VCD comparison
docs: Describe the mixed-language delta mapping
```

The subject line is imperative, capitalised, and 72 characters at most, its prefixes included. The
body explains **why**, not what — the diff already shows what. `scripts/check-commit-messages.sh`
checks every commit a branch adds for a category's shape and for the length, and CI runs it.

### 17.2 Commit granularity

**Commit incrementally.** Each logical change is its own commit with its own descriptive message.
The project is built largely by AI agents, which makes the commit log the primary record of what was
changed and why; a single large commit destroys that record and makes bisecting a semantic
regression effectively impossible.

### 17.3 Definition of done

A feature is not done until all of the following hold:

1. The implementation exists.
2. Unit tests cover the feature **and its error paths**.
3. If it affects simulation semantics, a golden or differential test pins the behaviour.
4. New public classes and non-trivial public methods carry `///` comments.
5. `clang-format` and `clang-tidy` are clean.
6. CI is green, including the sanitiser job.
7. If behaviour described in this guide changed, this guide was updated **in the same commit**.
8. Conformance pass rates did not regress.
9. Benchmark results did not regress beyond the agreed threshold.

### 17.4 Standing invariants

These are the rules most easily broken by accident. Violating one is a defect even if tests pass:

- **Determinism** (3.8). Never let hash container iteration order, pointer values or allocation
  addresses affect simulation results or output ordering.
- **Process purity** (4.2). A process reads and writes only its declared sets in the state arena.
- **Four-state shape** (3.6). Never write a value path that assumes a single bit plane.
- **No flattening in frontends** (5). Flattening is an elaboration-time optimisation only.
- **Thin frontend adapters** (5.2). slang- and NVC-specific code stays in its own directory and does
  not leak into EIR or downstream.
- **Opcodes carry their lowering** (6.1). A new opcode without a documented LLVM lowering sketch is
  incomplete.
- **No colour literals in GUI code** (10.5). Colours come from the theme.
- **No allocation on the hot path** (13.4).

### 17.5 Repository hygiene

Never commit build directories, generated files, external RTL designs (`tests/designs/` fetches
them), waveform dumps, or simulation libraries.

### 17.6 When to stop and ask

Stop and ask rather than guessing when a task depends on a decision this guide marks *(open)*; an
implementation would require violating a **MUST**; a new dependency seems necessary; or the language
standards appear to conflict with what this guide specifies. In the last case the standard is
probably right and this guide needs fixing, but that is a decision to surface, not to make silently.

---

## 18. Roadmap

Milestones are gated by their exit criteria. Do not begin the next milestone while the current one's
exit criteria are unmet.

### Done

| Milestone | Scope |
|---|---|
| M0 | Build system, test infrastructure and CI; the diagnostic engine |
| M1 | A SystemVerilog subset simulating, with VCD and FST output and benchmarks recorded |
| M2 | The interactive GUI: the process split, the live waveform database and the wave window |
| M2.1 | The wave window's mouse and keyboard model (10.7) |
| M2.2 | Signal list files (10.8) |
| M3 | Subcommands, exit statuses, file lists and analysis libraries (8) |
| M3.1 | One translation per parameterisation rather than per instance |
| M4 | VPI and cocotb (9) |
| M5 | Four-state completion and language breadth: nets, drivers and resolution, packages, interfaces, generate, structs, enums, arrays |
| M6 | VHDL via NVC, the VHDL simulation cycle and VHPI |
| M7 | Designs of both languages (3.5) |

### M7.1 — AMD Xilinx simulation libraries

Designs instantiating AMD Xilinx's simulation models, read from the user's own installation: the
unisims, unimacro, `glbl` and XPM from SystemVerilog; the same through `unisim` and `xpm` libraries
of components from VHDL; and 7-series functional-simulation netlists.

**Exit:** self-checking testbenches for the primitives designs use most — flip-flops with `glbl`'s
reset, LUTs, carry chains, shift registers, block RAM, DSP slices, clock buffers, MMCMs, DDR
registers, I/O buffers with pulls — match Icarus Verilog, their clock-to-output delays included;
XPM memories, FIFOs and synchronisers match Verilator; the same checks run from VHDL through the
component libraries agree with both; and NEORV32's 7-series functional netlists, in Verilog and in
VHDL, run its program as its RTL does.

### M7.2 — Altera simulation libraries

Designs instantiating Altera's models: `altera_primitives`, LPM, `altera_mf`, `sgate` and the
Cyclone 10 LP atoms from SystemVerilog, and from VHDL through Altera's component packages or as
VHDL libraries of their own; and Cyclone 10 LP `.vo` netlists.

**Exit:** testbenches for `altsyncram` with an initialisation file and for the LPM functions match
Icarus Verilog, and `scfifo` and `dcfifo` match Verilator; the Cyclone 10 LP atoms match Icarus
Verilog, and `cyclone10lp_pll` Verilator; Altera's VHDL models give GHDL's results; and NEORV32's
Cyclone 10 LP netlist runs its program as its RTL does.

### M8 — Performance

LLVM JIT backend behind a flag; profiling and hot-path work; activity-based scheduling refinements;
optional static partitioning for threading; the performance target replaced with a measured,
committed figure.

**Exit:** the committed performance target is met.

### M9+ — Verification features

SVA and PSL assertions; functional coverage; constrained random (which requires a constraint
solver); save and restore; multiple-clock-domain tooling; UVM.

**The VHDL verification libraries, VUnit and OSVVM**, and what they are built from: access types as
a general heap, with `deallocate`; protected types and shared variables of them; generic packages
and their instantiation; configurations; external names; and VUnit's simulator interface for Enzan,
which VUnit chooses by name as cocotb chooses a makefile. They also need values whose length only
the run knows, beyond the scratch of 4.1, and a VHDL top level's generics set from the command line,
as `-G` sets a SystemVerilog one's parameters. They are done when VUnit's own suite and OSVVM's
regression each run with a recorded pass rate.

### Known gaps

Left out of the milestones above, and refused by name where a design uses them rather than
mistranslated:

- SystemVerilog `string` variables, real and string ports, arrays and structs of reals, a real
  exponent, events as values, dynamic and associative arrays, queues, classes, unpacked unions,
  `fork`, `highz` strengths, resistive and bidirectional switches, the module paths 5.1 refuses, net
  aliases, and a procedural `assign` or `force` of part of an object.
- VHDL access types beyond what `std.textio` needs, protected types and configuration declarations.
- VHDL values whose length only the run knows, beyond a subprogram's strings (4.1): arrays of
  anything but `character`, a function's result, a concatenation outside a message or a `write`,
  a whole such string assigned, a function's own string sized by a parameter its call leaves to the
  run, and one held across a `wait`.
- A port of a real, an enumeration other than `std_ulogic`, `bit` and `boolean`, or a physical type
  across the language boundary; a VHDL port of an unconstrained type instantiated from
  SystemVerilog; storing a design of both languages in a library, and `-G` for one; a second top
  level of VHDL.
- In VPI, declared ranges (a vector reports `[width-1:0]`), integer atom types, part-select handles,
  struct members, user system tasks, and the difference between the delay modes (9.4).
- The Processes and Source windows (10.2), and a per-bit change index (10.3).

---

## 19. Risk register

| Risk | Impact | Mitigation |
|---|---|---|
| NVC's internal APIs drift on an upgrade | High — blocks VHDL | Pinned revision; a thin adapter in one directory; golden tests to catch drift; co-simulation over VHPI as the fallback (5.2) |
| A defect inherited from NVC's frontend is invisible to differential testing against NVC | High — silent wrong results | GHDL is the primary VHDL reference, and both suites are run (14.3, 14.4) |
| Mixed-language delta semantics differ from commercial tools | High — silent wrong results | Specified (3.5), documented for users, and tested by moving the boundary, since no free simulator runs both languages |
| Event-driven interpretation misses the performance target | Medium | Bytecode VM from the start; JIT-ready EIR; continuous benchmarking |
| Waveform memory exhaustion on large designs | Medium | Selective capture and a memory budget with disk spill (7.1) |
| Waveform rendering does not scale when zoomed out | Medium | Pixel-bounded rendering (10.3) |
| Scope creep into verification features | Medium | Explicit non-goals (1.3) and milestone gating |
| Parallelism delivers less speedup than hoped | Low | Expectations calibrated in 6.4; the single-threaded kernel is the plan, not a fallback |
| One unit per parameterisation (2.1) makes a functional netlist too many units | Medium | Measured: a 7-series NEORV32 netlist is 2,306 cell parameterisations for 41,355 instances; value-only parameters would reopen 2.1 |
| Vendors' models change between releases, and CI cannot run them | Medium | A release pinned and recorded; golden tests of each construct, and Yosys's models of AMD's cells, in CI; the vendor harnesses run before merging |

---

## 20. Glossary

**Analysis**: compiling one design unit to EIR, independent of any top level; a parameterised unit
once per set of values the design uses (2.1). Cacheable and reusable.

**Delta cycle**: a zero-duration iteration ordering evaluations at the same simulated time.

**EIR**: the Enzan Internal Representation. EIR-S is structural, EIR-B behavioural.

**Elaboration**: building the instance tree and state layout for a specific top level. Not cacheable
across designs.

**Region**: one of the ordered phases within a SystemVerilog time slot.

**State arena**: the single contiguous allocation holding all mutable simulation state.

**Time precision**: the global integer unit in which simulated time is counted.

**VPI / VHPI / DPI**: VPI and VHPI are the introspection and callback APIs tools such as cocotb
drive a simulation through. DPI is the separate mechanism for calling C from SystemVerilog.
