# Enzan RTL Simulator — Design and Development Guide

**Status:** Living specification. Revised as the project evolves.
**Audience:** AI coding agents and maintainers working in this repository.
**Authority:** This document is the single normative specification for Enzan. Documentation for
users lives separately and is derived from it, never the reverse.

---

## 0. How to use this document

Agents working in this repository MUST read sections 0, 12, 13 and 17 before making any change, and
MUST read the section covering the component they are modifying.

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

**Citing this document.** A comment states its own reason. It also cites this document, as
`DESIGN_GUIDE.md section N.M`, only where the code enforces a rule made here and a change made
locally could break it unknowingly. Documentation, and anything a user reads, never cites it.
Section numbers are stable: a change that renumbers a section updates every citation of it in the
same commit.

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

- Gate-level simulation, SDF back-annotation and timing checks.
- Power-aware (UPF/CPF) and mixed-signal (Verilog-AMS, VHDL-AMS) simulation.
- IP encryption (IEEE 1735).
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
recorded, in the user documentation, with its license, upstream URL and pinned revision.

---

## 2. Architecture

### 2.1 The three phases

**Analysis** turns one design unit at a time — a module, package, interface, entity or architecture
— into Enzan's internal representation (EIR, section 4), and may store the result in a library. Its
output is independent of any particular top level, and so is reusable across runs and designs.

**A parameterised unit is analysed once per set of values the design uses.** slang is an elaborating
frontend: it type-checks a body in the context of the values it is given, because a type can depend
on a parameter, and offers no body generic over them. The VHDL adapter folds generics the same way
(5.2). So a unit becomes one unit per set of values, named for them (8.2): the same values make the
same unit wherever it is produced, but a library is re-runnable rather than reusable at values it
has never seen. Rebuilding a generic unit out of slang's elaborations was rejected: it would be
checked against slang only at the values slang saw, and trusted at the others.

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
  on 8-bit and on 32-bit operands is two bodies.
- **A call's frame lives on one stack the kernel owns.** A subprogram never suspends — one that
  waits, or takes a signal parameter, is inlined into its caller instead — so every frame is popped
  before its process suspends, and one stack serves the single-threaded kernel. It is sized at
  elaboration for the deepest chain of calls the design can make; a recursive design is given a
  configurable depth limit, past which the run stops with a diagnostic, as the delta limit does.
- **A caller's read and write sets include its callees'**, bound to the instances they touch.
- **A value whose length is known only when it is computed** — `integer'image`, `to_string` — lives
  in scratch the kernel resets whenever a process suspends.

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
holds its values' bits in Enzan's own format, as each simulator has its own.

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
  relative — `core/state`, `../u_peer/ready` — because one definition serves every instance of a
  parameterisation.
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
  language, as commercial tools extend it. The component's generics are the module's parameters.
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

**Qt 6 Widgets**, not QML. QML is a poor fit for dense, data-heavy desktop
tooling and for the custom high-performance painting the waveform view needs.
Qt 6 is used under LGPLv3 via dynamic linking.

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

Docking uses `QDockWidget`. Layout is persisted, in the configuration directory (10.9), and is
definable from a Tcl configuration script.

**A served kernel's own output is part of its transcript.** A VPI application prints to the
process's standard output, never through Enzan — cocotb's whole log is written that way — so a
transcript holding only `$display` and diagnostics would show a cocotb run as silence. While it
serves a front end, the kernel therefore captures its standard output, and its standard error
when that is the same file, and gives the window what the terminal gets. Five rules make it
behave:

- **The terminal loses nothing.** Everything captured is written on to where it was going, as
  written; the window gets a copy.
- **An application sees the kind of output it had.** A terminal is replaced by a
  pseudo-terminal, not a pipe: a program writing to a pipe buffers in large blocks, Python does,
  and its lines would reach the window long after the events they describe. Colour codes stay on
  the terminal and are stripped from the window's copy.
- **Standard error is merged only when it already was.** Errors the user had sent somewhere
  other than standard output stay there, and Enzan's own diagnostics stay out of the capture
  altogether, because they reach the window as diagnostics.
- **One route, in one order.** The kernel's own transcript is written to the captured standard
  output and flushed as it is written, so `$display` lines and an application's lines reach the
  window in the order they reached the terminal.
- **A thread drains the capture.** One callback can write more than a pseudo-terminal holds, and
  a write that cannot complete blocks its thread; if that thread were also the one draining the
  capture, it would wait for itself. The thread copies bytes and touches no simulation state, so
  the kernel is single-threaded in every sense section 6.4 means.

A batch run captures nothing: with no window, the terminal is the transcript.

**Wave and Source share the central area as tabs**, rather than one of them
being a dock beside the other. Each wants the whole width: a waveform squeezed
next to an eighty-column listing leaves too little of either to read, and a
reader is looking at one or the other. The Source tab is raised automatically
when the design stops at a line, because the reader asked to stop somewhere and
the point is to see it.

**A command is one action wherever it is offered.** The menu bar, the toolbar and a pane's
own menu share it, so a command has one name and one key in all of them; the wave list's menu
is the window's Edit menu, entry for entry. A pane's keys act while the focus is in that pane
and nowhere else, so that Delete in the Structure window cannot remove a wave row. The run's
keys are the window's, a debugger's: F5 runs and Shift+F5 stops. *Help → Keyboard Shortcuts*,
F1, lists every key, read from the actions each time it opens so that it cannot disagree with
them, and the mouse's gestures, which are not actions, from a table beside their handlers.

### 10.3 Waveform view

A custom `QWidget` with manual painting.

**The rendering cost MUST be a function of visible pixels, not of the number of
value changes in the visible time range.** When zoomed out, a single pixel
column may span millions of transitions; the renderer MUST use per-signal
downsampling with min/max bucketing rather than iterating transitions. Getting
this wrong makes the viewer unusable on exactly the large designs Enzan targets,
and it is difficult to retrofit.

**One exception, and it is a defect to be paid off rather than a design.** A row
showing a *bit range* of a signal — an unpacked bit, an extracted slice — cannot
be summarised from the change index, because that index counts changes of the
signal and records nothing about which bits each of them moved. Summarising the
whole signal for such a row is wrong in the way that matters: every column in
which any bit of a bus moved reported a transition, so a bit that never changed
was drawn as a solid busy bar for the width of the window and became the flat
line it always was only once the reader zoomed in far enough for each column to
hold a single value. The same went for the unknown flag — one X bit made every
bit of the bus look unknown.

So a sliced row is summarised from the values, at a cost proportional to the
changes in the range, and section 1.2 is what decides that: a wrong picture is
worse than a slow one. It is bounded twice over — the renderer keeps a summary
until the time range changes, so scrolling and repainting pay nothing, and only a
row the reader has deliberately sliced or unpacked takes the path at all.

The way back to the pixel bound is a per-bit change index, which is not built
because it costs memory for every bit of every recorded signal whether or not
anyone ever looks at one. Anyone finding this in a profile should build it rather
than reinstate the whole-signal summary.

That bounds a repaint. What bounds *scrolling* is a second rule: **a vertical
scroll MUST NOT repaint what it has not changed.** Scrolling moves where each
trace sits and changes not one of them, so the window moves the pixels it
already has and paints only the strip that has come into view — which is what
the signal list beside it already did, and the reason the two used to feel so
different. Three things make it hold, and each is worth stating because each was
a separate cost in the same place:

- A row's *summary* — one entry per pixel column — depends on the row, the time
  range and the width, and none of those change when the list scrolls, so it is
  kept between repaints.
- A row's *drawn trace* depends on the same three, so it too is kept, as a
  picture, and blitted rather than drawn again. Drawing a bus means a segment, a
  value measured against the font and a string laid out for every run of columns
  across the window; that is the expensive half and it was being repeated to put
  the same traces one line up. Nor does a trace depend on the cursor, so moving
  the cursor draws none of them again.
- The distance to move by is **measured from the list**, by asking where a row
  the canvas has drawn sits now, rather than worked out from the scroll bar.
  Deriving it would mean assuming whether the bar counts items or pixels and
  whether every row is the same height, and a blit that moved by the wrong
  amount leaves traces sitting against the wrong names — a window that looks
  entirely plausible and is lying. Anything the geometry cannot account for
  repaints instead: slow is recoverable, wrong is not.

One-pixel rules — the grid, the ruler ticks, the line between rows — are
*filled*, not stroked. The pixels are the same either way and the fill path
writes them directly, where the stroker sets up a pen and a clip per call; on a
wide window with a line per row it was a third of what the ruler cost, paid on
every repaint.

**The ruler's unit follows the zoom.** Its labels are written in the coarsest of
s, ms, us, ns, ps and fs in which the step between ticks is at least a tenth of a
unit, so a window zoomed out to microseconds reads `1.5us` rather than
`1500000ps`. The unit is chosen once for the whole ruler, from the step, so every
label on it is in the same unit; and a tenth is the finest it goes, so a label has
at most one decimal place. It is computed on integers, never rounded. A cursor's
label and the status bar keep the time precision, because there the reader wants
the instant itself rather than a round number near it.

Required features: multiple named cursors, cursor locking, zoom and pan, signal
grouping, extracting a bit range of a signal into a separate displayed signal,
and per-signal value radix selection.

**A radix may be ASCII**, for a vector holding a string. It shows the bytes as
characters, the most significant first, and drops leading NUL bytes as `%s` does.
A byte that is no printable character is an escape, `\n` or `\x07`. A byte with an
X or Z bit is U+FFFD rather than the character `%s` would make of it by reading the
unknown bits as 0, since a waveform must not show an unknown value as a known one.

A bit range is *extracted* rather than dialled in. The window reaches one by
unpacking the signal, removing the bits that are not wanted and, when the range
should read as one value, grouping what is left — each step being a thing the
reader can already do to any row, and the result being a row that shows the
range on its own line (10.7). A dialog asking for `high:low` was the first way
this worked and has been withdrawn: it was a second way to reach the same place,
it could only ever produce a contiguous range, and it put a number between the
reader and a thing they can point at. The model keeps the operation, because the
a front end asks for a range as text and will call it.

Defaults, as specified: multi-bit values display in **hexadecimal**; single-bit
values display as a line high or low.

What the mouse and the keyboard do with all of this is section 10.7.

### 10.4 Writing signals from outside

The waveform is the headline feature but not the whole product. Also required:
run/continue controls, and `examine`, `force` and `deposit` on signals from the
GUI.

**These operations live on the kernel.** The GUI does not reach them through any
intermediate layer and VPI will not either, and a second implementation of "write
a signal from outside" would be a second set of semantics to keep in step.

**A force outranks the design; a deposit does not.** Every subsequent write to a
forced signal is discarded until it is released, which is what makes a force
useful for pinning a reset or a mode bit. A deposit is written once and whatever
drives the signal next overwrites it.

**Forcing costs an ordinary run nothing.** The table of forced signals is
allocated when the first force in a run needs it, so the check on the signal
write path is a test of one size against zero until then — which is what keeps
13.4 intact.

### 10.5 Theme

Dark by default. **Every colour MUST come from a theme file**; no colour literals in widget
code.

Waveform view defaults: high-contrast black background, light green traces.

**Two themes are built in**: `dark`, the default, and `light`, for a reader who wants one. Each is a
file of its own compiled into the front end, and each sets every colour itself: the light theme is
not the dark one with some colours changed, because a colour it forgot would then be the dark
theme's, a dark patch in a light window, and a test holds every built-in theme to setting every
key. The light theme keeps the dark one's distinctions — its own colours for unknown and high
impedance, a shade between the background and the traces for time not yet simulated — turned to
read on white. Both are held to one bar of readability: text at the contrast WCAG asks of body
text, traces and cursors at the contrast it asks of what is not text.

**A theme is the default one with a script over it, taken whole or not at all.** A script that
sets three colours changes three colours, and the rest are the default theme's rather than
undefined. A script that fails — a key this build does not know, a colour that is not one —
changes nothing, the lines before the mistake included, and the error names the file and the line
it stopped at. A window coloured partly by a theme that did not load takes its colours from two
places with nothing to say which came from where, and a signal list follows the same rule for the
same reason (10.8). Which theme the window uses is named in the configuration directory (10.9).

**A theme is chosen, written and reloaded from the window.** *View → Theme* offers the built-in
themes, the default first, and every file in the configuration directory's `themes/`, and
choosing one colours the window and names it in `config` for the next. *New Theme…* writes a copy
of the theme in use to a file of its own there — the built-in theme's text, or the file's —
refusing a name already taken: what the reader is looking at is what they go on to edit. *Reload
Theme* reads the theme in use again for a reader who has just edited it — asked for, rather than
done whenever the file changes, because an editor saves in steps and a theme loaded halfway
through a save is one that failed. The built-in themes are always among them: they are the ones a
newer Enzan has improved, which a copy written out by an older one never sees, and the ones that
work when the directory does not.

**A new theme reaches everything already drawn.** Nothing in the window keeps a colour or a size
past the moment it was painted without being told when the theme changes: the canvas's drawn
traces (10.3), the transcript's lines, each of which remembers which of the theme's colours it is
in rather than the colour, and the highlight swatches. A window that showed half its traces in the
old theme until they happened to be drawn again would look broken, and would be. Sizes are worked
out from the desktop's fonts every time, so a theme that sets none gives the desktop's back after
one that set them. Nothing else changes — not the rows, the cursors, the zoom or the run.

**The colours reach the widgets through a `QPalette`, not a style sheet.** This
is a performance rule wearing a presentation rule's clothes. A Qt style sheet
rule that gives a widget a background — or a border — makes Qt draw that widget
through the style sheet's box model, and a widget drawn that way can no longer
scroll by moving the pixels it already has: it repaints them. On a signal list
of two hundred rows in a tall window that was the difference between 1.7 ms and
6.2 ms for every line scrolled, and a single `QAbstractScrollArea { border }`
rule was nearly all of it. A palette is read by the ordinary style and costs
nothing.

So: **the style sheet MUST NOT style a scrolling surface**, meaning `QWidget`,
`QAbstractScrollArea`, or any item view, in background or in border. What a
palette can say goes in the palette; the style sheet keeps only what has no
palette entry — padding, the dock title, the parts of the scroll bar. A test
asserts this, because the rule is invisible in the result: a style sheet that
breaks it looks exactly right and scrolls four times slower.

**Font sizes are the desktop's unless the theme gives one.** A theme sets them beside its
colours, `font <area> <points>`, for four areas: `window`, everything not named after it;
`wave`, the signal list and the traces beside it, one size for both so that each name stays
level with its trace, which sets how tall a row is; `transcript`; and `source`, the Source
window's. An area left unset takes the window's size, and the window the desktop's. The shipped
theme sets none, because how large text should be is the reader's screen's and eyes' to say,
and lists the four commented out.

### 10.6 Diagnostics as a feature

slang's diagnostics are dramatically better than those of most commercial
tools, and preserving that quality end-to-end is a genuine selling point rather
than polish.

A single diagnostic engine serves the whole tool. Diagnostics carry a severity,
a source location with file, line and column, a source snippet with a caret,
optional attached notes, and a stable error code (`ENZ1234`) that can be
referenced in documentation and suppressed selectively. Frontend diagnostics
from slang and NVC are translated into this engine so that output is uniform
regardless of source language.

**One family of slang's errors is a warning in code a constant condition rules out.**
A parameter can make a construct degenerate: a part-select of width 0, a replication
count below 0, or zero outside a concatenation, a zero-width cast, a zero replication
in an assignment pattern. IEEE 1800 makes each an error even in a branch that can never
run, and slang and Verilator report it; but parameterised RTL often guards one with an
`if` or a `?:` on a parameter, and Icarus Verilog accepts it there. Enzan accepts it as
ENZ1008 when it lies in the branch of a procedural `if` that a constant condition does
not take, or the arm of a `?:` that a known condition does not select, in every
instance that elaborated it. The same construct live in any instance stays an error,
and a `case` item passed over by a constant selector is not dead. Such code is never
translated, so nothing of it reaches the simulation.

**Error codes are permanent.** A code appears in user documentation, in
suppression lists in users' scripts, and in bug reports, so reusing a number for
a different meaning silently changes what a user's suppression suppresses. A code
is retired by deleting it and never reusing the number; codes are not renumbered.
Numbers are allocated in ranges by the component that emits the diagnostic — 1–999
for the driver and source handling, 1000–1999 for the SystemVerilog frontend,
2000–2999 for VHDL, 3000–3999 for elaboration, 4000–4999 for codegen, 5000–5999
for the runtime, 6000–6999 for the session, 7000–7999 for the waveform
database, 8000–8999 for the PLI, and 9000–9999 for internal errors. The catalogue
itself is `src/common/DiagnosticCodes.def`.

A diagnostic whose default severity is `Fatal` MUST NOT be downgradable or
suppressible. Fatal means continuing is unsafe, and letting a user turn one into
a warning converts a clean stop into wrong results, which priority 1 of section
1.2 forbids.

### 10.7 Waveform view interaction

The gestures below are normative. A wave window is read for hours at a time and
driven almost entirely by hand, so what the mouse and the keyboard do is part of
the product rather than a detail of the widget. They are written down so that
they are decided once, and so that the next person to touch the view does not
quietly swap two of them for the pair that feel more natural to them — both pairs
feel natural, and a tool that changes which is which between releases is worse
than either choice. They arrive with M2.1 (section 18).

**Where the traces stop.** Every trace stops at the same time, and the time axis
beyond it is shaded: where the kernel has recorded to, or short of that while
the window is still fetching a signal's changes (2.2). Drawing a signal's last
known value on past that point would read as "this signal is flat here" when
the truth is "nothing is known here yet", and a signal still catching up would
be drawn holding a value it may already have left. The time axis reaches as far
as the traces have, so that following the tail follows them, and the status bar
says how far the kernel has got, which a window behind a fast clock can be well
short of. A signal added part-way through a run stops the traces where its past
is still to come, but does not pull the axis back with them, which would move
the view under the reader.

**The wheel.**

- **Wheel** scrolls the signal list vertically, wherever the pointer is.
- **Ctrl+wheel** zooms the time axis about the pointer.
- **Shift+wheel** pans the time axis.

The names and the traces are two widgets and one window. A wheel that scrolled
over the names and zoomed over the traces would make an internal boundary the
reader has no reason to think about decide what their wheel does; one wheel, one
meaning, across the whole window.

**The right button.** Dragging with the right button is two gestures told apart
by their direction, with no mode to select first:

- **Right to left** — zoom to the time range the drag covered.
- **Left to right** — zoom to fit the whole run.

While a drag is in progress the range it covers is shown, so that the reader sees
what they are about to get. A drag of only a few pixels is not a gesture at all:
a plain right-click must not zoom anywhere, or every mis-click costs the reader
their place.

**The left button.** A click on the trace area does three things at once:

- moves the active cursor, unless it is locked;
- selects the signal whose row was clicked;
- snaps the cursor to a transition of that signal when one is close to the click.

Selecting a row **in the list** means the same thing, with the cursor's own time
standing in for where the click landed. Picking a signal by name and picking it
by trace are the same act, and a reader who did the first and found that Tab did
nothing would reasonably conclude the key was broken.

The snap radius is measured in **pixels, not in time units**, so the gesture
behaves the same at every zoom: close enough to click is close enough to snap.
With no transition in range the cursor goes exactly where it was put, because a
cursor that jumped to a distant edge would be worse than one that did not snap at
all.

Which signal is selected is not decoration: it is what Tab steps along below.

**Cursors.**

- Every cursor shows its time beside its name.
- Clicking a cursor's label makes it the active cursor.
- Double-clicking a label renames it.
- Right-clicking a label opens that cursor's own menu: rename, lock or unlock,
  remove. That is the mouse's way, and it names the cursor by pointing at it. A key
  cannot point, so the keyboard acts on the active cursor, which is drawn heavier
  than the rest: Ctrl+L locks or unlocks it, `,` and `.` make the cursor to its left
  or right active, and Ctrl+Home and Ctrl+End move it, unless it is locked, to time
  zero and to where the traces stop.
- A cursor is added from the toolbar, beside the zoom controls, because adding
  one is part of measuring rather than part of configuring the window; or with
  Insert. The new cursor is the active one, at the active one's time.
- **Tab** moves the active cursor to the next transition of the selected row,
  **Shift+Tab** to the previous one. It does so wherever the focus is — the
  signal list must not spend the key on its own focus navigation.
- A *transition of the row* means a time at which what the row displays differs
  from what it displayed an instant earlier. Not every change of the underlying
  signal: a row showing one bit of a counter must not stop at every count, and a
  group's transitions are its children's put together. There is no index of when
  a single bit turns over, so the search walks the signal's transitions and gives
  up after a bounded number of them; past that the cursor lands on a real
  transition of something in the row, which is worse than the exact answer and
  far better than a frozen window.

Together these are the measuring workflow, and the reason cursor locking (10.3)
exists: pin a cursor on the edge you are measuring from, select the signal, and
step the active cursor along it. Reading both times off the labels is what makes
that worth doing, which is why the times are on the labels and not in a status
bar where only one of them would fit.

A readout of the interval between two cursors is the obvious companion and is
deliberately **not** part of this. It needs a decision about which pair of
cursors it measures, and making that decision before anyone has used the stepping
it would sit beside is how a tool acquires a number nobody reads.

**The signal list.**

- **Reordering.** Selected rows move by being dragged, including into and out of
  groups. The order is the reader's, and nothing may re-sort it behind them.
- **Renaming.** A row is renamed in place, by editing its name. The row's own
  menu offers it as well, and opens the same editor rather than a dialog of its
  own: a name is the contents of a cell, and offering it twice in two shapes
  would be two things to keep in step for nothing.
- **Packing and unpacking from the menu.** One entry, not two. A row is either
  packed or unpacked, so a menu offering both would always have one entry that
  did nothing; the entry toggles every selected row that has contents to show.
  It is the same act as the disclosure arrow, which is what creates a vector's
  bits the first time, so the two can never disagree about what a row is showing.
- **Copy and paste.** Ctrl+C copies the selected rows, with everything below
  them, and Ctrl+V pastes them after whatever is current, at its level, a group
  included; dragging is what puts rows inside a group. A copy keeps its radix, its highlight and how far it was unpacked,
  because a row that had been set up and then pasted back as a bare default
  would be a worse answer than copying nothing. The clipboard also carries the
  hierarchical paths as plain text, so that a copy is worth something outside
  this window.
- **Adding or pasting a row changes nothing else.** A new row arrives packed,
  and whatever was unpacked or collapsed stays as it was. Worth stating twice
  over, because both obvious implementations get it wrong: expanding the tree so
  the new row is visible unpacks every bus in the window, and rebuilding the
  model to insert a pasted subtree makes the view forget every expansion in it.
  Either way the reader loses the arrangement they had built, every time they
  add a signal.
- **Anything that changes which rows are on screen redraws the traces.**
  Expanding, collapsing, pasting, deleting. The names and the traces are two
  widgets, and only one of them is told by Qt.
- **Unpacking.** A multi-bit signal carries an expander that shows its bits as
  rows of their own, which then behave like any other row — radix, highlight,
  removal and all. Unpacking is a property of the display and creates no new
  subscription: the window fetches the signal once however many of its bits are
  on screen, exactly as it already does for an extracted bit range (10.3).

  **An array unpacks by its dimensions**, outermost first, one level at a time:
  `logic [63:0][17:0] count` into sixty-four rows of eighteen bits, `count[63]`
  down to `count[0]`, each of which unpacks into its bits in turn. Packed and
  unpacked dimensions behave alike in this, and so do a VHDL array's, where an
  array of arrays and an array of two dimensions are indexed down to the same
  bits. The dimensions are the ones the source declares (4.3), and the kernel lists
  each name with its own, since a port collapsed onto its actual may be declared
  flat where the actual is not. A row is named by the index its source gives it, not
  by where its bits sit in the flat vector: `logic [0:7] v` unpacks into `v[0]`
  first, its most significant bit, because that is what `v[0]` means in the code. An
  element with no dimensions of its own — an `int`, a packed struct, a record —
  unpacks into its bits.
- **A row with children shows what its children show, joined together.** One
  rule, and it is doing three jobs. A bus that has been unpacked and had some of
  its bits deleted shows what is left of it, in the order the rows are in. A
  group whose children are one-bit signals behaves as a vector, on its line and
  in its trace, which is how a bus assembled by hand out of separate signals
  becomes readable. And because the rule is recursive, a group of equal-width
  groups is a dimension above that — which is what makes multi-dimensional
  arrays fall out of nesting rather than needing a mechanism of their own.

  The first child is the most significant, which is the order the list shows
  them in and the order unpacking creates them in. A bus whose bits have *all*
  been deleted shows nothing, rather than quietly reverting to the value the
  reader just took apart.

  **How it reads depends on what is in it.** Children that are single bits make
  one number, which is what lets an unpacked bus read as the bus. Children wider
  than a bit make a *list* of them — `{001122}{112233}` — and because that
  recurses, a group of such groups reads `{{11}{22}}{{22}{33}}`. Joining a vector
  of vectors into one number files off exactly the boundaries that are the reason
  it has that shape. The list form also means a group of unlike signals is
  readable rather than being either nonsense or nothing, so no group is refused a
  value for the widths it happens to contain.

  An array reads the same way before it is unpacked as after, as the list of its
  elements, on its line, in its trace and in the Objects window: `count` reads
  `{00a}{00a}…` whether its rows are open or not, and a vector, whose elements are
  bits, stays one number.

  A multi-bit signal is therefore a group whose children happen to be its own
  bits, and the only thing that distinguishes it is that those children are made
  on demand rather than up front. Making them eagerly would cost a thousand rows
  for a thousand-bit vector nobody had asked to look inside.

- **A signal whose type is an enumeration shows the name, not the number.**
  `S_SEND` is what the reader wrote; `3'h2` is what the compiler made of it, and
  a reader watching a state machine should not have to hold the encoding in their
  head to follow it. This is a display rule and nothing more: EIR models an
  enumeration as the packed vector every operation on it treats it as, and the
  mapping is carried alongside, from the frontend that alone knew it, through
  elaboration and the protocol, to the row.

  Three things follow from the rule being about a *whole value of that type*. A
  slice is not named, because an enumeration names values and bit 1 of `S_SEND`
  is not a state. A row that has lost some of its bits is not named, because
  naming what is left would be naming a value other than the one on screen. And
  a group is not named, because a join of several signals is not of any one
  enumerated type — though what is inside the group keeps its own name.

  A value the enumeration does not name falls back to the number rather than to
  nothing. An X is such a value, and so is an encoding outside the declared set,
  and both are things the reader needs to see rather than be shielded from.

  **The naming can be turned off**, and choosing any radix turns it off. The
  encoding is sometimes exactly what is being debugged; more to the point, a
  radix menu that silently did nothing to a state register would read as a fault
  rather than as a choice. The way back is a `Symbolic` entry in the same menu,
  listed first, since it is where an enumerated row starts.

- **Parameters and localparams are rows like any other.** The objects window
  lists a scope's parameters after its signals, and one dropped into the wave
  window shows its value across the whole width of it. A design built out of
  three differently-configured copies of one module is a thing a reader has to
  be able to tell apart, and the way they do that is by putting `ACK_DELAY`
  beside the traces it explains.

  A parameter is **immutable and constant**, and those are two statements rather
  than one. Constant: its value is the same at every time, so it has no edges to
  step between and it is *not* clipped where the window's traces stop — a trace
  stopping there says "nothing is known here yet", which is true of a signal and
  false of a parameter, and before the first run the clip would hide it
  altogether. Immutable: it has nothing behind it to unpack into, so it offers
  no expander; a `+` that did nothing would be worse than no `+`.

  It has no slot, no trace and nothing to subscribe to, so its value travels with
  the row — including onto the clipboard, because there is nowhere for the other
  end to look it up. A parameterised module is already one analysed definition
  per parameterisation (4.4), so what each one resolved to is a fact the frontend
  records about that definition and nothing else could reconstruct.

- **What a scope lists is what the reader wrote.** The Objects window, and so every *Add to
  wave*, leaves out two things a reader did not write. One is the objects a frontend
  introduced (7.1, 9.2), whose storage it lists under the names the source gave it: the
  storage behind `.count(count_lin[I])` is the counter's `count`, inside the counter. The
  other is the copy a generate loop makes of its genvar in each block, `gen[3].I`, whose
  value is already the index in the block's name; a loop of sixty iterations would bury the
  signals beside it under sixty of them. VPI still shows a genvar in its block, as other
  simulators do, since an application may look one up.

- **An unpacked vector is a group in every respect, including what may be put in
  it.** Rows may be dragged out of one and back into it, and rows from elsewhere
  may be dragged in. Half a container — one that lets a bit be taken out but
  never put back — is not the same kind of thing as a group, and a bit removed by
  mistake could never be recovered.

  The cost is that a row labelled `count` can be made to show something that is
  not count's. That bargain was already struck the moment deleting a bit was
  allowed to narrow the row: the label names where the row came from, and the
  value says what is actually in it. A row whose children *are* exactly its own
  bits, in order, is recognised as the signal again — which is both what makes
  unpacking free to look at and what tells a reader they have put it back
  together.

- **Making a group must not disturb the rest of the window.** The same trap as
  adding and pasting, in a third place: rebuilding the model to re-parent a row
  makes the view forget every expansion in it. Rows that move are re-expanded by
  the window, because Qt tracks expansion by position and a row that moves loses
  it.

- **Highlighting.** A row may be given a background colour from a palette of
  five, or returned to the default. **The palette lives in the theme file**
  (10.5), as `wave.highlight.1` through `wave.highlight.5`; five colour literals
  in widget code would be five violations of the same rule. A highlight covers
  the whole row in both panes, the name and the trace, because they are one row
  to the reader and a highlight that stopped at the splitter would read as a
  rendering fault. The name and value on a highlighted row of the signal list
  are drawn in a colour of their own, `wave.highlight.text`, one for all five
  backgrounds: a theme that makes its highlights strong enough to find at a
  glance can then keep the text on them readable, rather than leaving it in the
  panel's colour, chosen for a background the highlight has replaced. A row
  with no highlight keeps the panel's text, and a selected one the selection's.

**The keyboard.** A modifier means what it means on the wheel: Ctrl zooms, Ctrl+`+` and
Ctrl+`-` by a step, Ctrl+F to fit the run and Ctrl+Shift+F to the cursors; Shift pans, with
Shift+← and Shift+→. A key without one is the signal list's own, as Qt's tree gives it: the
arrows move, pack and unpack, `+`, `-` and `*` pack and unpack, and typing the start of a name
jumps to the row. The traces hand such a key to the list, so it means one thing across the Wave
tab, and the window's wave keys act only while the focus is there.

**Finding a row by typing its name is kept on purpose.** It is why the keys above carry a
modifier or are punctuation, and a command MUST NOT take a key that types a letter or a digit in
the Wave tab: it would take away a way of finding a row that a reader may rely on without
knowing it has a name.

Panning is the middle button's alone. Ctrl+drag used to pan as well, and no
longer does: Ctrl means *zoom* on the wheel, and a modifier that meant zoom under
one hand and pan under the other is a coin toss every time it is reached for.

### 10.8 Signal list files

What the wave window shows can be saved to a file and loaded again, from the window or
when it starts. A reader who has spent twenty minutes arranging a large design's signals
must not have to do it again after every run, and a team must be able to keep the
arrangement for a block beside its sources.

**The syntax is Tcl's; the language is not.** A signal list is split into commands by
Tcl's own parser (`Tcl_ParseCommand`, from the library the theme already needs) and is
never evaluated. Quoting, braces, backslashes, line continuation and comments therefore
behave as in Tcl, without a lexer of Enzan's to keep in step with it. Nothing else does:

- **There are two commands**, below, and any other word in command position is an error.
  `set`, `foreach`, `proc` and `source` are not Tcl that is not supported yet; they are not
  part of the format. A signal list is a list, not a program — which is what lets the
  window write one back out, and what makes loading a list someone sent you a read rather
  than running their code.
- **There is no substitution.** `$` and `[…]` are ordinary characters, so a path through a
  generate block, `/tb/gen[0].q`, is written as it is printed and needs no braces.
- **`#` starts a comment only where a command could start**, as in Tcl. A comment after a
  command on the same line is written `;# …`.

**Two commands.**

```tcl
add wave ?options? path ?path …?
group ?options? name body
```

```tcl
# The control signals, then the counters.
add wave /tb/clk /tb/rst_n
add wave -radix dec -label lin0 /tb/cntr_lin_gen[0].count
group "Recursive counters" {
    add wave -r /tb/cntr_rec_i
    add wave -radix bin -contents {
        add wave /tb/bus[7]
        add wave -highlight 2 /tb/bus[3]
    } /tb/bus
}
```

What `add wave` adds depends on what the path names:

- **A signal or a parameter**, by the path the Objects window shows for it, adds one row.
- **A signal with selects after it** adds a row showing what they select, by the indices the
  design declares, as its source would write them (10.7). Of `logic [63:0][17:0] count`,
  `/tb/count[3]` is element 3, `/tb/count[3][7:4]` four bits of it and `/tb/count[7:4]` four
  elements, still an array; `/tb/v[0]` is the most significant bit of `logic [0:7] v`. A VHDL
  array is indexed as it is declared too. A part-select is the last select and runs in its
  dimension's direction, as the languages require, and what has no dimensions of its own, an
  `int` or a record, is indexed by its bits from 0. A select outside its dimension is a
  warning, naming the ranges the signal is declared with. The whole path is looked up as a
  signal first, then with one select fewer at a time, so a name that itself ends in brackets is
  never taken for a select.
- **A scope** adds what *Add to Wave* on that scope in the Structure window adds: a group
  named by the scope's path, holding its signals. With `-r` it adds what *Add to Wave
  Recursive* adds: the subtree's groups, nested as its instances are, in the Structure
  window's order. A scope with instances below it becomes a group holding its own signals in
  a group called `$top`, then a group for each instance below it, named by the instance's
  name; one without is the group of its signals. A group that would hold nothing is left out,
  so a subtree with no signals adds nothing. Both are defined as those menu entries rather
  than beside them, so that the two cannot come to disagree.

`group` adds a group named `name` holding the rows its body describes. The body is a
signal list of its own — `add wave` and `group` commands — and it may be empty, which is
why a group is a block rather than an option on each of the lines inside it.

| Option | Meaning |
|---|---|
| `-radix bin\|oct\|dec\|hex\|ascii` | the radix, and the number rather than an enumeration's name, as choosing a radix from the menu does; `ascii` shows the bytes as characters |
| `-symbolic` | an enumeration's names after all; applied after `-radix` wherever it is written |
| `-label <text>` | the row's name, for a single signal or a non-recursive scope |
| `-highlight <1–5>` | the row's background (10.7) |
| `-expand` | unpacked into its elements or its bits, exactly as unpacking leaves it |
| `-contents <body>` | unpacked, with exactly the rows `body` describes beneath it |
| `-r` | for a scope path, the whole subtree |

An option applies to every row its command adds. `group` takes `-radix`, `-symbolic` and
`-highlight`, which are what a group's own line carries.

An unpacked signal is a body rather than a list of bit numbers because that is what the
window lets it become: its bits can be deleted and reordered, and other rows can be dragged
into it (10.7). `-contents` describes any of that with the same two commands, each bit with
its own radix, label and highlight, so nothing the window can show is beyond the file.

**Saving writes what the window shows.** *Save Signal List* writes every row, top to
bottom, back to the file the list was last loaded from or saved to, asking only when there is
none, and *Save Signal List As…* to another; arranging, saving and running again is the loop
the file exists for. Loading the file rebuilds the same list:

- a label only where the row was renamed, `-expand` for a signal exactly as unpacking left
  it and `-contents` for one that has been changed since;
- signals, selects and parameters by path, never a scope or `-r`, because by then the rows
  exist one by one and may have been rearranged; a range of bits no select can name, part of
  one element and part of the next, is left out rather than written as something else;
- words quoted as Tcl quotes a list element, and only when they must be;
- nothing that varies from one save to the next — no date, no time, no user — so that
  saving an unchanged window writes the same bytes, and a list kept in version control
  shows a change only when the list changed.

Cursors, zoom and which groups are collapsed are not part of a signal list. They are where
the reader is looking rather than what they are looking at, and may follow as commands of
their own.

**Loading is pasting.** A file is read into the same pending rows a paste builds, and they
go into the window the way a paste puts them there. So:

- nothing already in the window collapses or expands (10.7);
- rows arrive in file order, whatever order the kernel answers in — a scope's objects come
  from the kernel, which may be on another machine (2.2);
- the whole file is read, and every path looked up, before any row is added, so a file
  with a mistake in it changes nothing rather than half of the window.

*Load Signal List…* replaces the window's list. Adding one list to another is what copying
and pasting are for.

**A mistake in the file is an error; a path the design lacks is not.** An unknown command,
an unknown option, a bad value or an unbalanced brace is reported with its file and line,
and nothing is loaded. A path that names nothing in this design is a warning, with its file
and line, and that row is skipped while the rest load. A list outlives the version of the
design it was saved from, and refusing a whole arrangement because one signal was renamed
would make the file useless at exactly the moment it is needed.

**From the command line, `--wave-list <file>`**, an option of `enzan` forwarded to the front
end (8.4): `enzan run --gui --wave-list <file> <top>`, or with `--gui
--attach`. Without `--gui` it is refused, because there is no window to show the list in.
A file that cannot be read is reported and the window comes up empty, as a theme that
cannot be read leaves the built-in colours.

**The window says what it did.** Adding signals from the Structure or Objects window writes
the `add wave` command that does the same into the transcript. The format is learnt by
using the window, and a line can be copied straight into a file. Only additions from the
design are echoed: moving, renaming and unpacking change rows already there, and saving is
how a whole arrangement is captured.

**It is not ModelSim's `wave.do`.** The commands are shaped like ModelSim's so that they
read familiarly, but its files are not loaded. A `wave.do` is a Tcl program, written by its
window, full of that window's own commands (`WaveRestoreCursors`, `configure wave`), and its
paths separate a generate block with `/` where Enzan's use `.` (5.1). Loading some of them
would be promising all of them.

### 10.9 The configuration directory

The window keeps what its reader has chosen about it in one directory, `$XDG_CONFIG_HOME/enzan`,
which is `~/.config/enzan` when that variable is unset. `enzan --help` says where it is, because a
setting kept in a file nobody can find might as well not be kept.

**Nothing in it may change what a simulation computes or records.** It holds how a window shows a
run, never what the run does. A run is reproduced from its command line and its files (3.8), and
a setting in someone's home directory that changed a result would be the cache section 8.2
refuses: found only after it had been trusted, and on a second machine not at all. So the kernel
reads nothing from it, and anything that would change a result belongs on the command line however
often a user gives it.

**Only the front end reads or writes it**, on the machine its window is on. A window attached to
a kernel on a compute server (2.2) is coloured by the settings of the person looking at it, and a
headless run makes no directory.

**The window makes it the first time it opens**: `config`, naming the theme, and
`themes/dark.tcl`, the default theme written out as a starting point. Each is written only when
it is missing, and nothing Enzan writes there replaces a file that exists: the directory is the
reader's, and a file they have edited is theirs. When it cannot be made — no home directory, a
file system that is read-only — the window opens with the default theme and says why in the
transcript. Settings are a convenience, and a window that refused to open without them would put
them before the design.

**`config` has Tcl's syntax and is not Tcl**, as a signal list is not (10.8): it is split into
commands by Tcl's parser, nothing is substituted and nothing evaluated. It has one command, which
names the theme as a file, relative to the directory or as an absolute path, or as one of the
themes built in:

```tcl
theme themes/dark.tcl
theme -builtin light
```

A built-in theme is named by an option rather than by a word of its own, so that no file,
whatever it is called, can be taken for one. Without a `theme` command the window uses the
default built-in theme, and `theme -builtin dark` says the same. A theme that cannot be read or
does not load leaves the default theme too, as 10.5 says, and so does a built-in theme this build
lacks — one a newer Enzan has — and the transcript names the file and the line.

**A command this build does not know is a warning, and the rest applies.** This is the one place
`config` departs from a signal list's rules, and it is deliberate: a newer Enzan may write a
setting an older one has never heard of, and a directory shared between the two must not lose its
theme in the older one. A mistake in a command that is known — `theme` naming no theme, or two —
and text that cannot be split into commands are errors, and then nothing in the file is used.

**The window's layout is kept there too**, in `layout.ini`: where the docks are and how large the
window is, written as it closes and read as it opens. It is Qt's own record of the docks rather
than a setting anyone writes, which is why it is a file of its own, in Qt's format, rather than a
command in `config`; deleting it opens the window as it first opened.

**The window changes `config` in place.** Choosing a theme rewrites the `theme` command where it
stands, adds one when there is none, or takes its line out for the default theme, and keeps
every other byte of the file, comments and unknown commands included: a file the reader has
annotated stays theirs. A file with a mistake in it is not rewritten, since where its theme is
would be a guess, and the theme chosen is then used until the window closes and said not to be
kept.

---

## 11. Build system and dependencies

### 11.1 Toolchain

- **C++20** (required by slang). GCC 13+ or Clang 17+.
- **CMake 3.24+**, with `CMakePresets.json` providing reproducible
  configurations.
- Linux only (section 1.3).
- `ccache` supported when present.

### 11.2 Dependency policy

**System packages are the primary acquisition method**, with `FetchContent` as
a fallback when a dependency is absent or too old. Every dependency MUST be
discoverable through a `find_package` call that produces a clear, actionable
error naming the package when it is missing. "Clearly recognisable
dependencies" means a user reading the CMake output knows exactly what to
install.

Vendoring in `third_party/` is reserved for cases where it is unavoidable — NVC
is the expected case, since it is consumed at a pinned revision of its internals
rather than as a released library.

Adding a dependency requires: an entry in this section, a `find_package` path
with a good error message, a `FetchContent` fallback, CI installation, and a
license check against section 1.4.

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
| cocotb | the cocotb golden test and regression suite (M4); never linked | `pip`, pinned in `tests/conformance/cocotb/requirements.txt` |

### 11.4 Build configurations

`Debug`, `Release`, `RelWithDebInfo`, plus an `ASan+UBSan` sanitiser
configuration used by CI. A `TSan` configuration is added when threading
arrives.

**Performance is measured in an optimised build, never in `Debug`.** A debug build of the
kernel runs an order of magnitude slower than an optimised one, and not uniformly: what
dominates its profile is what the optimiser removes, so tuning against it tunes the wrong code.
The profiling configuration is `RelWithDebInfo` with frame pointers kept throughout, the
dependencies included, so that a sampling profiler can walk a stack cheaply and without
recording each one whole. It is a local configuration rather than a CI job: a profile is read
by the person who took it.

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

**Nothing else is installed.** `enzan-vhdl-library` builds the standard libraries and has no use
afterwards, and the themes are compiled into the front end (10.5). A dependency built in the tree
installs nothing either: its own install rules would put a library's headers into every package
of Enzan, so the `golden.install` test fails on any file the table does not list. Documentation
and the licence are left to each package, which puts them where its distribution keeps them.

**`enzan` finds the rest of Enzan from where it is itself**, through `/proc/self/exe`, by paths
relative to its own directory that are compiled in. An installation therefore works wherever it is
moved, and two versions installed side by side each run their own parts. Every directory is held
relative to the prefix, including one a distribution's build macros give as an absolute path, and
one outside the prefix is refused when CMake configures, since no relative path made at build time
could be relied on to reach it.

**The build tree is laid out as an installation is.** A build directory is a prefix that has not
been installed: `enzan-gui` is built into its `libexec/enzan/` and the standard libraries into
its `lib/enzan/vhdl/`. The one lookup then serves both, and a build always runs its own parts
rather than an installed copy's.

**`lib/enzan`, not the distribution's library directory.** The standard libraries are compiled
files only Enzan reads, and `CMAKE_INSTALL_LIBDIR` is for libraries other programs link:
`lib/x86_64-linux-gnu` on Debian, `lib64` on Fedora. Both keep a package's private files in
`/usr/lib/<package>`.

**An installation is never written to.** Nothing Enzan does at run time writes beside its
executable — a library a user names is where they name it (8.2), and settings are in the
configuration directory (10.9) — so an installation its user cannot write to works as one they
can. `golden.install` runs one that has been moved and made read-only.

### 11.6 Releases

**A release is a tag, `vX.Y.Z`, on a commit whose version is `X.Y.Z` with no suffix.** The release
commit sets `project(VERSION)` and clears the default of `ENZAN_VERSION_SUFFIX`, so that anyone
who builds its tarball is told the version it is, rather than every package having to pass
`-DENZAN_VERSION_SUFFIX=` and anyone else building a `-dev`. The commit after it moves to the next
version, with `-dev` again.

**Its source tarball is made by `scripts/make-dist.sh`, not by GitHub.** The archive GitHub makes
of a tag leaves submodules out, so it holds no NVC, and building it downloads slang, which no
distribution packages. A distribution's builder has no network — Debian's, Fedora's and
openSUSE's do not, nor Launchpad's — so the tarball carries both: every submodule at the commit
the tag pins, and in `third_party/bundled/` the sources FetchContent downloads, slang's, the fmt
slang builds and GoogleTest's, which RHEL 9 packages older than the 1.12 the tests need. The
script finds them by configuring the tag with every download forced and copying what CMake
fetched, so the revisions slang pins for its own dependencies are restated nowhere.

**A bundled source stands where its download would.** A system package is still preferred where
one is found (11.2), and a checkout, which has no such directory, downloads as before. Nothing is
vendored by it: the directory exists only in a tarball. `ENZAN_ALLOW_FETCH=OFF` admits a bundled
source, since building one downloads nothing.

**The tarball records the revision it was made from**, in `.tarball-revision`, and `--version`
reports that rather than asking git. A tarball is not a checkout, and one unpacked inside a
distribution's packaging repository would be given that repository's revision.

**The same tag gives the same tarball**: its files in name order, owned by no one, dated at the
commit and compressed by one `xz` thread. A tarball can therefore be checked by making it again.

**A release is made by pushing its tag**, and the release workflow (16) does the rest but
publish. It makes the tarball and refuses a tag that is not the tarball's version, so a tag on a
`-dev` commit fails before anything is built; builds and tests every distribution's packages
(11.7); and attaches the tarball, the packages and their checksums to a draft release. A
maintainer reads the draft over and publishes it. Nothing is published unattended, because a
published release is never replaced: its tarball's checksum is in every recipe that names it.
`docs/releasing.md` has the steps in order.

### 11.7 Packages

**Enzan is packaged for the distributions its toolchain floor reaches**: Ubuntu 24.04, Debian 13,
Fedora, RHEL 9 and its rebuilds with gcc-toolset-13 and EPEL, and Arch Linux. Ubuntu 22.04 and
Debian 12 are not, their GCC being 12, below 11.1's floor; Ubuntu 22.04's Qt is 6.2 besides.

**The recipes are each distribution's own**, `packaging/debian/`, `packaging/rpm/enzan.spec` and
`packaging/arch/PKGBUILD`, not CPack's. A distribution's own tools build with its compiler flags,
hardening and LTO included, split the debug symbols into packages of their own and work out the
dependencies; and only a recipe in a distribution's format can later be built into a repository,
by Launchpad, COPR, OBS or the AUR. CPack makes binary packages and nothing a repository builds
from, and has no format for Arch at all.

**The packages are attached to a release, not served from a repository.** A repository, from
which a new release arrives with the system's other updates, is a later step, and the recipes are
what it would be built from.

**Every recipe builds from the release tarball** (11.6), never from a checkout or GitHub's
archive: with no network, and with `ENZAN_ALLOW_FETCH=OFF`, so that a download is an error rather
than an attempt. It runs the test suite, `golden.install` included, and installs with
`cmake --install`. Its version comes from the tarball's name, so the version is set in one place,
`project()`. A suffix is spelled `~dev` for dpkg and RPM and run on, `dev`, for pacman, which is
how each sorts `0.2.0-dev` before `0.2.0`.

**The front end is a package of its own, `enzan-gui`**, which depends on `enzan` at the same
version. The simulator's package depends on no GUI toolkit, so a compute server or a CI runner
installs Enzan without Qt (8.4). It suggests the front end rather than recommending it, since
apt and dnf install what is recommended; `enzan --gui` without it says it is not installed and
names its package.

**What a distribution packages, Enzan is built against.** The tarball's bundled sources are used
only where the distribution's package is missing or too old; Arch's mimalloc is used, and on
Fedora its Tcl 9.

**`packaging/build-package.sh <target> <tarball>` builds one distribution's packages in a
container of it**, as the release workflow does and as anyone can with Docker or Podman. What the
build needs is installed from the recipe into an image of its own; the build then runs with no
network; and the packages are installed into a fresh container, the simulator's alone first.
There `packaging/smoke-test.sh` runs a design of each language from them, shows that no Qt came
with the simulator, and starts the front end through `enzan --gui`.

---

## 12. Repository layout

```
Enzan-RTL-Simulator/
├── CMakeLists.txt            top-level build
├── CMakePresets.json         reproducible configurations
├── LICENSE                   GPLv3
├── README.md                 short orientation, points here
├── DESIGN_GUIDE.md           this document — the normative specification
├── .clang-format
├── .clang-tidy
├── .github/
│   └── workflows/            GitHub Actions CI
├── cmake/                    find modules and helper functions
├── include/
│   └── enzan/                ONLY headers third parties compile against
│                             (vpi_user.h, sv_vpi_user.h, later vhpi_user.h and
│                             svdpi.h)
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
│   └── cocotb/               cocotb's simulator makefile for Enzan (section 9.6)
├── tests/
│   ├── unit/                 GoogleTest, mirrors src/ layout
│   ├── golden/               self-checking HDL cases and expected output
│   ├── conformance/          harnesses for cocotb, sv-tests, ivtest, VHDL suites
│   └── designs/              scripts fetching external RTL — NEVER vendored
├── bench/                    performance benchmarks and result tracking
├── docs/                     human documentation, derived from this guide
├── packaging/                each distribution's recipe, and the scripts that build them (11.7)
├── scripts/                  repository checks and release tooling, runnable locally and by CI
└── third_party/              vendored dependencies where unavoidable
```

Rules:

- Headers live **beside** their sources inside each `src/` component.
  `include/enzan/` is reserved for the small set of headers that code outside
  this repository compiles against. Do not mirror the whole tree into
  `include/`.
- `tests/unit/` mirrors the `src/` layout so that the test for a file is always
  findable at the corresponding path.
- `tests/designs/` contains *scripts that fetch* external RTL at pinned
  revisions. External designs MUST NOT be committed to this repository: they
  are large, and they carry their own licenses.
- Each `src/` component has a `README.md` describing its responsibility and its
  allowed dependencies on other components. The build enforces its presence.
- `scripts/` holds checks that CI runs and that a contributor MUST be able to run
  locally with no arguments. A check that exists only inside a CI workflow cannot
  be run before pushing, so it gets discovered as a red default branch instead of
  as a local failure.

---

## 13. Coding standards

### 13.1 Naming

Conventions follow slang's, since slang code and Enzan code sit side by side
and mixing two styles in one translation unit is worse than either style alone.

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

- `.clang-format` is authoritative. Formatting is not a matter of judgement and
  is enforced in CI.
- `.clang-tidy` is enforced in CI. Suppressions require a comment giving the
  reason.
- Prefer `enum class` over plain enums, `std::span` over pointer+length,
  `std::string_view` over `const char*`, and RAII over manual cleanup
  everywhere.
- Exceptions are used for genuinely exceptional conditions only, never for
  control flow in the kernel hot path.
- No raw `new`/`delete` outside arena and allocator implementations.

### 13.3 Documentation comments

Every public class and every non-trivial public method MUST carry
a Doxygen `///` comment.

A good comment explains the **contract and the reasoning** — preconditions,
invariants, ownership, why this approach was chosen. A comment restating the
signature in prose is noise and should not be written:

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
- Prefer contiguous structures over pointer-chasing ones; the state arena
  (4.3) exists for this reason.
- Do not optimise without a measurement. `bench/` exists so that performance
  claims can be checked rather than asserted.

---

## 14. Testing

Testing is continuous, not a phase. **Every feature ships with its tests in the
same commit.**

### 14.1 Unit tests

GoogleTest, under `tests/unit/`, mirroring `src/`. Cover the feature and its
error paths, not just the happy path.

### 14.2 Golden tests

Small HDL cases under `tests/golden/`. **Self-checking testbenches are strongly
preferred** — a testbench that calls `$fatal`/`report failure` on mismatch needs
no golden file to drift out of date. Where output comparison is genuinely
needed, compare transcripts or VCD.

### 14.3 Differential testing

Running the same design under Enzan and under a reference simulator (Icarus
Verilog, Verilator, NVC, GHDL) and comparing results is **the single
highest-value testing strategy available** and MUST exist from M1.

One practical caution: two correct simulators may legitimately differ in
delta-level ordering. Comparisons MUST be made at settled time boundaries, not
at every delta, or the suite will produce constant false failures and be
switched off.

**The VHDL references are not interchangeable.** Because NVC supplies Enzan's
VHDL frontend (5.2), NVC is not an independent oracle for anything the frontend
decides: a shared parsing or semantic-analysis defect produces the same wrong
answer on both sides and the comparison passes. **GHDL is therefore the primary
VHDL differential reference and MUST be part of the suite**, with NVC kept as a
secondary reference that is still meaningful for everything downstream of EIR.
The same reasoning will apply to any future frontend dependency.

A VHDL golden case is compared with GHDL on two things: what the design reported, as each
report's severity and message in order, and every signal GHDL records, as the value it
settled on at each time step. GHDL writes `std_logic` as its nine values, which are mapped
as 3.6 maps them before comparing; and the time step a run finishes in is left out, since
GHDL stops before it writes that step and Enzan writes it.

### 14.4 Conformance suites

Run against public suites and **track the pass rate as a number over time**:
`sv-tests` (CHIPS Alliance) for SystemVerilog, `ivtest` for Verilog, and for VHDL
the VESTs suite and GHDL's and NVC's own test suites. A published compliance
percentage is also a credible public signal of maturity.

The GHDL and NVC suites are both run for the reason given in 14.3: NVC's suite
cannot exercise a defect Enzan inherited from NVC's own frontend. VESTs is
GPL-2.0 and ships inside GHDL's test suite, and every one of its compliant cases
checks itself, reporting `PASSED TEST` or `FAILED TEST`.

**The pass rate MUST NOT regress.** This is a ratchet: a change that lowers it
is a defect unless the regression is explicitly justified.

`tests/conformance/sv-tests/run-suite.sh` runs sv-tests. It fetches the suite at the revision
named in `revision.txt` beside it, the one place that revision is written, and runs every case
under the suite's `tests/` through **sv-tests' own driver** with an Enzan runner. So a case
passes by sv-tests' rules — the tool succeeding or failing as the case expects, and every
`:assert:` a simulation prints holding — rather than by a reading of them. A preprocessing-only
case is skipped, as sv-tests skips a case of a kind a tool does not run, and is in neither
count. A case that expects the tool to fail passes whenever Enzan fails, an unsupported construct
included; those passes count, being sv-tests' definition, and are reported as a second number.

`tests/conformance/vests/run-suite.sh` runs VESTs from GHDL's test suite, fetched at the
revision its `revision.txt` names: every compliant VHDL-93 case the suite's own `.exp` files
run, by their rules as GHDL's `testsuite.sh` reads them — the units an `.exp` builds before a
case analysed with it, the top its `ENTITY=` names or else the last entity named as a top, its
`STOP=` time, `--assert-level error`. A case passes when Enzan runs it to its end and it reports
no `FAILED TEST`. The cases run in parallel but for those using files, which run one after
another in the suite's order, since the suite reads back in a later case what a simulator wrote
in its own format in an earlier one, and gives several pairs of cases one file name.

`tests/conformance/nvc/run-suite.sh` runs NVC's own regression tests, from the NVC vendored
under `third_party/nvc` and so at its revision: every test `test/regress/testlist.txt` lists,
by its options as NVC's `run_regr.c` reads them — the revision of the language, `stop=`, `fail`
for a test that must fail, `gold` for one whose transcript must hold each line of its gold file
in order. NVC writes a message's time and delta before it, which Enzan does not, so a gold line
is matched without them. A test of what only NVC has, VHPI, its shell, PSL, coverage, waveform
dumps, Verilog or its relaxed rules, or one setting generics on the command line, is in neither
count. Its `mixed` tests, VHDL with Verilog, are run apart, `run-suite.sh ... mixed`, by a build
with both frontends and against `passing-mixed.txt` (M7). Several fail by design: NVC shows
Verilog's `x` as `'U'` where 3.6 shows `'X'`, and counts a delta for each Verilog net a value
crosses where 3.5 counts one for the boundary.

`tests/conformance/ghdl/run-suite.sh` runs GHDL's own regression tests, its `testsuite/gna`, from
GHDL at the revision VESTs is run from. Each test's `testsuite.sh` runs unchanged under GHDL's
`testenv.sh`, with `ghdl-shim.sh` as the `ghdl` it calls: each GHDL library is an Enzan library
(8.2), the shim's `-a` analyses into the one `--work` names against the others, and `-e` and `-r`
elaborate or run a top from them, `--expect-failure` inverting what Enzan's exit says. A test
passes when its script exits 0. A test of what only GHDL has, synthesis, VPI or VHPI, GHW
waveforms, PSL, a C compiler or a feature it probes for, is in neither count; one comparing
GHDL's own messages or using its vendors' IEEE packages fails. So does one expecting an analysis
error that NVC, which Enzan analyses with, does not report until elaboration or not at all.

The ratchet is a list for each suite, as 14.6 describes, and one script,
`tests/conformance/ratchet.sh`, holds every suite to its list.

### 14.5 Design regression

Real designs, fetched at pinned revisions:

- SystemVerilog: secworks `aes`, fpganinja `taxi`, PULP `common_cells`; then the
  processors picorv32, VexRiscv (generated), Ibex and CVA6, which each need a program
  compiled for them.
- VHDL: NEORV32; then SLAC `surf`, an IP library whose cocotb testbenches exercise its RTL as
  `taxi`'s do, and the VUnit and OSVVM suites, which need the verification-library language of
  M9+.

Start with the small ones and add larger designs as capability grows.

NEORV32's is held to GHDL's reports and to the CPU trace and UART logs its testbench writes, for
the 160 us its on-chip debugger sequence takes, since those hold every instruction each core
retired, and so is the same testbench in Verilog around its VHDL (M7); on the main branch only,
as it takes minutes.

Each design is a directory under `tests/designs/` holding the revision it is pinned at and a
`run.sh` that fetches it there (`tests/fetch-revision.sh`, which every suite shares), runs its
own testbenches and says PASS or FAIL for each. A testbench passes when it reports success
and, where the reference simulator runs it — Icarus Verilog for SystemVerilog, GHDL for VHDL
(14.3) — when Enzan's transcript is the reference's line for line, so
a pass is a design checking its own answers and two simulators agreeing on them. Testbenches
the reference cannot run are held to their own verdicts alone, and the harness says why. A
testbench or a case that cannot pass for a reason outside Enzan — a design slang rightly
refuses, a testbench that assumes a two-state simulator — is named, with the reason, in the
design's `expected.txt`; any other failure fails the run.

Where a design's own testbenches leave a module out, or reach it only through constructs Enzan
does not have yet, self-checking testbenches for it sit in the design's `tb/`. Each compares
the module against a model of what it should do, and exits non-zero on a mismatch. A
testbench written here can be wrong as easily as the simulator. Before it is committed it is
run under a second simulator, and it must pass there too; one that none runs, a testbench in one
language around a design in the other that NVC cannot instantiate, is a translation of the
design's own, and its transcript must be that one's under the reference.

### 14.6 cocotb regression

Once VPI lands (M4), cocotb's own regression suite runs in CI and its pass rate
is tracked like any other conformance number.

`tests/conformance/cocotb/run-suite.sh` runs it: it fetches cocotb's source at the tag of
the cocotb installed beside it, runs every directory of `tests/test_cases` with `SIM=enzan`
exactly as cocotb's own CI runs them, and prints one line per case and a pass rate. A case
passes when its `make` succeeds, which is cocotb's own definition.

**A pass that never ran a test is said to be one.** Many cases skip themselves for anything
that is not a VHDL simulator or Verilator, and the ones that expect a run to fail pass when it
fails at analysis. Both count, because that is cocotb's definition, and both are marked, with a
second number for the passes in which cocotb actually ran under Enzan: only those say anything
about VPI.

**The cocotb version lives in one file**, `tests/conformance/cocotb/requirements.txt`, which
CI installs from. The suite is fetched to match whatever is installed rather than restating
the version, so the two cannot disagree.

**The ratchet of 14.4 is a list, not a number.** `tests/conformance/cocotb/passing.txt` names
every case that passes, and the run fails when one of them stops passing; the check is
`tests/conformance/ratchet.sh`, which sv-tests uses too. A pass rate alone
would let one case start passing while another stopped and report no change at all. A case
that newly passes is reported, and is added to the list in the same commit as the change that
made it pass.

The rate is low until M5, and that is expected rather than a defect of M4: most of cocotb's
cases share one design that uses unpacked arrays, structs, `real`, `string` and interfaces,
and a design that does not analyse fails every test in it. The cases that do analyse are what
exercise VPI, and the golden cocotb test of `tests/golden/` pins the VPI behaviour cocotb
relies on whatever the rate is.

**A VHDL toplevel is a second run of the same suite**, through VHPI (M6, section 9.7). Most of
cocotb's cases have a VHDL design beside the Verilog one, chosen by `TOPLEVEL_LANG=vhdl`, so the
suite runs twice, `run-suite.sh <enzan> <work> vhdl` the second time, and each run has its own
pass list, `passing.txt` and `passing-vhdl.txt`: a case passing in one language says nothing
about the other. The golden cocotb test has a VHDL twin, `tests/golden/vhdl/cocotb`, which pins
the VHPI behaviour cocotb relies on as the other pins VPI's.

### 14.7 Sanitisers and fuzzing

ASan and UBSan run over the unit and golden suites in CI. TSan is added when
threading arrives. Frontend fuzzing with mutated HDL input is added once the
frontends are stable.

---

## 15. Performance methodology

### 15.1 Target

**The target is provisional.** It will be replaced with a measured, committed
figure once M1 produces real numbers; setting a hard number before any
measurement exists would be guesswork.

Provisional: on the order of 10,000 simulated clock cycles per second on a
design of roughly 100k FPGA LUTs on a single core, and within about 5× of
Verilator on the purely synchronous subset.

**The binding requirement for M0–M2 is not the number — it is that `bench/`
exists and records cycles/sec per commit from the very first milestone.** A
performance goal without continuous measurement is decoration, and performance
regressions are invisible without a baseline.

### 15.2 Honest expectations

A tree-walking interpreter will not approach this target. The bytecode VM is a
large step up; the JIT (6.3) is what is expected to reach it. This is why the
execution model was decided before implementation began: it constrains EIR
design, and discovering the constraint later means a rewrite.

### 15.3 Benchmarks

`bench/` holds a fixed set of pinned designs with fixed stimulus. CI records
cycles/sec per commit. Once a threshold is agreed, a regression beyond it should fail CI,
in a nightly job if one is added (16).

---

## 16. Continuous integration

**GitHub Actions.**

**What runs on a development branch or a pull request must be fast.** GitHub gives the
project a monthly allowance of CI time, and every push to every branch spends it. A job or
step that takes long runs on `main` only, with `if: github.ref == 'refs/heads/main'`.

**Everything still runs before a pull request is merged, locally.** A main-only check that
fails after the merge leaves `main` red. So whoever merges runs the main-only jobs on their
own machine first, with the same commands the workflow uses.

Jobs on every branch and pull request:

- Build matrix: {GCC, Clang} × {Debug, Release}.
- Unit and golden test suites.
- Sanitiser build (ASan+UBSan) over unit and golden suites.
- `clang-format` check.
- cocotb's regression suite (14.6) and sv-tests (14.4), in the job that already builds the
  SystemVerilog frontend, since a second job would build slang a second time to run them; and
  there, the one build with both frontends, NVC's `mixed` tests and the mixed golden cases.
- VESTs, GHDL's and NVC's suites and cocotb's with VHDL toplevels (14.4, 14.6), in the build
  matrix's GCC Release job.
- The design regression of 14.5 for the designs that run quickly: `aes` and `common_cells`.

Each job builds a configuration chosen so that code compiled only with a component, or only
without one, is compiled somewhere with warnings as errors. The job that builds slang builds the
defaults, every optional component on, which is what a contributor's plain configure gives. The
build matrix and the sanitiser build the defaults but for the SystemVerilog frontend, which one
job is enough to prove. One job builds the GUI with neither language frontend, since what is
compiled only without them is compiled nowhere else.

Jobs on `main` only:

- `clang-tidy`, which analyses every source file.
- The design regression for `taxi`, whose cocotb testbenches take minutes, and for NEORV32,
  under its VHDL testbench and the Verilog one (14.5).

The release workflow runs on a tag only, `vX.Y.Z`, and by hand: it makes the source tarball,
builds and tests the packages of every distribution of 11.7, each in a container of its own, and
from a tag creates a draft release with them (11.6). Run by hand on a branch it tries the
packaging and releases nothing, which is how a change to it is tried before a release depends on
it.

There is no scheduled job. A nightly run would be a useful addition once there is somewhere to
publish its results: conformance pass rates over time, the benchmark series of 15.3, and the
design regressions too long for every commit.

Dependency builds and `ccache` are cached. The default branch MUST stay green;
a red default branch is fixed or reverted before other work continues.

---

## 17. Working agreements for agents

### 17.1 Commit messages

Every commit carries a category prefix. When a change is confined to a
particular directory or file, the path follows the category as further
colon-separated segments:

```
<category>: [<dir1>: [<dir2>: [<file>: ]]]<Subject line>
```

Path segments are lowercase and mirror the source tree, even though source
files are named in PascalCase. The path prefix is used only when the change
genuinely touches just that area; a change spanning several components carries
the category alone.

Categories: `build`, `ci`, `design`, `docs`, `eir`, `elab`, `frontend`, `gui`,
`session`, `ipc`, `wavedb`, `pli`, `runtime`, `codegen`, `test`, `bench`, `fix`,
`perf`, `refactor`, `license`.

Examples:

```
build: Add CMake presets for gcc and clang
runtime: scheduler: Drain the NBA region before Observed
fix: frontend: sv: Resolve parameters before port binding
test: golden: picorv32: Add differential VCD comparison
docs: Describe the mixed-language delta mapping
```

The subject line is imperative, capitalised, and under 72 characters. The body
explains **why**, not what — the diff already shows what.

### 17.2 Commit granularity

**Commit incrementally.** Each logical change is its own commit with its own
descriptive message. Do not accumulate unrelated work into a large commit.

The project is built largely by AI agents, which makes the commit log the
primary record of what was changed and why. A single large commit destroys that
record and makes bisecting a semantic regression — the hardest class of bug
here — effectively impossible.

### 17.3 Definition of done

A feature is not done until all of the following hold:

1. The implementation exists.
2. Unit tests cover the feature **and its error paths**.
3. If it affects simulation semantics, a golden or differential test pins the
   behaviour.
4. New public classes and non-trivial public methods carry `///` comments.
5. `clang-format` and `clang-tidy` are clean.
6. CI is green, including the sanitiser job.
7. If behaviour described in this guide changed, this guide was updated **in
   the same commit**.
8. Conformance pass rates did not regress.
9. Benchmark results did not regress beyond the agreed threshold.

### 17.4 Standing invariants

These are the rules most easily broken by accident. Violating one is a defect
even if tests pass:

- **Determinism** (3.8). Never let hash container iteration order, pointer
  values or allocation addresses affect simulation results or output ordering.
- **Process purity** (4.2). A process reads and writes only its declared sets
  in the state arena. Never reach arbitrary global state from a process.
- **Four-state shape** (3.6). Never write a value path that assumes a single
  bit plane, even while only two-state is implemented.
- **No flattening in frontends** (5). Flattening is an elaboration-time
  optimisation only.
- **Thin frontend adapters** (5.2). Keep slang- and NVC-specific code confined
  to its own directory. It must not leak into EIR or downstream.
- **Opcodes carry their lowering** (6.1). A new bytecode opcode without a
  documented LLVM lowering sketch is incomplete.
- **No colour literals in GUI code** (10.5). Colours come from the theme.
- **No allocation on the hot path** (13.4).

### 17.5 Repository hygiene

Never commit: build directories, generated files, external RTL designs
(`tests/designs/` fetches them), waveform dumps, or simulation libraries.

### 17.6 When to stop and ask

Stop and ask rather than guessing when: a task depends on a decision this guide
marks *(open)*; an implementation would require violating a **MUST**; a new
dependency seems necessary; or the language standards appear to conflict with
what this guide specifies. In the last case, the standard is probably right and
this guide needs fixing — but that is a decision to surface, not to make
silently.

---

## 18. Roadmap

Milestones are gated by their exit criteria. Do not begin the next milestone
while the current one's exit criteria are unmet.

### M0 — Foundation *(first)*

Build system and test infrastructure before any simulator code.

CMake skeleton and `CMakePresets.json`; dependency discovery with good error
messages; GoogleTest wired up; `.clang-format` and `.clang-tidy`; GitHub
Actions running build, test, sanitiser and format jobs; the `common/` component
with the diagnostic engine and source manager; `enzan --version`.

**Exit:** CI green on all jobs; at least one real unit test; a contributor can
clone, configure and build from the README alone.

### M1 — SystemVerilog subset simulating, waveform out

slang integration; EIR-S and EIR-B; elaboration and the state arena; bytecode
compiler and VM; scheduler with Active, Inactive, NBA and Postponed; four-state
value representation with a two-state fast path; `always_ff`, `always_comb`,
`always @`, `initial`, `assign`, `if`/`case`, arithmetic, packed vectors,
hierarchy; `#delay`; `$display`, `$time`, `$finish`; VCD and FST writers;
`bench/` recording cycles/sec.

The GUI is deliberately **not** in this milestone. Waveforms are viewed in
GTKWave or Surfer. This reaches a visibly working simulator far sooner and
de-risks the kernel independently of the GUI, and the export path is needed
permanently regardless.

**Exit:** a counter and a FIFO testbench simulate correctly; waveforms open in
GTKWave; results match a reference simulator; cycles/sec recorded.

### M2 — Interactive GUI

Kernel/GUI process split and IPC protocol; Qt 6 application with Structure,
Objects, Wave and Transcript windows; live waveform database; downsampled
waveform rendering; cursors, zoom, grouping, radix selection; dark theme from
Tcl configuration.

**Exit:** a design runs interactively with signals added from the GUI and the
waveform progressing live.

### M2.1 — Waveform interaction

The mouse and keyboard model of section 10.7: the wheel scrolling with Ctrl to
zoom, right-drag to zoom to a range or to fit, left-click selecting a signal and
snapping the cursor to a nearby edge, cursors that carry their time and can be
renamed and stepped from edge to edge, and a signal list whose rows can be
reordered, unpacked into their bits and highlighted. With the database query the
last of those needs (7.1).

Placed here, straight after the GUI, for three reasons. It is contained entirely
in the components M2 has just built, while that code is still warm. Nothing later
blocks on it, so putting it first costs no other milestone anything. And design
priority 1.2 puts usability above both performance and language breadth: the
wave window is the part of Enzan a user touches most, and leaving its controls
until after the VHDL frontend would be ordering the work for everything except
the person using it.

It is a point release rather than a milestone of its own because none of it
changes what Enzan can simulate, record or export. Every item is about how the
window is driven.

**Exit:** every gesture in section 10.7 works and is covered by a test; a reader
can lock one cursor on an edge, step a second along the same signal with Tab, and
read both times off their labels without touching the mouse.

### M2.2 — Signal list files

The signal list files of section 10.8: saving the wave window's list and loading it again
from the window or with `--wave-list`, and the `add wave` echo in the transcript.

Numbered with the window it belongs to, and placed after M4 because that is when large
designs made it needed: arranging two hundred signals by hand on every run is the cost a
reader pays for not having it. Like M2.1 it is a point release — it changes nothing Enzan
can simulate, record or export — and it is contained in the front end and one forwarded
option, so nothing waits on it or is held up by it.

**Exit:** a list saved from the window and loaded into a fresh one gives the same rows, with
the same labels, radices, highlights, ranges and unpacked contents, and saving that again
writes the same bytes; a file written by hand with a scope, `-r` and a nested group loads;
a path the design lacks is skipped with a warning naming its line, and a syntax error loads
nothing; `enzan run --gui --wave-list` opens with the list in place.

### M3 — Command line, file lists and libraries

The subcommands and exit statuses of 8.1. File lists with `--dry-run`, `--depfile`
and `--depfile-target` (8.3). Named, opt-in analysis libraries with `lib create`,
`lib list`, `lib clean`, `--out-lib`, `--in-lib` and `--context` (8.2, 4.4), which
requires two things EIR does not have yet: **a logical library on every unit**, so
that two libraries defining one name collide loudly rather than silently, and **an
entry path derived from the unit name** rather than from a hash, so that `make` can
depend on it. A library index carrying the logical name and the format version.
`--gui` as an option of `enzan`, with the lifetimes of 8.4.

The per-source hit rule of 4.4 is **not** in scope. It is a property of the frontend
rather than of the store — a module cannot be type-checked without the packages it
imports, so today one changed file re-analyses all of them — and changing it is
frontend work that a library's shape does not depend on. What M3 owes is a library
whose entries `make` can name and a boundary rule a user can follow, so that finer
incrementality is a frontend change later rather than a redesign.

**Exit:** the Makefile of 8.3 builds and simulates a design split across three
libraries; editing one library's source rebuilds that library and the libraries
analysed against it and no other, editing a context file rebuilds every library that
named it, and editing nothing rebuilds nothing; two libraries defining one name are
reported rather than silently resolved; `enzan run --gui` opens a window on a design
it elaborated, and closing that window ends the kernel, while closing a window that
reached the kernel through `--gui --attach` leaves it running.

### M3.1 — Analysis cost proportional to the design

One translation per parameterisation rather than per instance. slang creates a
separate body symbol for each instance, so a module was translated once per
instance and every translation but one discarded; a hundred instances of one module
cost a hundred translations of it.

Placed here rather than deferred to M8 because it is not an optimisation of the
simulation, it is analysis doing a hundred times the work it needs to on any design
that instantiates anything more than once — which is every real design. M8's subject
is how fast a *run* is, and none of the measurement discipline there is what this
needed.

A port map keyed on the frontend's own symbols is what made the naive version
unsafe: such a map belongs to one instance's body, and sharing it binds no
connections while leaving the design elaborable and runnable. **A frontend
structure shared between instances MUST be keyed by something the source names**,
not by a pointer the frontend happened to produce.

**Exit:** analysing a design whose instances share a parameterisation costs what
one of them costs, and a test pins each instance reaching its own port storage
rather than pinning the sharing.

### M4 — VPI and cocotb

VPI subset per section 9; `vpi_user.h`; callback plumbing into the scheduler
regions; loading PLI shared objects.

**Exit:** cocotb's own regression suite runs with a recorded pass rate.

### M5 — Four-state completion and language breadth

Full four-state throughout; X-propagation; nets, drivers and resolution per 3.7;
tasks, functions, packages, interfaces, generate, structs, enums, arrays; the top
level's parameters set from the command line; `sv-tests` pass rate tracked.

**`string` stays a literal.** A string variable has no fixed size, while 4.3 lays out
all state at elaboration and 13.4 forbids allocating on the write path, so storing one
is a design of its own. The exit designs below are chosen to need none, and the cocotb
cases whose shared design declares one keep failing until it is taken up (14.6).

**The exit designs need no program to run.** A processor tests a simulator only as
well as the software it is given, and running one would bring a cross-compiler into CI
and, for a core iverilog cannot read, a second reference simulator. Three designs cover
the language M5 adds without either:

- **secworks `aes`**: Verilog with functions and memories, and a self-checking
  testbench against the NIST vectors that iverilog also runs, so it is compared
  differentially (14.3).
- **The AXI-stream components of fpganinja `taxi`**: interfaces and modports, enums
  and generate. Each is tested by its own cocotb testbench, which needs
  `cocotbext-axi` beside cocotb and sets the top level's parameters from its makefile.
- **PULP `common_cells`**: packages and packed structs, which `taxi` does not use.
  Tested by those of its own testbenches that use no classes, queues, `randomize` or
  `real` variables, and by self-checking ones kept beside its fetch script for further
  modules. Its concurrent assertions are SVA, which comes later, and are compiled out
  with its own `ASSERTS_OFF`; its immediate ones run.

**Driving disjoint parts of one signal is the case to get right**, not an edge. A
parameterised number of instances with ports connected to an array is ordinary RTL.
Overlap, not sharing a signal, is what multiple driving means, and a variable driven
in disjoint parts is legal where an overlapping one is an error.

**Exit:** `aes` matches iverilog; the `taxi` AXI-stream tests and the `common_cells`
testbenches above pass under Enzan; a design whose instance count is a
parameter and whose ports are bits of one array simulates, while two drivers of one
bit are still refused — a variable with an error and a net by resolving them.

### M6 — VHDL via NVC

NVC integration spike first, to confirm the approach in 5.2 before committing
to it; NVC vendored at a pinned revision; NVC tree to EIR adapter; `std_logic` value model;
VHDL generic and subtype elaboration, with generics folded by the adapter (5.2); Enzan's own implementations of the predefined and foreign subprograms NVC's
runtime would have resolved; VHDL simulation cycle; VHPI subset for cocotb.

**M6 is cut at the language designs are written in, not the language verification
libraries are written in.** Synthesisable VHDL and the testbenches shipped beside it use
almost nothing of what VUnit and OSVVM are built from: protected types, generic packages,
configurations, external names, and access types used as a general heap. Measured over
their sources, VUnit allocates in 37 files and OSVVM declares protected types in 31 and
configurations in 389, while NEORV32 does none of these. The verification libraries come
with the other verification features in M9+, so that the VHDL frontend, the simulation cycle
and VHPI are proven on designs before they are asked to carry a library's runtime.

**Access types are in M6 only as far as `std.textio` needs them.** NEORV32 cannot be
simulated without `line`: its UART prints what the program sends through `write` and
`writeline` in every simulation, from its RTL rather than its testbench, and the testbench
logs both UARTs to files and can load a memory from one. So M6 builds the heap of 4.3 —
access values are handles in the arena into a heap the kernel owns, since 4.3 lays out every
piece of state at elaboration and 13.4 forbids allocating on the write path — and makes it
work for an access to an array, as `std.textio` uses one, with textio's subprograms run
natively over it (4.3). M9+ uses the same heap for everything else.

The exit designs need no program compiled for them, for the reason M5 gives:

- **NEORV32**, simulated by its own testbench with the program image its repository
  commits. The testbench drives the on-chip debugger over JTAG — halting the CPU, writing
  memory through it, resetting and resuming it — and asserts each step.
- **cocotb's VHDL designs**, through its own test cases with a VHDL toplevel (14.6). These
  are what VHPI is measured by, as cocotb's suite measured VPI in M4.
- **VESTs and GHDL's and NVC's own suites** (14.4). Most of each needs nothing M9+ brings,
  and a pass list per suite lets the rest wait without hiding a regression.

**Exit:** NEORV32's testbench completes its debugger sequence under Enzan with no assertion
failing, and its transcript is GHDL's line for line; cocotb's suite runs with a VHDL
toplevel with a recorded pass rate; VESTs and GHDL's and NVC's suites run with recorded pass
rates.

### M7 — Mixed language

A design whose hierarchy crosses between SystemVerilog and VHDL, in either direction and at
any depth, simulated by the one kernel of 3.5. Everything below the frontends already serves
both: EIR, elaboration, drivers and resolution (3.7), the kernel, the waveform database and
the PLI context hold one design whatever language each definition came from. What M7 adds is
the meeting of the two frontends and of the two interfaces.

**SystemVerilog's time units come first.** A design with VHDL in it counts femtoseconds (5.2),
so a SystemVerilog delay counted in the run's unit rather than its module's would make every delay
of a mixed design wrong. Delays, `$time` and `%t` follow each module's time unit (3.1), and a
module stating no time scale takes `--timescale`'s unit, or picoseconds.

**Analysis of both languages in one session.** The session analyses SystemVerilog through
slang and VHDL through the NVC child into one `eir::Design`. An instance of the other language's
unit names a placeholder and an `eir::UnitRequest`, which the other frontend answers, slang through
a module instantiating the one asked for and the adapter by translating the entity for the values;
the session links the two by port name. A design of both is not stored in a library yet, nor
given `-G` overrides. VHDL goes into Enzan's libraries as SystemVerilog does, `--out-lib` and
`--in-lib` (8.2), and a design may draw on several VHDL libraries at once, not only the one `--work` names, since a mixed design is
usually built as a testbench in one library and its DUT in another. Both are also what the GHDL suite's multi-library tests and cocotb's VHDL designs in
their own libraries need (14.4, 14.6).

**Instances across the boundary.**

- *SystemVerilog instantiating a VHDL entity.* The entities' interfaces, their generics and
  ports with their types, are given to slang as the modules it would otherwise not find, so
  that the instance type-checks; its parameter values are the generics' values, and the
  architecture is the entity's default one (5.2), since choosing another from SystemVerilog takes
  a `config`, which Enzan does not read.
- *VHDL instantiating a SystemVerilog module.* A component with no VHDL entity to bind is
  bound to the module of its name (the default binding of IEEE 1076 section 7.3.3 extended
  to the other language, as commercial tools extend it); the component's generics are the
  module's parameter overrides. `entity work.m` does not name a module: NVC's analysis looks
  for an entity of that name and finds none.
- *Specialisation across the boundary.* VHDL generics are folded by the adapter (5.2) and
  SystemVerilog parameters by slang, so each side's instance asks the other's frontend for
  the specialisation its values make. The session alternates between the frontends until
  neither asks for one the other has not given, as elaboration asks the VHDL frontend for
  resolution functions (3.7), with a bound on the rounds so that a hierarchy recursing
  through both languages is reported rather than followed for ever. A port's width may depend on
  generics in any way VHDL can say, so the module an entity stands in as has ports as wide as a
  translation for the instance's values made them, from a table each translation adds a row to,
  and one bit wide until one has: instantiating VHDL from SystemVerilog takes a second round.

**Ports across the boundary.** A port whose two sides are of different languages is
connected by a zero-delay continuous assignment that converts between them and consumes one
delta cycle (3.5), in each direction its mode needs. Elaboration makes them, from what the VHDL
side's type is made of, so neither frontend converts anything: into VHDL by a transaction, and
into SystemVerilog through the non-blocking region and then a continuous assignment, so that a
net it drives still resolves its drivers:

- `std_ulogic` codes to four-state bits with `L`→`0`, `H`→`1` and `W`, `U`, `-`→`X` (3.6),
  and `0`, `1`, `X`, `Z` back to their codes; `bit` and `boolean` to two-state bits and back;
- `std_logic_vector` and its kin to packed vectors, left element to the most significant bit;
  integers to `int` or `longint` by their range; arrays of these to unpacked arrays and records
  to packed structs, field by field, where the shapes agree;
- an `inout` port as two conversions, split as 3.7 splits a port's driving value from its
  effective value: the inner port's driving value, what its own drivers resolve to, goes out as
  one driver of the outer signal, which resolves it by its own language's rule — a `std_logic`
  signal's function, a net's table — and the outer signal's value comes back as the inner port's
  effective value, not as a driver. Were each side's value a driver of the other, a value driven
  once would be held for ever: a port released to `Z` would go on seeing its own `1`.

A port no rule covers is refused by name: a real, until the SystemVerilog frontend has `real`; a
VHDL port of an unconstrained type instantiated from SystemVerilog, whose bounds nothing there
gives; and enumeration, time and physical ports other than those above.

**The interfaces across the boundary.** VPI and VHPI share the one PLI context (9.7), and each
shows its own language. VHPI shows a SystemVerilog region as one of kind `vhpiVerilog`, cocotb's
number for it, which cocotb leaves to VPI, and has no root instance under a SystemVerilog top.
VPI finds no VHDL object by name, and an iteration that meets a VHDL instance returns it as a
`vpiUnknown`, named, which cocotb then asks VHPI for by name. A name either is asked for may
begin in the other's hierarchy, the VHDL part of it in any case. cocotb requires this: for a
Verilog toplevel, its mixed-language test asserts that VPI alone does not find the VHDL instance
and that VHPI does, and a VHDL region shown through VPI would lose the types 9.7 gives it.
cocotb runs a mixed design as it does elsewhere: the toplevel's language's library is loaded
with `--vpi` or `--vhpi`, and the other's is named in `GPI_EXTRA`, which `Makefile.enzan` sets
(9.7).

**Reference simulation.** No free simulator takes SystemVerilog and VHDL together. NVC
simulates Verilog beneath VHDL, not VHDL beneath Verilog, and only its analysis is built here, so
CI runs no NVC: its own `mixed` regression tests check themselves and run under Enzan alone
(14.4), and a mixed testbench written for Enzan is run under a locally built NVC before it is
committed, where NVC can run it (14.5). Results are checked by splitting: the same design with
the boundary moved — the DUT in one language driven by a testbench in the other, against the DUT
in its own language's testbench under iverilog or GHDL — matches transcript for transcript, the
boundary's delta aside.

The exit designs need no program compiled for them, for the reason M5 gives:

- **NEORV32** in a testbench in Verilog that drives it as its VHDL one does, its reports and
  its CPU trace and UART logs against the VHDL testbench's run under GHDL (14.5).
- **An M5 SystemVerilog design in a VHDL testbench**: secworks' `aes` core, its test vectors
  driven from VHDL, against its own testbench under iverilog.
- **cocotb's mixed-language test cases**, and its VHDL cases whose designs sit in libraries
  of their own (14.6); **NVC's `mixed` regression tests**.

**Exit:** a SystemVerilog testbench drives a VHDL DUT and a VHDL testbench a SystemVerilog
one, each matching its reference; cocotb's mixed-language cases run with a recorded pass rate;
NVC's mixed tests run with one.

### M8 — Performance

LLVM JIT backend behind a flag; profiling and hot-path work; activity-based
scheduling refinements; optional static partitioning for threading; the
performance target replaced with a measured, committed figure.

**Exit:** the committed performance target is met.

### M9+ — Verification features

SVA and PSL assertions; functional coverage; constrained random (requires a
constraint solver); save and restore; multiple clock domain tooling; UVM.

**The VHDL verification libraries, VUnit and OSVVM**, and what they are built from: access
types as a general heap, with `deallocate`; protected types and shared variables of them;
generic packages and their instantiation; configurations; external names. VUnit's simulator
interface for Enzan, which VUnit chooses by name as cocotb chooses a makefile (9.6). They are
done when VUnit's own suite and OSVVM's regression each run with a recorded pass rate.

They need three things the RTL milestones do not build. Values whose length only the run knows
— a message built with `to_string`, a function returning what an access value designates, a
local sized by an unconstrained parameter — which 4.1 places in scratch the kernel resets when
a process suspends, where EIR today gives every value a width fixed at translation. Several
VHDL libraries at once, `vunit_lib` and `osvvm` beside a design's own, which M7 brings. And a
VHDL top level's generics set from the command line, as `-G` sets a SystemVerilog one's
parameters (8.1), since VUnit hands each test its `runner_cfg` as one.

---

## 19. Risk register

| Risk | Impact | Mitigation |
|---|---|---|
| NVC internal APIs drift or prove unusable as a frontend | High — blocks VHDL | Spike first in M6; pin the revision; thin adapter in one directory; documented co-simulation fallback (5.2) |
| The predefined and foreign subprograms NVC's runtime resolved are a larger body of work than estimated | High — inflates M6 | Enumerate the set during the M6 spike by walking the analysed tree, before the milestone is planned (5.2) |
| VHDL generic and subtype elaboration must be reimplemented rather than reused from NVC | Medium — inflates M6 | Sized by the M6 spike, which also decides whether generics are folded by the adapter or by elaboration (5.2); the tree boundary is reconsidered if it proves worse than the alternative (5.2) |
| A defect inherited from NVC's frontend is invisible to differential testing against NVC | High — silent wrong results | GHDL is the primary VHDL reference and both conformance suites are run (14.3, 14.4) |
| Mixed-language delta semantics differ from commercial tools | High — silent wrong results | Specify explicitly (3.5) and document it (`docs/mixed-language.md`); golden tests against references before declaring it working, the design split at the boundary since no free simulator runs both (M7) |
| Event-driven interpretation misses the performance target | Medium | Bytecode VM from the start; JIT-ready EIR; continuous benchmarking from M1 |
| Waveform memory exhaustion on large designs | Medium | Selective capture and a memory budget with disk spill (7.1) |
| Waveform rendering does not scale when zoomed out | Medium | Pixel-bounded rendering with downsampling (10.3), specified before implementation |
| Scope creep into verification features | Medium | Explicit non-goals (1.3) and milestone gating |
| Parallelism delivers less speedup than hoped | Low | Expectations calibrated in 6.4; single-threaded kernel is the plan, not a fallback |

---

## 20. Glossary

**Delta cycle** — a zero-duration iteration used to order evaluations at the
same simulated time.

**EIR** — Enzan Internal Representation. EIR-S is structural, EIR-B behavioural.

**Elaboration** — building the instance tree and state layout for a specific
top level. Not cacheable across designs.

**Analysis** — compiling one design unit to EIR, independent of any top level;
a parameterised SystemVerilog module, or a VHDL entity with generics, once per
parameterisation the design uses (2.1). Cacheable and reusable.

**Region** — one of the ordered phases within a SystemVerilog time slot.

**State arena** — the single contiguous allocation holding all mutable
simulation state.

**Time precision** — the global integer unit in which simulated time is
counted.

**VPI / VHPI / DPI** — VPI and VHPI are introspection and callback APIs used by
tools such as cocotb to drive a simulation. DPI is the separate mechanism for
calling C from SystemVerilog.
