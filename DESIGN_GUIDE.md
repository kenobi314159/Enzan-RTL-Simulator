# Enzan RTL Simulator — Design and Development Guide

**Status:** Living specification. Revised as the project evolves.
**Audience:** AI coding agents and maintainers working in this repository.
**Authority:** This document is the single normative specification for Enzan.
Human-facing documentation (user manual, tutorials, installation guide) lives
separately under `docs/` and is derived from this document, never the reverse.

---

## 0. How to use this document

Agents working in this repository MUST read sections 0, 12, 13 and 17 before
making any change, and MUST read the section covering the component they are
modifying.

The keywords **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT** and **MAY** are
used in their normal RFC 2119 sense. A **MUST** is an invariant: if you find
yourself writing code that violates one, stop and raise the conflict rather
than working around it.

**The update rule.** If a change alters behaviour that this document describes,
the corresponding section MUST be updated in the same commit. A commit that
makes this document wrong is a defect, regardless of whether the code works.

**The escalation rule.** This document deliberately leaves some decisions open
and marks them *(open)*. If your task depends on an open decision, do not
silently pick one. State the options and ask.

---

## 1. Project overview

### 1.1 What Enzan is

Enzan is an event-driven, mixed-language RTL simulator for SystemVerilog and
VHDL, with an interactive graphical debugging environment.

The name is from Japanese and carries two readings: *distant peaks* (遠山), for
the mountain-like horizon of a screen full of waveforms, and *calculation*
(演算), for the computational work the simulator performs.

The gap Enzan targets is real and currently unfilled. Questa, VCS and Xcelium
are excellent and prohibitively expensive. Verilator is fast but is a cycle-based
translator rather than a true event-driven simulator, is Verilog-only, and has
no debugging GUI. Icarus Verilog is event-driven and free but slow and
Verilog-only. GHDL and NVC are strong VHDL simulators with no SystemVerilog and
no GUI. Nothing in the open-source world offers *fast, event-driven,
mixed-language simulation behind a usable interactive GUI*. That combination is
Enzan's reason to exist.

### 1.2 Design priorities, in order

When two goals conflict, resolve the conflict in this order:

1. **Correctness of simulation semantics.** A fast simulator that computes the
   wrong answer is worthless, and semantic bugs are the hardest class of defect
   for users to detect. Language semantics are not negotiable for speed.
2. **Usability and debuggability.** The GUI, the diagnostics and the scripting
   interface are the product as much as the kernel is.
3. **Performance.** Enzan must handle real designs, but never by guessing.
4. **Language breadth.** Coverage of exotic language features comes last.

### 1.3 Explicit non-goals

The following are **out of scope** and MUST NOT be implemented without an
explicit decision to change this list. They are recorded here so that scope
does not creep into them by accident:

- Gate-level simulation, SDF back-annotation and timing checks.
- Power-aware simulation (UPF/CPF).
- Mixed-signal simulation (Verilog-AMS, VHDL-AMS).
- IP encryption (IEEE 1735).
- Windows and macOS support. Enzan targets Linux only.
- Synthesis, formal verification, equivalence checking.
- UVM, constrained-random stimulus and functional coverage in the initial
  development arc. These are deferred to M9+ (see section 18) and constrained
  random additionally requires a constraint solver, which is a project in
  itself.

### 1.4 Licensing

Enzan is licensed **GPLv3**. This is a consequence of linking NVC, which is
GPLv3, to provide the VHDL frontend.

**This is a consequence of the choice, not a reason for it.** The licence
follows from the engineering decision in 5.2 and does not justify it. The only
serious alternative VHDL frontend, GHDL, is GPL-2.0-or-later and therefore also
copyleft: it would not have bought a permissive licence, only the freedom to be
GPLv2-or-later instead of GPLv3. Any future re-examination of the VHDL frontend
must be argued on the grounds in 5.2, and the licence adjusted to follow.

The dependency stack is internally consistent under GPLv3:

| Dependency | License | Compatible |
|---|---|---|
| NVC (VHDL frontend) | GPLv3 | yes, and what sets the project's licence |
| slang (SystemVerilog frontend) | MIT | yes |
| Qt 6 | LGPLv3 | yes |
| Tcl (GUI theme and signal lists) | BSD-style | yes |
| GoogleTest | BSD-3 | yes |
| FST / libfst | permissive | yes |

Every source file MUST begin with an SPDX identifier:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
```

Third-party code vendored into `third_party/` MUST retain its original license
text and MUST be recorded in `docs/third_party.md` with its license, upstream
URL and pinned revision.

---

## 2. Architecture

### 2.1 The three phases

Enzan separates the work into three phases. Conflating them is the most common
architectural mistake in simulators, and the separation is what makes the
"reusable compiled libraries" goal coherent.

**Analysis** operates on one design unit at a time (a module, package,
interface, entity or architecture). It parses, type-checks and lowers the unit
to Enzan's internal representation, then stores the result in a library
directory. Analysis output is *independent of any particular top-level design*
and is therefore cacheable and reusable across runs and across designs. This is
the phase Questa exposes as `vlog`/`vcom`.

**A parameterised SystemVerilog module is analysed once per parameterisation the
design uses.** slang is an elaborating frontend (5.1): it type-checks a body in the
context of the values it is given, because a type can depend on a parameter, and it
offers no body generic over them. So parameter resolution and generate expansion
happen in analysis, inside slang, and a parameterised module becomes one unit per
set of values, named for them (8.2). Each unit is still independent of the top
level — the same values make the same unit wherever it is produced — but which
units exist depends on what the design instantiates, so a library is re-runnable
rather than reusable at values it has never seen: a design that needs new ones
reads the sources again.

Enzan does not rebuild that elaboration to make a unit generic. Doing so would
spare Enzan's own translation of each parameterisation and a library's sources at
the boundary, but slang would still elaborate every parameterisation of the design
being analysed. And a unit derived from one of slang's bodies would be checked
against slang only at the values slang saw, and trusted at the others; 1.2 ranks a
slower analysis above a result that is trusted rather than checked.

**VHDL generics take the same shape** (5.2). An entity and its architecture become one
unit per set of generic values, named for them, with the values folded by the VHDL
adapter rather than by elaboration.

**Elaboration** takes a named top level plus the libraries, binds each instance
to the unit for its values, resolves whatever parameters and generics analysis
left open, builds the instance tree, binds ports, computes the flat state layout,
and produces an executable image.
Elaboration is inherently per-design and **cannot** be cached across different
top levels or parameterisations. Optimisation passes — inlining, selective
flattening, process merging, partitioning — belong here.

**Execution** runs the elaborated image under the simulation kernel.

The instruction in `IDEA.md` to avoid whole-design flattening applies to
*analysis*, which MUST remain per-design-unit and modular. Flattening at
elaboration time is a legitimate optimisation and MUST remain available, because
it is where a large fraction of achievable performance lives.

### 2.2 Process separation

The simulation kernel and the GUI are **separate processes**. This MUST NOT be
collapsed into a single process for convenience.

The reasons are concrete: the kernel must be testable and scriptable with no
display attached; a stalled or slow GUI must never stall the kernel; and
attaching a local GUI to a kernel running on a remote compute server is a
feature users of commercial tools rely on daily and will expect.

```
   ┌──────────────────────┐        Unix domain socket        ┌──────────────────┐
   │   enzan (kernel)     │ <──────────────────────────────> │   front end      │
   │  headless, batch     │      versioned binary protocol   │   Qt 6 widgets   │
   │  links no Qt         │                                  │   attach/detach  │
   └──────────────────────┘                                  └──────────────────┘
```

The GUI MUST be able to attach to an already-running kernel and to detach
without terminating it.

**The user does not invoke the two separately.** `enzan --gui` starts the front
end, which is a second process and an implementation detail rather than a second
program to learn (8.4). Two processes is the architecture; two commands was never
the point.

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

**This section is normative and is the highest-risk part of the project.** Most
homebrew simulators produce plausible-looking but subtly wrong results because
their event scheduling does not match the language standards. Changes here MUST
be accompanied by tests that pin the behaviour.

### 3.1 Time model

Simulated time is a **64-bit unsigned integer** counting units of a single
global time precision. Time MUST NOT be represented in floating point anywhere
in the kernel.

The global precision is determined at elaboration as the finest precision
requested anywhere in the design — SystemVerilog `timeprecision`/`` `timescale ``
directives and the VHDL resolution limit — with a floor of 1 fs. All delays are
converted to this unit during elaboration, so the kernel never performs unit
conversion at run time.

`IDEA.md` referred to a "pre-set minimum delta step". The correct concept is
this global time precision. Delta cycles have no duration at all (see 3.2) and
are a separate mechanism.

Note the practical range: at 1 fs precision the 64-bit counter covers about 5.1
hours of simulated time; at 1 ps it covers about 213 days. Overflow MUST be
detected and reported as an error rather than wrapping silently.

### 3.2 Delta cycles

A delta cycle is a **zero-duration** iteration used to order evaluations that
occur at the same simulated time. An unbounded number of delta cycles may occur
at a single timestamp.

A delta-cycle iteration limit MUST be enforced, defaulting to 10,000 and
configurable. On exceeding it the kernel MUST abort with a diagnostic naming
the processes and signals still active, because a delta loop with no diagnostic
is one of the most frustrating failures a user can hit.

### 3.3 SystemVerilog event regions

Enzan implements the IEEE 1800 stratified event queue. The regions, in order
within a single time slot:

```
  Preponed  →  Active  ⇄  Inactive  ⇄  NBA  →  Observed  →  Reactive  ⇄  Re-Inactive  ⇄  Re-NBA  →  Postponed
```

Iteration rules: the kernel drains Active fully; if Active is empty it promotes
Inactive (`#0`) events to Active and repeats; if both are empty it promotes NBA
events to Active and repeats. Only when Active, Inactive and NBA are all empty
does it advance to Observed. The Reactive family iterates by the same rule.
Exhausting all regions ends the time slot after Postponed, and time advances.

For the initial development arc, Preponed, Active, Inactive, NBA, Observed,
Reactive and Postponed MUST be implemented. The PLI-specific regions
(Pre-Active, Pre-NBA, Post-NBA, Pre-Observed, Post-Observed, Pre-Postponed)
MAY be represented as callback hook points rather than as fully separate queues,
provided the callbacks fire at the correct point in the ordering. Section 9.3 says where each
VPI callback fires.

A delayed write is scheduled for its time, not for a process. A nonblocking `a <= #d b`
joins the NBA region of the time step `d` later. A continuous assignment with a delay
writes at the start of that step's Active region. Its delay is inertial: a newer write
from the same assignment replaces one still pending, which is taken out of the schedule,
so a pulse shorter than the delay leaves no time step behind (IEEE 1800 section 10.3.3).

### 3.4 VHDL simulation cycle

Per IEEE 1076, a VHDL simulation cycle is:

1. Current time `Tc` is set to the next time `Tn`.
2. Every active signal is updated; events occur where the new value differs.
3. Processes sensitive to those events resume, as do processes whose `wait`
   timeout has expired.
4. Each resumed process executes until it suspends.
5. `Tn` is computed as the minimum of the next driver activation time and the
   next process timeout.
6. If `Tn == Tc`, the next cycle is a delta cycle.

**A signal assignment is a transaction of its process's driver.** A VHDL process has one
driver for each signal it assigns, and each driver a projected waveform of transactions
waiting for their time (IEEE 1076 section 14.7.2). The kernel keeps that waveform per
driver, because the language defines an assignment by what it does to it (section
10.5.2.2): the driver's transactions at or after the new one's time are deleted, so of a
process's assignments to one signal in one run the last is what happens and the others
leave no event; and an inertial assignment also deletes those within its rejection limit
before it, but for a run of its own value ending there, so a pulse shorter than the
limit never reaches the signal. Treating an assignment as a SystemVerilog non-blocking
update instead gives every one of them its own event, and a design whose clock stops by
reassigning it toggles once more.

The drivers are of scalar subelements, so a transaction is of the bits it assigns: an
assignment to an element or a slice deletes what is pending for those bits and leaves the
rest of a pending transaction to happen, cut down to what it still assigns.

**A cycle begins with each batch of updates**, a delta cycle's or a time step's, and the
kernel numbers them. A signal's `'event` is whether it last changed in the cycle now
running, and its `'last_value` is what it held before that change, which the kernel copies
aside only for the signals some body reads it of: a shadow of every signal would double the
state of a design that never asks.

A transaction for the current time takes effect in the next delta cycle, after every
process of this one has run. A later one takes effect when its time step begins, before
any process resumes in it, which is step 2 coming before step 3: a process woken by a
timeout sees the values signals take at that time, not the ones they had before it.

### 3.5 Mixed-language interleaving

**This is the single hardest correctness problem in the project and it is
under-specified in every public source.** The rules below are Enzan's
definition and MUST be treated as normative; where they prove to disagree with
commercial tools on real designs, this section is what gets updated.

- There is **one** unified kernel and **one** event queue. VHDL and
  SystemVerilog are not simulated by separate engines that synchronise.
- The VHDL signal-update phase (3.4 step 2) is mapped to the beginning of the
  SystemVerilog **Active** region. VHDL process execution (3.4 steps 3–4)
  occurs within Active.
- VHDL has no non-blocking-assignment region. A VHDL delta cycle therefore
  corresponds to one full Active/Inactive/NBA iteration of the SystemVerilog
  time slot.
- A VHDL process's `wait for 0 ns` is not `#0`: it resumes in the next delta
  cycle, after that cycle's signal update, where `#0` resumes in Inactive
  before the NBA updates. The kernel holds such a process until the delta
  cycle's transactions are applied and then schedules it in Active with the
  processes those updates wake.
- A VHDL wait with a timeout and a sensitivity or a condition arms both under one
  wait generation (`WaitEventsFor`): whichever comes first resumes the process and
  makes the other stale, and an event that comes first takes the timeout back, so
  its time is not a step the run stops at. The process waits again for what is left
  of the timeout while its condition is false.
- A VHDL postponed process resumes in the **Postponed** region, so it runs
  once the time step has settled and sees the values the step ends with. At
  initialization it runs in Inactive, after the other processes and before the
  first delta cycle (IEEE 1076 section 14.7.5.2).
- A port connection crossing a language boundary is modelled as a zero-delay
  continuous assignment and **consumes one delta cycle**. This MUST be
  documented in the user manual, because it is observable and will surprise
  people.
- Value conversion at the boundary follows the mapping in 3.6.

Mixed-language behaviour MUST be covered by golden tests comparing against a
reference simulator before it is considered working (see 14.3).

### 3.6 Value representation

**The value representation MUST be four-state-shaped from the first line of
code written**, even though full four-state support is scheduled later (M5).
Retrofitting a second value plane onto a single-plane implementation is a
rewrite of the entire runtime, not an extension.

**Packed bit vectors** use two parallel bit planes, `aval` and `bval`, stored as
arrays of `uint64_t`:

| `aval` | `bval` | value |
|---|---|---|
| 0 | 0 | `0` |
| 1 | 0 | `1` |
| 0 | 1 | `Z` |
| 1 | 1 | `X` |

This is deliberately the same encoding as VPI's `s_vpi_vecval`, which makes
`vpi_get_value` and `vpi_put_value` close to free — a direct benefit given
cocotb's priority (section 9).

**Two-state fast path:** a vector whose `bval` words are all zero is
two-state. The runtime SHOULD branch to specialised two-state operations in
that case. Two-state is an optimisation of the four-state representation, never
a separate engine.

**VHDL's values are codes in the same vectors.** `std_logic` has nine values
(`U X 0 1 Z W L H -`) and does not fit two planes as a value, so it is not stored
as one: a VHDL scalar is a two-state code in EIR's packed vector type. An
enumeration, `std_ulogic` among them, is its position in as many whole bytes as
its values need — **one byte per element** for `std_ulogic`, `bit`, `boolean` and
`character`, holding the enumeration value — and an integer or a physical value
such as `time` is signed, 32 or 64 bits as its range needs. A real, VHDL's or
SystemVerilog's, is its IEEE 754 double's 64 bits in EIR's real type, held as 64 bits
are; arithmetic and comparison read them as the double, and Convert goes between it
and an integer, rounding halfway away from zero as both languages round. Records and
constrained arrays are flat, as SystemVerilog's aggregates are (4.3), so a
`std_logic_vector(7 downto 0)` is a 64-bit vector of eight codes.

So everything built on bit ranges serves VHDL unchanged: drivers per range and
resolved segments (3.7), part stores, change detection, waveform capture and
VPI. What VHDL adds is what a code means, which the type carries — for display,
for conversion at language boundaries and for range checks. The second plane of
such a vector is always zero, so a `std_logic` costs two bytes of arena rather
than one until two-state values are packed into one plane, an optimisation
`Type::storageWords` already anticipates.

Conversion to the four-state form happens at language boundaries, in the
waveform database and in the PLI, with `L`→`0`, `H`→`1`, `W`/`U`/`-`→`X`. An EIR
object carries what its bits mean as its encoding, `std_ulogic` codes, `bit` or `boolean`
codes, or the value itself, so that what converts needs nothing of VHDL but that: a
`std_logic_vector(7 downto 0)` shows in a waveform as eight four-state bits.

A new kind of storage for VHDL was rejected because each of the mechanisms above
would have needed a second path kept in step with the first, which is the
second engine this section exists to forbid.

Integers, reals, strings and aggregate types use native representations
described in the EIR type system (section 4).

### 3.7 Drivers and resolution

**A driver owns a bit range of a signal, not the whole signal.** Each process that
assigns to a signal owns a driver covering exactly the bits it assigns. A bit with
one driver takes that driver's value directly; a bit with more than one computes its
value with a resolution function.

Granularity is the whole of what makes this section non-trivial, and getting it
wrong rejects legal designs. The pattern that forces it is the ordinary one:

```systemverilog
logic [N-1:0] ready;
for (genvar i = 0; i < N; i++)
    channel u (.ready(ready[i]));
```

Every instance drives one bit and no other. Were a driver to own the whole signal,
these would be `N` drivers of `ready` and resolution would have to invent what each
contributes to the bits it never touches — so a parameterised number of instances
connected to an array could not be expressed at all.

**Overlap is what makes a signal multiply driven, not sharing a signal.** Two
drivers of one signal whose ranges are disjoint are not in conflict and MUST NOT be
treated as though they were. This distinction is load-bearing in SystemVerilog
rather than a convenience:

- A **variable** (`logic`, `bit`, `reg`) may be continuously driven by at most one
  driver *per bit*. Disjoint part-drivers of one variable are legal; overlapping
  ones are not, and are an error rather than something to resolve.
- A **net** (`wire`, `wand`, `wor`, `tri`, `triand`, `trior`, `tri0`, `tri1`,
  `supply0`, `supply1`) may have any number of overlapping drivers, resolved per bit
  by the built-in table for its kind.
- A bit of a net with no driver at all is its default — `z` for `wire` and `tri`,
  and the pull value for `tri0`, `tri1`, `supply0` and `supply1` — and not the value
  a neighbouring bit happened to resolve to. A bit that has a driver is X until that
  driver first runs, as a variable is: before then it holds no value the design gave
  it, and `z` would claim that nothing drives it.

Further requirements:

- VHDL resolution functions are arbitrary user code. They are compiled to EIR like
  any other subprogram and invoked by the runtime. The runtime MUST NOT assume
  resolution is a built-in table.
- **Elaboration asks for a resolution function by its driver count.** Its one parameter
  is an array as long as the bit has drivers, which only elaboration knows, while the
  adapter specialises a subprogram for a static shape (5.2). So an EIR object names
  its function as the design does, by library, package and signature, with the width of
  the subelement it resolves, and once elaboration has counted the drivers the session
  asks the frontend, in a second child over the same sources, for the function
  specialised to each count, links what comes back into the design, and elaborates
  again with it, which binds each segment to its function. The kernel calls it for each
  subelement with its drivers' values, the first driver's left; a VHDL driver starts at
  the initial value of the signal its process names, which for an out port is the port's
  default rather than that of the signal connected to it, and is what the function sees
  of it until it first runs. Before the run begins the kernel resolves each such signal
  from its drivers' starting values, which is where the signal starts.
- **Single-driver bits MUST bypass resolution entirely** where resolving a lone
  driver is known to return its value unchanged, and a signal all of whose bits
  are such MUST bypass it as a whole rather than bit by bit. This is the common
  case and it is on the hot path (13.4). It is known of the built-in tables of
  every net kind but the pulled and supply ones, and of a VHDL resolution
  function proven to be the identity on a lone driver by calling it once with each
  value of its element type, where that is an enumeration of sixteen values or fewer,
  as NVC does; `ieee.std_logic_1164.resolved` is one. The adapter proves it as it
  translates, running the function on each as a constant function (5.2), and an EIR
  object's resolution records it.
- **Any other resolution function is called for a lone driver too.** VHDL defines a
  resolved signal's value as its resolution function applied to its sources,
  however many there are (IEEE 1076 section 14.7.3.2), and a user's function need
  not return a lone driver's value. Bypassing it would be a different language.
- A driver's range is fixed at elaboration. A part-select whose bounds are not
  constant does not create a driver over the bits it might touch; it is a write
  through the driver that owns them, and a design that drives one signal from two
  processes through variable selects is multiply driven for every bit either could
  reach.

**A driver is a range and nothing more until two of them overlap.** It is the signal,
the lowest bit it drives and its width. A process's write set records the same thing
— each entry is an object and the bits written, by the rule above for a variable
select — so the drivers of a signal are known before anything runs, and elaboration
finds where they overlap. The runtime keeps no record of which driver owns which bit, because nothing
about ownership changes once elaboration is done.

- **A signal none of whose bits has two drivers carries no driver state at all**,
  unless its resolution must still be applied to a lone driver, as above. Every
  legal variable is one, and so is most of every net. A write goes straight into
  the signal, which is how such a signal bypasses resolution as a whole.
- **A signal with a bit driven more than once is split into segments**: maximal runs
  of bits driven by the same set of drivers. Each driver of such a signal has its own
  value in the arena, in both planes, over its range; each segment lists its drivers
  and the resolution it applies — the built-in table for the net's kind, or a call to
  a compiled VHDL resolution function. A write through one of these drivers updates
  the driver's value, resolves the segments that driver covers and no others, and
  writes the result into the signal through the ordinary change-detected path (6.2),
  so a contended bit wakes nothing unless its resolved value moves.
- **Whether a store goes through a driver depends on the instance, not the body.** A
  compiled body is shared by every instance of its module, and one instance's output
  may land on a resolved net where another's does not. A store therefore names its
  target by the position of its object among the objects the process writes, and each
  bound process carries a table from that position to the signal itself or to its
  drivers of it — more than one when it writes separate ranges of the object, in which
  case a part store goes through the one holding its bits. A JIT, which specialises a
  process per instance, burns the answer in as a constant.

A per-bit table of owning drivers was rejected because it holds at run time a fact
that is fixed at elaboration, and pays a lookup on every write, single-driver ones
included. Routing every write through a driver whether or not anything overlaps it
puts a step on the path this section requires to be bypassed.

### 3.8 Determinism

**Simulation results MUST be bit-identical across runs of the same build on the
same inputs.** Determinism is not a nicety: without it, waveform diffing and
regression triage are impossible, and intermittent failures cannot be
reproduced.

Concrete invariants:

- Process execution order within a region MUST be a stable insertion order
  (FIFO). It MUST NOT depend on pointer values, allocation addresses, or the
  iteration order of hash-based containers.
- Code MUST NOT iterate `std::unordered_map`/`std::unordered_set` in any way
  that affects simulation results, diagnostic ordering, or generated output.
  Use ordered containers or sort explicitly where order is observable.
- All randomisation MUST draw from an explicit seeded generator. The seed
  MUST be settable from the command line.
- The kernel is single-threaded in the initial arc, which makes determinism
  achievable. Any later parallelism MUST preserve it.

### 3.9 Initialisation and X-propagation

At time zero, four-state variables and signals start at `X` (SystemVerilog) or
`U` (VHDL `std_logic`) unless explicitly initialised. X-propagation follows the
language standards: pessimistic in expressions, with the standard-defined
exceptions for `case`/`casex`/`casez` and conditional operators.

A two-state simulation mode, where `X` is replaced by a deterministic `0`, MAY
be offered as an explicit user-selected mode for performance. It MUST NOT be
the default and MUST be announced in the transcript when active, because silent
X-suppression hides real design bugs.

---

## 4. The Enzan Internal Representation (EIR)

EIR is Enzan's own language-neutral representation. It is the contract between
the frontends and everything downstream.

**hdlConvertor was considered as a shared representation and rejected.** It is
an ANTLR-based *parser*: it produces a syntax tree with no elaboration, no type
resolution, no parameter or generic evaluation and no generate expansion.
Elaboration is the difficult part of a frontend, not parsing. EIR is built
in-house instead.

### 4.1 Layers

**EIR-S (structural)** describes design units and hierarchy: modules and
entities, ports and their directions and types, parameters and generics,
instances, signals and nets, processes, continuous assignments, subprograms.
Language-neutral.

**EIR-B (behavioural)** describes process and subprogram bodies as a
control-flow graph of three-address instructions over typed values. Operations
cover arithmetic, logical and bitwise ops, part- and bit-selects, concatenation,
comparison, branching, calls, signal reads, blocking and non-blocking drives,
and the suspension primitives (`wait` on event, on time, on condition).

**A subprogram is a body that is called rather than scheduled.** It is the same
control-flow graph a process is, over registers of its own, with parameters, a
result, and the read and write sets of what it reaches beyond its parameters
(4.2). It belongs to the definition that declares it — a package's to the
package, an architecture's to the architecture — and a call names it by its
definition and its name, resolved when the design is compiled as an instance is
resolved at elaboration. Nothing in it is particular to VHDL: a SystemVerilog
function is the same thing, and the frontend that inlines one today can call it
instead without anything downstream changing.

- **A subprogram is specialised by the lengths of its unconstrained
  parameters**, as a unit is by its generics (5.2), so that every width in EIR
  stays fixed: `numeric_std."+"` called on 8-bit and on 32-bit operands is two
  bodies, each named for its lengths.
- **A call's frame lives on one stack the kernel owns.** A subprogram never
  suspends — a VHDL procedure that waits, like a SystemVerilog task with a delay,
  is inlined into its caller, and so is one with a signal parameter, whose body
  needs to know which signal it is — so every frame a process pushes is popped
  before the process suspends, and one stack serves every process of a
  single-threaded kernel (6.4). It is laid out at elaboration, sized for the
  deepest chain of calls the design can make, and a recursive design is given a
  depth limit, configurable, past which the run stops with a diagnostic naming the
  subprogram, as the delta limit does (3.2).
- **A caller's sets include its callees'.** A process declares what it touches
  itself; elaboration adds what its callees touch, bound to the instances they
  touch it in, so the bound process's sets are exactly what a run of it can reach
  (4.2).
- **A value whose length is known only when it is computed** — `integer'image`,
  `to_string`, a concatenation of either — lives in scratch the kernel resets
  whenever a process suspends. It is a temporary of one activation, which is all
  a VHDL expression of unconstrained type is; anything that has to outlive it is
  stored in an object of fixed size or in the heap of 4.3.

### 4.2 The core invariant

**Every EIR process MUST be expressible as a pure function over an explicit
state arena, with a declared read set and write set.**

This is the single most important structural rule in the project. It is what
keeps later optimisation open: static partitioning for threading, JIT
compilation, activity-based scheduling and checkpointing all depend on knowing
exactly what a process touches. Code that lets a process reach arbitrary global
state closes those doors permanently and MUST be rejected in review.

### 4.3 State layout

The elaborated design's entire mutable state lives in a **single contiguous
arena**. Every signal, variable and driver is assigned a byte offset at
elaboration. EIR and bytecode refer to state by offset, never by pointer.

The consequences are all deliberate: checkpoint and restore become a memcpy of
the arena; the waveform database reads values directly out of it without
indirection; a JIT can burn offsets into generated code as constants; and cache
locality becomes something that can be optimised by reordering the layout.

**A fixed-size unpacked array or an unpacked struct is stored flat**, as the vector of all
its bits, the left element and the first field highest, exactly as the packed equivalent
would be. An element, a field or a select of one is a window of those bits at an offset, so
every rule about bits — drivers per bit range (3.7), part stores, change detection — applies
to it unchanged, and an element is read in place rather than by copying the array. What the
flat vector loses, the declared dimensions, is kept beside it for VPI and the waveform
database, which present an array as an array. A memory is not recorded in a waveform, as a VCD
leaves memories out.

A driver occupies the arena only where 3.7 gives it a value of its own: on a signal
with a bit driven more than once, each of its drivers holds both planes over its
range, laid out at elaboration beside the signal. A signal with at most one driver per
bit has nothing here but itself. The segment tables and the store routing of 3.7 are
fixed at elaboration and only read during a run, so they belong to the image rather
than to the arena, and a checkpoint has nothing of theirs to save. Nothing about
drivers is sized at run time, which on the write path 13.4 forbids.

**An access value is a handle.** What VHDL's `new` allocates — a textio `line`, a
VUnit queue — has a number and a size known only as the design runs, which is
what the arena cannot hold. It lives in a heap the kernel owns, beside the arena,
and an access value in the arena is a handle into that heap: an integer, numbered
in the order the allocations happen, so that a run allocates the same handles
every time (3.8) and change detection, waveforms and VPI see an integer like any
other. Allocating and deallocating are calls into the runtime from a process,
which RTL's write path never makes; textio is where they happen. The heap is
state a process reaches, so a process that dereferences a handle has the heap in
its read or write set (4.2), and a checkpoint (M9) saves it with the arena. A
freed handle is reused, the last freed first, which keeps a run's handles the same
every time without the heap growing with every line written.

**std.textio runs natively.** NVC's textio body grows a line through accesses to strings
of run-time lengths, which registers of fixed width cannot hold, so it is not translated:
`write`, `writeline`, `readline`, `read` and `endfile` are system functions the runtime
runs over its heap and its files, as NVC's runtime runs its foreign subprograms, and the
adapter calls them for textio's subprograms and VHDL's predefined file operations. A file
of a type other than text holds its values' bits in whole bytes, the lowest first: Enzan's
own format, as each simulator has its own, which a run reads back as it wrote it. A value
of an unconstrained array type goes with its count of elements, so that `read(f, v, n)`
gives `v` as many of its leftmost elements as fit, leaves the rest of `v` as it was, and
gives `n` how many it read, as GHDL does. A
string whose length only the run shows, which a function builds from images and writes,
is built in a line of its own: the function is inlined with its `return`s appending
their pieces to that line, and the line is appended to the one written.

### 4.4 Libraries

Analysis output is serialised into a library directory, named by the user
(`work/` is the conventional choice, not a default — see 8.2). Entries carry a
key over the source text plus the analysis options that affect the result, so
unchanged units are never re-analysed.

**A library has a logical name as well as a path**, because that is what a design
refers to: `library ieee;` resolves against a name. The name is fixed when the
library is created and stored in the library's own index, so that the two ends of a
build cannot disagree about it; 8.2 covers how it is set and overridden.

**An entry's file name comes from the unit it holds, and its key lives inside
it.** The two are separate on purpose. A path that were a hash of the source could
not be named by a Makefile, and a build that cannot predict its own outputs cannot
be incremental; a key that were only the path could not tell a stale entry from a
current one. So the name is what `make` depends on and the key is what Enzan
checks.

**Everything that changes what analysis produces belongs in the key**, not only
the source text: the macro definitions, the include path, the time unit, the
serialised format version and Enzan's own version. A define that is outside the key is a define
whose change the library cannot see, and the result is a simulation that runs
and is wrong.

The serialised form MUST carry a format version, and a version mismatch MUST
cause clean re-analysis rather than a crash or, worse, silent misbehaviour. The
same applies to a truncated or corrupted entry, which a checksum detects: an
interrupted write leaves a file that would otherwise decode into plausible
nonsense.

**The format version MUST be bumped when the frontend's translation changes**,
not only when the encoding does. A unit produced by an older translator decodes
perfectly and simulates differently, which is the harder failure to notice of
the two.

**Using a library is opt-in, and there is no implicit one.** Nothing is written
and nothing is consulted until the user names a library (8.2), so a simulation runs
from a one-time analysis by default. The version is in the key and the format
version is disciplined, but a cache that can change a simulation result is a thing
a user must be able to stop doing without having to find it first — and a cache
that appeared without being asked for is one they would have to find first.

**VHDL's standard libraries are part of the tool, not a cache.** `std`, `ieee` and
NVC's own `nvc`, which NVC's 2008 `ieee` bodies use, are analysed from the sources
NVC ships when Enzan is built, and are found beside the executable without being
named. Nothing a user does writes to them, they change only when Enzan does, and
`library ieee;` has to resolve with no flag in every VHDL tool, so none of the
reasons above applies; they are to a VHDL design what a compiler's standard headers
are to a C one. A library the user names `ieee` replaces them (8.2).

**A VHDL entry also holds NVC's analysed units.** VHDL is analysed one unit at a time
against units already analysed — `use work.pkg.all`, an entity bound to a component —
so a library holding only EIR could not be analysed against. A VHDL source's entry
therefore keeps NVC's serialised units beside the EIR, and the NVC revision and the
VHDL standard are in its key with everything else that changes what analysis
produces.

A hit is currently all or nothing, and that is a property of the frontend rather
than of the store. SystemVerilog analysis hands every source to one slang
compilation, because a module cannot be type-checked without the packages it
imports; so either every file's entry is present and current, in which case
nothing is analysed at all, or one is missing and everything is analysed and
every entry rewritten. Entries are nonetheless kept **per source file**, because
that is the shape this section describes and the shape a per-unit frontend will
want. Only the hit rule changes when one arrives.

---

## 5. Frontends

A frontend's only job is to turn source text into EIR. Frontends MUST NOT
flatten hierarchy, make scheduling decisions, or allocate runtime state.

### 5.1 SystemVerilog via slang

[slang](https://sv-lang.com/) is MIT-licensed and explicitly designed to serve
as a frontend for simulators. It is the most standards-compliant open-source
SystemVerilog frontend available and carries no licensing constraints. It is
used for lexing, preprocessing, parsing, type checking and elaboration.

slang provides an elaborated AST with parameters resolved and generate blocks
expanded. `src/frontend/sv/` walks that AST and emits EIR. slang performs no
simulation, so everything from EIR onwards is Enzan's own work.

**A generate block is part of the name of everything inside it**, objects and instances
alike, joined with a dot: `/tb/gen[0].q` and `/tb/gen[0].u_dut/count`. A block is named as
IEEE 1800 section 27.6 names it — its label, `genblk<n>` when it has none, and for one
iteration of a loop the loop's label and the iteration's index. Without the block, two
iterations instantiating `u_dut` would be two instances with one path, and nothing in a
waveform, the front end or VPI could tell them apart. Changing how a block is named changes
the EIR the frontend produces, so it bumps the serialisation version of 4.4.

**A hierarchical reference is an object with a path and no storage.** A testbench reading
`dut.core.state` gets an object whose path leads from the referencing instance to the one
that declares `state`, and elaboration binds it to that object's slot once the whole tree
exists, so reads, writes, sensitivity and drivers all see the real signal. The path is
relative — down as `core/state`, up and across as `../u_peer/ready` — because one
definition serves every instance of a parameterisation (M3.1), and a path from the root
would be right for only one of them. A path that names nothing is an elaboration error.

**A package variable is one variable however many modules use it** (IEEE 1800 section 26).
Its package's variables are a definition of their own, named `<package>::` so that no
module can share the name, and with one instance at the root beside the top level,
`/<package>`, created only when something refers to one. A module's reference to one is a
reference with the only absolute path there is, `/<package>/<variable>`, since a package has
no position in the hierarchy for a relative path to start from. Only the variables a module
refers to are in the definition. A package's constants, types, functions and tasks need no
instance: slang resolves the first two, and a call is inlined like any other. A package is
never a top level, and VPI's root does not list it as a module.

**An interface instance is an instance**, its body a definition like a module's and its signals
stored under its own path, `/tb/bus/data`. A member a module reaches through an interface port
is a reference through that port, `@in/data`, and each instance records what its interface
ports are connected to — an interface instance of its parent, or the parent's own port, which
is how an interface is passed down. An element of an array of instances or interfaces is an
instance named with its index, `u[2]`, and a port that takes an array reaches its elements as
`@in[2]/data`. Elaboration follows those bindings, so two instances of
one definition on two interfaces each reach their own. A modport restricts what a module may
touch, which slang checks, and changes nothing about where a signal is.

**A function or task is inlined into the process that calls it.** The body is translated in
place, its variables bound to registers, so a task's delays suspend the caller as its own
statements would, the caller's read and write sets are exactly what the call touches (4.2),
and nothing at run time keeps a call stack. A recursive call is refused, since an inlined
body cannot contain itself; slang folds a recursive call with constant arguments before
Enzan sees it, which is where recursion in RTL usually is. EIR has callable subprograms
(4.1), which the VHDL frontend uses; calling a SystemVerilog function rather than inlining it
is a change to this frontend alone, and a task with a delay stays inlined either way.

slang requires **C++20**, which sets the project's language floor.

### 5.2 VHDL via NVC

[NVC](https://github.com/nickg/nvc) provides the VHDL frontend. It supports
almost all of VHDL-2008 and is actively maintained.

**Why NVC and not GHDL.** [GHDL](https://github.com/ghdl/ghdl) is the other
credible open-source VHDL frontend, and on one axis it is the better candidate:
it deliberately exposes its analysis frontend as a shared library, `libghdl`,
which already has third-party consumers — `pyGHDL` and the language server,
`pyVHDLModel`, and `ghdl-yosys-plugin`, a C++ tool that walks GHDL's analysed
VHDL and emits an unrelated IR. That last one is almost exactly the job
`src/frontend/vhdl/` has to do, with years of evidence that it is possible. The
API-drift risk recorded in section 19 is genuinely lower for GHDL, and this
section should not pretend otherwise.

NVC wins on everything else, and the balance is not close:

- **GHDL is written in Ada.** Using it puts GNAT in the dependency set for CI
  and for every contributor, and links the Ada runtime into a C++20 binary —
  elaboration ordering, a secondary stack, and exceptions at the boundary. It
  also degrades the sanitiser configuration of 11.4, which would no longer see
  into the frontend. NVC is C11: one toolchain, one runtime, `extern "C"`.
- **Only NVC can be consumed in part.** Its analysis stage pulls in no LLVM, and
  with NVC's constant evaluator replaced (see the integration plan below) it is 34
  of NVC's C files, about 66,000 of its 172,000 lines. GHDL has no equivalent
  subset; `libghdl` is built whole, with GNAT, or not at all.
- **`tree_t` is a better tree to walk.** It is arena-allocated behind uniform
  accessors that assert node kinds at runtime with usable messages. GHDL's `Iir`
  is an untyped integer handle over several hundred node kinds with heavy field
  aliasing, and its generated bindings are maintained for Python, not C++.
- **Coverage and momentum.** NVC covers nearly all of VHDL-2008 except PSL and
  has VHDL-2019 work landing; GHDL targets VHDL-2008. NVC releases steadily and
  its maintainer is responsive on frontend-level defects, which matters when the
  dependency is on internals nobody has promised to keep.

The VHDL designs Enzan is measured against — NEORV32 in M6, VUnit and OSVVM in M6.1 —
are all first-class NVC targets.

**Understand the integration honestly before working on it.** NVC is not a
frontend library; it is a complete simulator written in C that lowers VHDL
through its own `vcode` IR to LLVM and runs it on its own scheduler with its own
VHPI runtime. There is no stable public API for "give me the elaborated VHDL".

The integration plan is therefore:

- Vendor NVC as a git submodule pinned to a specific revision.
- Use its lexer, parser and semantic analysis, consuming the resulting `tree_t`
  after its simplification and bounds-check passes.
- **Do not** use its `lower`/`vcode`/LLVM path, its runtime, or its scheduler.
- **Replace its constant evaluator.** NVC's simplification folds locally static
  expressions by lowering them and running them on its JIT interpreter, so
  analysis as NVC builds it links 80 of its 102 objects, the path the previous
  point rules out among them. Enzan does not compile `eval.c` and supplies the
  two functions the simplifier calls instead, so a tree arrives with those
  expressions unfolded and the adapter folds them as it translates. On NEORV32
  that is 159 expressions, 135 of them implicit type conversions, and the
  analysis is otherwise unchanged.
- `src/frontend/vhdl/` walks the analysed tree and emits EIR, exactly as the
  SystemVerilog adapter does.

NVC builds with autotools, which does not compose with the build of 11.1. Since
the plan uses only the analysis stage, the spike SHOULD establish that Enzan can
compile that file subset directly with its own CMake target, and record the file
list in `docs/nvc_integration.md`; driving NVC's `configure` from
`ExternalProject` is the fallback if it cannot.

**Two costs are larger than they look, and neither is an API problem.**

*Predefined and foreign subprograms.* NVC implements much of `std.standard`,
`std_logic_1164`, `numeric_std`, `std.textio` and `std.env` as builtins,
intrinsics or foreign functions that its **runtime** resolves. Discarding that
runtime means Enzan MUST supply its own implementation of everything the
analysed tree leaves predefined or foreign, and the set is discovered only by
walking it. This is more certain than any drift risk and is not avoided by
choosing a different frontend — GHDL is built the same way — so it is scope for
M6 rather than an argument about 5.2.

*Semantics that settle only at elaboration.* Consuming trees after
simplification and bounds checking is the right boundary: it is what section 5
means by a frontend that does not flatten hierarchy, and Enzan elaborates for
itself. The consequence is that generic folding, subtype resolution and
unconstrained arrays sized by a generic are Enzan's work, not NVC's, and Enzan
reimplements them rather than reusing NVC's `elab`. The spike sized this
(`docs/nvc_integration.md`), and the adapter does it, as the next paragraph decides.

**The adapter folds generics.** That was the spike's to decide, since 2.1 had left it
open for VHDL. Unlike slang, NVC hands over a tree with the expressions over
generics still in it, so the adapter can either fold them as it translates each
instance — one unit per set of values, as SystemVerilog has — or emit units that
carry those expressions for elaboration to evaluate per instance. The second gives VHDL the
reusable library of 2.1, at the cost of EIR holding expressions over a unit's
generics, an elaborator that specialises units by them, and constant functions
run at elaboration; the first needs neither of the first two. Subtypes, generate statements
and constant functions that depend on a generic are what the spike sizes the two
against.

Measured on NEORV32 (`docs/nvc_integration.md`): 60 of its 77 entities have
generics, 610 in all; outside processes and subprograms its architectures refer to
them 802 times, hold 209 generate statements, and call user functions 61 times,
such as `index_size_f(IMEM_SIZE)` in a subtype. Since NVC's evaluator is replaced,
**both choices run those functions with the generics' values** — the first in the
adapter, the second at elaboration — so constant functions are not what tells them
apart.

The first was chosen. The adapter translates an entity and its architecture once per
set of generic values a design gives them, into a unit named for the values as a
SystemVerilog specialisation is (8.2): subtypes sized, generate statements expanded and
constant functions run as it goes, starting from the units a design names as its top
and following its instances down. EIR holds no expression over a generic, and
elaboration specialises nothing. The second would have made generate statements
conditional structure in EIR and every width an expression, the largest change to
EIR, elaboration and VPI that anything in M6 would make, to buy libraries reusable at
values no design has asked for; what a VHDL library keeps for those values instead is
NVC's analysed units (4.4), from which a design that needs them translates them
without parsing anything again.

Known risks, to be confirmed by a spike at the start of M6: NVC's internal APIs
are not stability-guaranteed and will drift; NVC is C and uses its own arena
allocators and global state, so reentrancy and ownership need checking; and the
C/C++20 boundary needs careful `extern "C"` wrapping. The spike found one
analysis context per process, no threads started by analysis, and fatal errors
that end the process with status 1 rather than reaching Enzan's diagnostics; its
findings are in `docs/nvc_integration.md`.

**So NVC runs in a child process**, forked for each analysis. A fatal error, which
a design naming a library nobody built is enough to cause, then ends the child and
is reported as a diagnostic, and every analysis starts from NVC's initial state
rather than the previous one's. The child reports each of NVC's diagnostics back as
NVC gives it, so what the user sees is what one process would have shown.

**The child translates as well.** A design to simulate is analysed and translated in
one child, which sends the parent the result in EIR's own encoding (4.4), so NVC's
trees, and NVC's memory, never reach the process that runs the simulation. What the
child translates is the hierarchy under the top, or under every entity of the sources
that none of them instantiates when no top is named: a unit nothing instantiates cannot
stop a design for using something the adapter lacks.

What a VHDL unit becomes in EIR:

- An entity's ports and its architecture's signals are objects holding codes (3.6),
  each starting at its declared value or its type's left bound, as VHDL's objects do.
- A process is an EIR process that loops for ever, its declarations initialised on
  entry; its variables are registers, which keep their values while it waits. It is
  marked as **driving** the signals it assigns, so that elaboration counts each as a
  driver as it counts a continuous assignment (3.7), and two processes assigning one
  unresolved signal are refused. A process that cannot suspend is refused too, since it
  would never let time advance.
- A signal assignment is `Drive`, or `DriveTransport` for a transport assignment and
  for each element of a waveform after the first: the transactions of 3.4. One to an
  element or a slice is `DrivePart`, at the offset of its bits, or after a delay
  `DrivePartAfter` and `DriveTransportPart`, whose one operand holds the offset and the
  delay together.
- An array is its elements end to end, the left element highest (3.6), with bounds the
  adapter has folded; a record is its fields end to end, the first highest. A field is
  at an offset its type fixes, so a field of a signal is a part of it like a slice. An
  element or a slice of a variable is spliced into its register;
  an index known only at run time is checked against the bounds, and one outside them
  ends the run with a failure, as VHDL's index check does. A slice is a part of a fixed
  width, so one whose bounds move with a loop's parameter, `d(4*i+3 downto 4*i)`, unrolls
  that loop, each copy with its value bound. A variable's slice whose length is known only
  at run time, as numeric_std's shifts by a count assign, is its value shifted into place
  and merged under a mask of the slice's bits.
- A function is a callable subprogram (4.1), translated once for each shape it is called
  with: the bounds of its unconstrained parameters, and the values of the scalar ones its
  own bounds are computed from. Inside a shape those bounds are static, so `l'length`
  folds and a branch on a condition that folds is translated alone, as a generate
  statement is: a guard for a null array that cannot be one leaves nothing behind. A
  package's subprograms are a definition `<package>::` of their own, which a call needs no
  instance of. A procedure, a subprogram taking a signal, and one a process declares are
  inlined into their caller, since a body of its own could not wait, assign a signal, read
  a signal's attributes or reach the process's variables.
- A generate statement is flattened into the architecture it is in, as SystemVerilog's
  are (5.1): the alternative whose condition folds true, or each iteration of a loop with
  its parameter bound, and what each declares is named inside it, `g(0).u`. A block is the
  same, named by its label: its generics are bound by its generic map, as an instance's
  are, and fold where it is; each of its ports is the signal associated with it as a
  whole, or else a signal of its own, `b.p`, connected to its actuals as an instance's
  port is, and one left unassociated keeps its default.
- A component instance is an instance of the entity the component is bound to by
  default: the one of its name in the library of the unit instantiating it, or else in
  the work library. The entity's generics take the values of the component's of the
  same names, or their own defaults, and its ports match the component's by name. A
  configuration specification, `for u : c use entity work.e(a);`, names the entity and
  the architecture instead, and its binding maps the entity's generics and ports to
  the component's. A configuration declaration is not translated yet.
- A port associated with a whole signal is that signal's storage. One associated with
  part of a signal, with an expression, or in parts is connected through an object of
  the instantiating definition, `u$port`, which a process for each association keeps
  equal to its actual: into it for an input, out of it for an output. Where the
  association is a name or a conversion of one, the process is the connection itself
  (`eir::Process::connectsPort`): the kernel runs it as part of the signal update,
  applying what it drives in the same delta cycle as what it copies, and once at
  initialization, so neither side lags the other, as a collapsed port does not. An
  expression actual is a signal of its own, assigned a delta later (IEEE 1076 section
  6.5.6.3), and its process is an ordinary one.
- A constant function is run as the adapter folds: a call of a pure function of the
  design or of the IEEE packages whose arguments fold runs its body's variables, scalar
  or composite, `if`, `case`, loops and `return` over their values, with an
  architecture's generics in view, as `index_size_f(MEM_SIZE/4)` sizing a memory needs.
  A signal's initial value is computed once at elaboration, so an impure function folds
  there too when what it runs is static, as a memory that reads a file only when given
  its name does when given none; elsewhere an impure function never folds, `now` least
  of all. What it returns is folded where it
  returned, a scalar or an array, so a function choosing between two vectors folds too.
  A call that does anything else, or runs a million statements, does not fold, and its
  use is refused where a static value is needed. Elements and slices of static arrays
  fold as well, so a generate statement can be conditioned on a constant list, and so do
  strings built from them and from the `'image` of static scalars: a string generic
  naming a log file, or a report announcing a configuration, which is its text then.
- A null array, a generic left at `""` most often, has no bits to hold, so it is its bounds:
  a comparison or an attribute of it folds, and a call given one is inlined, where its
  uses can fold too. A testbench that reads a file only when a name is given needs no
  files when none is.
- The IEEE packages are translated from their own bodies like any other VHDL. What NVC's
  versions of them take from its runtime instead, Enzan supplies: numeric_std's warnings
  are on, as they are in GHDL and in the IEEE's own packages, unless `--ieee-asserts
  disable` turns them off as GHDL's option of the name does.
- `report` and a failed `assert` are one system task taking the severity as an
  argument: note, warning and error are reported and the run goes on, and failure ends
  it with a failing status, as GHDL's default does; `--assert-level` names another
  severity to end it at, as GHDL's option of the name does. `wait;` waits on an empty event list,
  which nothing ever wakes.
- Time is a count of femtoseconds, VHDL's resolution limit, and a design with VHDL in it
  is simulated in femtoseconds unless `--timescale` names another unit (3.1).

Mitigations: pin the revision, keep the adapter thin and confined to one
directory, document the rebase procedure in `docs/nvc_integration.md`, and rely
on golden tests to catch drift on upgrade.

**Documented fallback.** If the adapter proves untenable, the alternative is
co-simulation with NVC as a separate process over VHPI. This is slower and makes
the mixed-language delta semantics of 3.5 considerably harder, so it is a
fallback and not a plan.

### 5.3 Adding a frontend

A new language frontend consists of exactly one directory under
`src/frontend/`, exposing a single entry point that takes source files plus
options and returns EIR design units or diagnostics. If a frontend needs
anything else from the rest of Enzan, that is a design smell and should be
raised rather than worked around.

---

## 6. Execution engine

### 6.1 Bytecode virtual machine

EIR-B is compiled to a **register-based bytecode**. Register-based, not
stack-based: it produces fewer instructions per operation and maps far more
directly onto both real registers and LLVM IR.

Requirements:

- Each process has its own register file. Wide vectors that do not fit in a
  register spill to a per-process scratch area.
- A call pushes the callee's register file onto the kernel's one call stack (4.1)
  and a return pops it. The lowering is an LLVM function per subprogram body and
  a `call` to it, which is why a subprogram never suspends: an LLVM function
  cannot return to the scheduler half-way and be resumed.
- The dispatch loop SHOULD use computed goto (threaded dispatch) rather than a
  `switch`.
- **Every opcode MUST have an obvious, documented lowering to LLVM IR.** The JIT
  backend (6.3) is intended to be a mechanical translation of the interpreter,
  and an opcode that cannot be lowered mechanically blocks that. Adding an
  opcode without recording its lowering sketch is incomplete work.
- Opcodes address state by arena offset (4.3).

### 6.2 Scheduler

The scheduler implements section 3. Structure:

- A time wheel for future events, with a hierarchical or calendar-queue
  structure sized for the common case of many events at few distinct times.
- Per-region FIFO queues within the current time slot.
- Signal update is **activity-based**: a process is scheduled only when a signal
  in its sensitivity list actually changes value, not merely when it is
  assigned. Value-change filtering before scheduling is one of the largest
  single performance wins available and MUST be present from the start.

### 6.3 JIT (deferred to M8)

An LLVM ORC-based JIT backend is the intended path to the performance target.
It is deliberately deferred: the interpreter must exist first, both to validate
semantics and to serve as the reference the JIT is differentially tested
against. The JIT is a backend behind a flag, never a replacement for the
interpreter.

### 6.4 Parallelism — realistic expectations

`IDEA.md` aims for parallel execution. This needs calibrating, because
thread-parallel event-driven RTL simulation has a thirty-year research history
of disappointing results: synchronisation cost typically exceeds the work
performed per event.

What actually pays, in order of confidence:

1. **Parallel analysis and elaboration.** Design units analyse independently.
   This is a straightforward, real win and SHOULD be done early.
2. **Static partitioning of the synchronous subset**, in the manner of
   Verilator's threaded mode. Realistically 2–4× on large designs. Viable at M8
   once the EIR read/write sets of 4.2 exist.
3. **Thread-parallel event-driven simulation** (conservative Chandy–Misra or
   optimistic Time Warp). Research-grade; rarely pays off on RTL. Not planned.

**The kernel is single-threaded in the initial arc.** A cache-friendly
single-threaded loop outperforms a naively parallel one, and determinism (3.8)
comes free. The purpose of the EIR invariant in 4.2 is to keep option 2 open,
not to justify threading the kernel now.

---

## 7. Waveform database

### 7.1 Requirements

- **Selective capture.** Only signals within an active logging scope are
  recorded. On a 100k-LUT design, recording everything exhausts memory within
  seconds of simulated time. Logging scope is user-controlled (`log -r /*` and
  equivalents), and is a *list* of paths rather than one: a reader builds a
  selection out of a subtree and a handful of individual signals, which is what
  `log -r` and `add wave` respectively name. A path that is a signal's own path
  selects that signal, so the two cases need no distinction in the writer.
- **Live streaming.** The GUI displays a waveform while the simulation
  progresses, so the database MUST support concurrent append and read.
- **Random access by time**, to redraw an arbitrary zoom window without
  replaying from the start.
- **A configurable memory budget**, with spill to disk when exceeded rather than
  unbounded growth.
- **Edge navigation.** Given a signal and a time, the next and the previous
  transition. Snapping a cursor to an edge and stepping it from edge to edge
  (10.7) are the two things a reader does most in a wave window, and both have
  to be one query rather than a scan.

### 7.2 Structure

Per-signal append-only chunks of value changes, plus a time index for random
access. Values are read from the state arena (4.3) at capture time.

### 7.3 Export formats

- **VCD** — universal interchange, human-readable, required for interoperability
  and for the differential testing in 14.3. Available from M1.
- **FST** — compressed, far more practical for large designs, readable by
  GTKWave and Surfer. Available from M1.

Export is not an afterthought: for milestone M1 the external viewer *is* the
GUI, and VCD/FST remain the interchange path permanently.

---

## 8. The command line

**The command line is Enzan's control interface, and there is no other.** No
embedded scripting language, and no vocabulary of the tool's own for driving a
simulation: what composes invocations is the shell, `make`, or whatever the user
already builds with. This is a deliberate reversal of the obvious arrangement, and
the reason is that an interface only the tool can speak has to be kept in step with
everything the tool can do, while a parameterised command is composable the moment
it exists.

A consequence worth stating, because it is the thing a reader will look for: there
is no way to stop a running simulation and poke at it from a script. Interactive
inspection is the graphical front end's job (8.4), and programmatic control is
VPI's (section 9). A third way to do it would be a third set of semantics.

### 8.1 Shape

One executable with subcommands, rather than one per phase:

| Invocation | Does |
|---|---|
| `enzan analyse [options] <sources…>` | analyses sources into design units |
| `enzan elaborate [options] <top>` | elaborates and reports; simulates nothing |
| `enzan run [options] <top>` | elaborates and simulates |
| `enzan lib create\|list\|clean <dir>` | manages an analysis library |
| `enzan [options] <sources…>` | analyses and runs in one process |

The last form takes no subcommand and is the shorthand every small case wants;
the first word is read as a subcommand only when it is one of the names above, so
a source file called `run.sv` is still a source file.

**`elaborate` persists nothing.** It exists to answer "does this design build",
which is what a CI job and a `make check` want, and it MUST NOT be a prerequisite
of `run`: an elaboration on disk would be a second serialised format to version
and a second thing to invalidate, and elaboration is not the expensive phase.

**A top level's parameters are set with `-G NAME=VALUE`**, or `-GNAME=VALUE` as Verilator and
cocotb's makefiles spell it. slang applies an override while analysing, so it is in a library
entry's key as a define is (4.4). One that names no parameter of a top level changes nothing,
and is reported rather than ignored, since a misspelt override is otherwise invisible.

**Random numbers are drawn from `--seed N`**, 0 by default (3.8). `$random` is IEEE 1364's
reference generator, bit for bit, so a seed draws in Enzan what it draws in other simulators
that follow it: Icarus Verilog's `$random` starts from 0 as well. `$urandom` and
`$urandom_range` draw from a generator each process has of its own, derived from the seed and
the process's name, so what one process draws does not change when another starts drawing (IEEE
1800 section 18.14).

**Exit statuses are distinct enough to branch on**, because a Makefile and a CI
job need to tell a broken invocation from a broken design:

| Status | Meaning |
|---|---|
| 0 | success |
| 1 | the command line was wrong |
| 2 | analysis or elaboration failed |
| 3 | the simulation ran and the design failed |

Conflating the last two is the mistake to avoid: a regression script that cannot
distinguish "the tool could not compile this" from "the testbench reported an
error" reports both as the same red square.

### 8.2 Analysis libraries

A library is a directory of serialised design units, as section 4.4 describes.
This section is about naming them and reaching them.

**Libraries are optional and there is no implicit one.** With no `--out-lib`,
analysis output lives and dies with the process and a simulation runs from a
one-time analysis. A directory that appeared because the user ran the tool would be
a cache they did not ask for, and section 4.4 explains why that is worse than no
cache: it can change a result, and they would have to find it before they could
stop using it. VHDL's `std`, `ieee` and `nvc` are not caches and are found without
being named (4.4); `--in-lib ieee=<dir>` replaces them.

```
enzan lib create [--name <n>] <dir>   creates a library and fixes its logical name
enzan lib list <dir>                  what a library holds, and what wrote it
enzan lib clean <dir>                 empties it, keeping the library

--out-lib <dir>                       the library analysis is written into
--in-lib <dir>                        a library to read; repeatable
--in-lib <name>=<dir>                 the same, overriding the name it carries
--context <file>                      analysed so references resolve; not stored
```

**The logical name lives in the library, not in the command line.** It is fixed
when the library is created and read from the library thereafter, so the two ends
cannot disagree — a name restated at every use is a name that will eventually be
restated differently, and a design analysed into `uart` and then read back as
`work` fails to find its own units. `--out-lib` on a directory that does not exist
creates one named after the basename, and refuses when the basename is not usable
as an identifier. `<name>=<dir>` exists for the cases a stored name cannot cover:
two vendors each shipping a directory called `work`, or a path whose basename is
not the name the design refers to.

**A library therefore carries an index**, holding its logical name and the format
version. The version belongs there rather than only in each entry so that
`lib list` can report a library written by an incompatible build without decoding
an entry to discover it. **A directory with no index is not a library**: libraries
are opt-in, so a directory that was never created is a mistake to report rather than
a library to invent.

**A library carries the time unit it was analysed in**, in its index beside its name. A
delay is stored as a count of the unit, so `#5` analysed in nanoseconds is five
nanoseconds and nothing else. A run, or an analysis against or into a library, therefore
takes its unit from the libraries that hold units, and `--timescale` may be left out. Given
anyway, it must agree: a run that relabelled the library's counts in another unit would
report every time wrongly, so a conflict is an error (ENZ6028), and so are two libraries in
different units (ENZ6027). A library emptied by `lib clean` holds no units, and can be
filled in another unit.

**Libraries are named because a design refers to them by name.** VHDL requires it —
`library ieee;` resolves against a name rather than a path — so the map exists from
the start rather than being retrofitted at M6. For SystemVerilog the same map gives
the search order for a module nothing analysed defines.

**`--in-lib` is an elaboration-time facility.** It resolves an instantiation of a
module this design does not define. It **cannot** resolve an analysis-time
reference — a package import, an interface named in a port list — because slang
type-checks from syntax and EIR does not feed back into it. Commercial tools manage
this because their compiled library is a semantic artifact their own front end
re-consumes; ours is not. A reader who assumes otherwise will write a library split
that cannot be analysed. **VHDL is the exception**, because a VHDL entry holds NVC's
analysed units (4.4): `use uart.uart_pkg.all` resolves against a library on
`--in-lib` as the language requires, with no source read again.

**Libraries are searched in the order given**, left to right.

**A name that two libraries both define is an error, not a silent choice.** The
message names both, and the fix is to qualify: `uart.fifo`. Picking whichever was
loaded first is the failure mode 1.2 ranks worst, and it is what happens today —
`Design::findModule` takes a bare name and `addModule` is additive, so **EIR MUST
carry the logical library on every unit** for any of this to hold.

**Two copies of one unit are not two definitions.** Libraries analysed against one
IP each store the parameterisations they needed (below), so two of them can both hold
`fifo$Width_8`; and a design analysed and run in one go against a library reproduces
the units it reads from that library's sources. Where every copy of a name encodes
identically once its source locations are left out, the copies are one unit, and the
name resolves. A copy that differs in anything, as one from another version of the
source or from other defines can, or another module that happens to share the name, is
a different unit, and the name is an error as above. Comparing what the units are rather than where they came
from is what makes this safe: a stale library is refused, not chosen.

**An entry's path MUST be derivable from the source it holds**, not from a hash of
that source's text. Entries are per source file (4.4), because that is what the
frontend produces in one piece, so the name comes from the file rather than from any
one unit inside it. A short digest of the file's *path* disambiguates two sources
sharing a basename; it never covers the text, so editing a file rewrites its entry
in place instead of leaving the previous one behind on every save. The text belongs
in the staleness key *inside* the entry, where it says whether the entry is current
rather than where it lives.

#### Where a library boundary may fall

**A library records the sources it was analysed from**, and a design analysed against
it reads them. That is what makes the boundary crossable at all: slang type-checks
from syntax, so a unit instantiating one from another library needs that unit's
declaration however it was analysed before, and EIR does not feed back into slang.
Paths rather than text, because duplicating the RTL into every library that uses it
would make a library a copy of the design instead of an index of it. This is the
SystemVerilog rule; a VHDL library carries NVC's analysed units instead (4.4), and a
design needing a VHDL unit at generic values its library does not hold translates it
from those, parsing nothing again.

**A file is analysed once however often it is named.** Two libraries built on one IP
both record its sources, and a file can be a source, a context file and a library's
source at once. Two readings of one file would define every module in it twice, so a
repeat — by path, after `.`, `..` and symbolic links are resolved — is dropped, and the
first naming keeps its role: a file named as context stays unstored.

So `--in-lib` on `analyse` means *read these libraries' declarations*, and `--in-lib`
on `run` means *resolve against these libraries' units*. The two are the same option
because they name the same thing; what differs is which half of it is wanted.

**A library is instantiable with parameters chosen by whoever uses it.** A
parameterised module becomes one unit per parameterisation, so a unit is not baked
with the parameters its library happened to be analysed with: the design that needs
`Width=8` creates that specialisation from the declarations it read, and stores it in
its own library. A specialisation the input library already holds is **not** stored a
second time, since a second copy of it would only be a second thing to keep current.

This is what makes a library usable rather than what makes it save work. Reading the
other library's sources means re-analysing them, so the boundary costs what it would
have cost without the library. Removing that would take a unit generic over its
parameters, which 2.1 explains SystemVerilog analysis does not produce.

**So a split is for names, not for speed.** Splitting a SystemVerilog design across
libraries saves no analysis and adds some. An edit to a library's sources rebuilds
that library and every library analysed against it, each reading those sources again.
A library analysed on its own holds its modules at their default values, which a
consumer using other values never instantiates and creates again for itself, while a
run still loads both. Measured on 3000 parameterised modules used at a width other
than their default, a two-library build cost 554 ms against 260 ms for one library,
an edit to the testbench rebuilt in 295 ms against 268 ms, and a run took 79 ms
against 46 ms. **A design is therefore built as one library** unless names have to be
kept apart: two vendors' modules sharing a name, the `library` clauses of a VHDL
design (M6), or units whose owner and build are not the design's.

**A specialisation's name MUST be a function of its parameter values.** A counter
numbers the parameterisations in the order one compilation walked them, which makes
the name meaningless outside it — `counter$1` from one analysis and `counter$1` from
another are different modules wearing one name, and an instance binds to the wrong
body with nothing on screen to say so. Naming it for the values gives a
specialisation one identity wherever it is produced, which is the whole of what makes
libraries composable.

**The interfaces a body is given are among those values.** A module reads an interface's
parameters through its port — `localparam W = bus.W` — so two instances setting none of
their own can differ in every width that depends on the bus. Where an interface port is
connected to a parameterised interface, to an array of them, or is generic, the name
carries what it is connected to, as it carries a parameter.

**A parameterised top is named for its values too**, so the name a user writes is the
one the *source* gave it. A top is therefore resolved by source name when no unit has
that exact name, and more than one parameterisation of it is reported rather than
guessed. Its instance takes the source's name as well, so that every hierarchical path
under it — in a waveform, in the front end, through VPI — starts `/counter` rather than
with the specialisation's.

**`--context` remains for a source no library holds.** It names a source analysed for
resolution whose units are not stored — a package shared by libraries that do not
depend on each other, or a vendor header compiled against but not redistributed. A
context file is a dependency of the library that names it: changing it makes that
library stale without being stored in it, so it MUST appear in `--depfile` (8.3). It
buys correct ownership and not speed, since the file is re-parsed by every analysis
that names it.

### 8.3 File lists

`.f` files are the de-facto industry standard and nearly every open-source design
ships one. Enzan reads them; it does not write them. Generating a list is what the
user's own build already does, and a generator here would be a second opinion
about their source tree.

Supported: `-f` and `-F` including nested, `-v`, `-y`, `+incdir+`, `+define+`,
`+libext+`, `-sv`, `//` and `/* */` comments, double-quoted arguments, and `$VAR`
and `${VAR}` substitution from the environment.

**Relative paths follow the convention the existing files were written against.**
A path inside a list reached by `-f` resolves against the process's working
directory; one inside a list reached by `-F` resolves against that list's own
directory. Resolving everything against the containing file would be the better
rule and would break the designs this feature exists to read. **The two spellings
MUST differ at the point they are given on the command line as well as when nested**,
or `-F` becomes a synonym for `-f` exactly where a user is most likely to type it.

**An option the reader does not recognise is an error.** File lists carry
tool-specific switches that change what is compiled — a timescale, a
compilation-unit mode, a language version — and skipping one produces a design
that analyses differently from what was asked for, with nothing on screen to say
so. The position of the option is reported, so the fix is a visible edit rather
than a hunt. `-sv` is the one switch accepted and dropped, since it asks for the
only language the reader reads.

**`$VAR` and `${VAR}` are substituted from the environment, and an unset variable
is an error.** An empty expansion turns `$RTL_ROOT/cpu.sv` into `/cpu.sv`, which
is reported as a missing source rather than as the missing variable that caused it.

Two options exist because a list is something a user has to debug:

**`--dry-run` prints the expanded list, in order, and exits.** The first question
anyone asks of a misbehaving list is what the tool thinks it names, and the answer
should not require a build.

**`--depfile <file>` writes a `make`-style dependency file**, as a C compiler's
`-MMD` does. It names **every file the analysis read** — the sources a list expanded
to, the headers they included, and the context files of 8.2 — not only the includes.
Listing all of them is what lets a Makefile name a `.f` file as the single
prerequisite it can see and get the rest back after the first build, exactly as a C
build bootstraps from a source file to its header dependencies. Without it a build
cannot know that editing an included header should trigger re-analysis, and the
incremental build is wrong in the one way that is both silent and expensive.

**`--depfile-target <name>` names the target those prerequisites attach to.** A
compiler can default this to the object file it is writing; analysis has no single
output file, so the target has to be given. When it is omitted the `--out-lib`
directory is used, which is right for a build that treats the library as its output
and wrong for one that uses a stamp file, so a Makefile should say.

#### A design driven from `make`

The workflow this section exists to serve, in full. Three things are worth reading
out of it: the library is the unit of rebuild, the `.f` file is the only
prerequisite written by hand, and the two `run` forms differ by nothing but a flag.

It is split into three libraries to show the boundary of 8.2 at work, which is the
shape for a design with names to keep apart: two IP blocks, a UART and an SPI
controller, that share a package and do not depend on each other, and a testbench
that instantiates both. A design without that reason is better built as one library
(8.2): the same Makefile with one stamp and one `.f` file.

```make
ENZAN ?= enzan
PKG   := rtl/pkg/types_pkg.sv

# The time unit is fixed when a library is analysed, and a run takes it from there (8.2).
UNIT  := --timescale ns

# Libraries are opt-in (8.2), so creating one is a step of its own rather than
# something that happens because analysis ran.
libs/%/.created:
	$(ENZAN) lib create libs/$*
	@touch $@

# One stamp per library rather than one target per unit: within a library a hit is
# all or nothing (4.4), so the library is the smallest thing make can rebuild.
#
# Neither IP library stores the package: each reads it for resolution only, so
# there is exactly one of it to resolve against at elaboration (8.2).
libs/uart/.stamp: rtl/uart.f | libs/uart/.created
	$(ENZAN) analyse $(UNIT) --out-lib libs/uart -f rtl/uart.f --context $(PKG) \
	         --depfile libs/uart/deps.d --depfile-target $@
	@touch $@

libs/spi/.stamp: rtl/spi.f | libs/spi/.created
	$(ENZAN) analyse $(UNIT) --out-lib libs/spi -f rtl/spi.f --context $(PKG) \
	         --depfile libs/spi/deps.d --depfile-target $@
	@touch $@

# The testbench's list names the package as a source of its own, so the package is
# stored here, with the design that ties the two IPs together. The testbench
# instantiates both, so it is analysed against both libraries, which reads their
# sources (8.2) and names them in its dependency file.
libs/tb/.stamp: rtl/tb.f libs/uart/.stamp libs/spi/.stamp | libs/tb/.created
	$(ENZAN) analyse $(UNIT) --out-lib libs/tb -f rtl/tb.f \
	         --in-lib libs/uart --in-lib libs/spi \
	         --depfile libs/tb/deps.d --depfile-target $@
	@touch $@

# Every file each analysis read. Until these exist a `.f` file is the only
# prerequisite make knows about, which is enough, because nothing is built yet.
-include $(wildcard libs/*/deps.d)

IN     := --in-lib libs/uart --in-lib libs/spi --in-lib libs/tb
STAMPS := libs/uart/.stamp libs/spi/.stamp libs/tb/.stamp

sim: $(STAMPS)
	$(ENZAN) run $(IN) --fst wave.fst tb

gui: $(STAMPS)
	$(ENZAN) run --gui $(IN) tb

check: $(STAMPS)
	$(ENZAN) elaborate $(IN) tb

clean:
	rm -rf libs
```

Editing a file in `rtl/tb` rebuilds `libs/tb` alone, though that analysis reads both
IPs' sources again; editing one in `rtl/uart` rebuilds `libs/uart` and `libs/tb`,
which read it, and leaves `libs/spi` alone; editing `types_pkg.sv` rebuilds all
three, because each named it. None of these is cheaper than rebuilding one library
holding the whole design. `make check` is the elaboration-only form of 8.1, which is
what a CI job wants when it only needs to know that the design still builds.

### 8.4 The graphical front end

**`--gui` is an option of `enzan`, not a second program to learn.** Starting a
window is not a different activity from starting a simulation — in nearly every
case the window exists *because* a run was started — so the user says
`enzan run --gui <top>` and the socket, the listening and the attaching are
arranged out of sight.

**It remains two processes, and that MUST NOT be collapsed.** Section 2.2's
reasons are unchanged: the kernel must be usable with no display attached, a
stalled front end must never stall the kernel, and a local window must be able to
drive a kernel elsewhere. `--gui` therefore starts the front end rather than
becoming it, and the kernel binary MUST NOT link Qt — a headless server and a CI
runner would otherwise install a GUI toolkit to run a batch simulation.

**With `--gui` the design is elaborated and handed over, not run.** The front end
drives every run. This is not only a preference: serving a session puts the design
back to time zero and begins recording there, so anything run beforehand would be
discarded rather than shown. `--run` alongside `--gui` is therefore refused rather
than silently dropped.

**`--vcd` and `--fst` are allowed with `--gui`.** Someone debugging in a window
who then wants a file for later should not have to run the design again, so a
served session records into both the live database and any writer that was opened.

**The kernel parses the command line; the window only attaches.** `--gui` does not
hand the user's arguments to the front end — it analyses and elaborates here, binds
a socket the user is not made to think about, starts a window attached to it, and
ends when that window does. The front end therefore needs to know nothing about
libraries, file lists or any option added later, and the kernel binary still links
no Qt.

**The front end's own options travel through `enzan` too.** Its theme and the signal
list it starts with (10.8) are what a user configures about it, so `--theme`,
`--write-theme` and `--wave-list` are options of `enzan` that are forwarded, and `--write-theme` starts no kernel because writing a
file out is not a simulation. An option reachable only by running the front end
directly would make it the second program this section exists to avoid.

**The front end is a separate executable and not a second entry point.** It remains
a separate *binary* because the kernel MUST NOT link Qt, and a separate *process*
for 2.2's reasons; neither makes it something to invoke. Its command line is
therefore only what `enzan` sends it, and when this project gains an install step
it belongs in `libexec` rather than on a user's `PATH`.

**The lifetime of the kernel follows how the session was started.** The flag a
user chose is the decision, so there is nothing further to configure:

- **`enzan run --gui <top>`** — the window started the kernel, so closing the
  window ends the kernel too, and the process's exit status is the simulation's
  (8.1) rather than the window's. A run nobody is watching, that nobody asked to
  keep, is a leak.
- **`enzan --gui --attach <socket>`** — the window did not start the kernel, so
  closing it only detaches. **`--attach` requires `--gui`**: it is a window that
  attaches to a running kernel, a kernel has nothing to attach to, and accepting it
  without one would start a second simulation while the user waited to be shown the
  first. This is what makes a long run something that can be
  looked in on and left alone again, and it is the case that the shared lifetime
  above deliberately gives up.

**A served kernel removes its socket when it is signalled.** An unhandled signal
ends a process without running a destructor, so the listener's own cleanup never
happens and the path is left on disk; `SIGINT`, `SIGTERM` and `SIGHUP` are handled
for that reason. `SIGKILL` cannot be — it is not deliverable to a handler by design
— so a kernel killed with it always leaves its socket behind. That MUST cost
nothing: **binding probes an existing path and reclaims one nothing is listening
on**, refusing only when a live kernel is there. A leftover path is then a file to
be taken back rather than a state a user has to clear by hand.

`enzan run --listen <socket>` is the other half of the second form: it elaborates
and waits without starting a window. Between them the two forms cover the two
things the explicit spelling is for — a kernel that outlives its window, and a
window on a different machine from its kernel — and nothing else needs it.

### 8.5 One tool, one spelling

Each executable's command line is small, and it is still an interface. The rules
below exist because the programs had already drifted apart without anyone deciding
that they should.

**An option two executables share MUST be spelled the same in both**, short forms
included. `--top`, `--timescale` and `--wave-scope` mean one thing whichever is
given them, and a user who learns `-t` on one and is refused it by the other has
been taught something untrue about the tool.

**`--version` is `-V`, and `--help` is `-h`, everywhere.** `-v` is not a synonym:
`.f` lists spell a library file that way (8.3), and binding it to something else on
one command line invites the two meanings to collide.

**One definition of what `--version` prints**, in `common` rather than in each
executable. Two copies is how one of them came to print its own name in front of a
line that already named the simulator.

**A generated usage text is not automatically the right one.** Qt's parser will
write a help message from the options it was given, and it will not match the one
beside it: a different wrap width, argv[0] as the program name, and its own
options offered as though they were Enzan's. Where a generated text and a
consistent one differ, consistency wins and the text is written out.

---

## 9. External API: VPI, VHPI and cocotb

**cocotb support is a higher priority than advanced SystemVerilog verification
features.** It gives Enzan a full Python-driven verification environment
without Enzan having to implement classes, randomisation, coverage or UVM — an
extremely high return for the effort, and a strong differentiator.

Note the distinction, which `IDEA.md` blurred: **DPI** is for calling C code
*from* SystemVerilog. **VPI** (IEEE 1364/1800) is the introspection and callback
API that cocotb drives simulations through. **VHPI** (IEEE 1076-2019) is the
VHDL equivalent. cocotb uses VPI, VHPI and FLI. Enzan implements VPI first.

The required subset is small and well-bounded:

- Handle lookup: `vpi_handle_by_name`, `vpi_handle`, `vpi_iterate`, `vpi_scan`.
- Properties: `vpi_get`, `vpi_get_str`.
- Values: `vpi_get_value`, `vpi_put_value`. The two-plane value encoding of 3.6
  was chosen to make these nearly free.
- Time: `vpi_get_time`.
- Callbacks: `vpi_register_cb` / `vpi_remove_cb` for `cbValueChange`,
  `cbReadWriteSynch`, `cbReadOnlySynch`, `cbNextSimTime`, `cbAfterDelay`,
  `cbStartOfSimulation`, `cbEndOfSimulation`.

Callbacks MUST fire at the correct point in the region ordering of 3.3:
`cbReadWriteSynch` in the Re-NBA/reactive area, `cbReadOnlySynch` in Postponed.

**cocotb's own regression suite is the acceptance test for this milestone**,
which makes M4 objectively gated rather than a matter of opinion. NVC's VHPI
subset, sized specifically for cocotb, is useful precedent for the VHDL side
later.

The implementation lives in `src/pli/`, and the header applications compile against in
`include/enzan/` (section 12).

### 9.1 Loading an application

`--vpi <library>[:<entry>]` loads a VPI application into the kernel. It is repeatable, and
libraries are loaded in the order given. With an entry the named function is called; without
one, the library's `vlog_startup_routines` array is, which is what the standard names and what
every application provides. A library offering neither is reported, rather than being a library
that was loaded and silently did nothing.

Libraries are opened with `RTLD_NOW | RTLD_GLOBAL`. *Now*, so that an application calling a
routine the kernel lacks fails as it is loaded and names the routine, rather than part-way
through a run. *Global*, because an application commonly loads further libraries that resolve
symbols against it — cocotb embeds Python that way.

**Applications resolve the `vpi_*` routines against the `enzan` executable, which exports
those and nothing else.** Exporting everything would put slang's symbols, and the standard
library's, where an application's own copies of them could collide.

**Every routine the standard declares is defined.** One Enzan does not support fails and
says why through `vpi_chk_error`, rather than being absent: an absent one would make an
application unloadable over a routine it might never call.

**Loading precedes analysis**, so that a library that cannot be loaded is reported before a
long analysis rather than after it, and so that startup routines run before the design exists,
as the standard orders it. Elaboration then binds the design and raises `cbStartOfSimulation`;
everything an application does to the design happens from that callback onwards.

**Plusargs.** A command-line word beginning with `+` that is not one of the analysis options
(`+incdir+`, `+define+`, `+libext+`) is a plusarg. It is accepted, and reaches an application
through `vpi_get_vlog_info`, which reports the whole command line as it was given. Plusargs
belong to the design and to its applications; refusing the ones Enzan does not itself
understand would be refusing all of them.

`--vpi` is an option of `run` and of the one-shot form (8.1). `analyse` and `elaborate` refuse
it, because neither runs anything an application could observe.

### 9.2 The object model

What VPI shows is the elaborated design, named the way the standard names it: dot-separated
from the top, as in `tb.u_dut.count`.

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

**Objects a frontend introduced are not visible.** A port connected to an expression becomes
an extra object and a continuous assignment, named with a `$` so that it cannot collide with
anything the user wrote, and VPI keeps it out of the user's namespace for the same reason.

**One storage may have several names.** A collapsed port is one slot reachable as the child's
port and as the parent's signal; both names resolve, and a value-change callback on either
fires when the slot changes, because they are the same signal.

Two things EIR does not record yet are visible to an application, and are stated so that
nobody mistakes them for VPI defects:

- **Declared ranges are not kept**, so every vector reports `[width-1:0]`, and a
  `logic [0:7]` reports `[7:0]`.
- **Integer atom types are not distinguished.** An `int` is a 32-bit signed two-state vector
  and is reported as `vpiBitVar` rather than `vpiIntVar`; an `integer` is reported as
  `vpiReg`.

Both are changes to EIR's type rather than to VPI, and belong with M5's type work.

**A bit is a signal of its own to an application, and so is an element.** Each reads and
writes its own bits, and a value-change callback on one fires when those bits change and not
when others of the signal do — the kernel reports changes of whole signals, and
`RisingEdge(dut.bus[0])` that fired on every change of `bus` would be wrong in exactly the
testbenches that use it. A force holds a whole signal (10.4), so a bit or an element is forced
by forcing its signal. An array has elements and no value of its own: reading or writing one
is an error that says to use its elements.

**Handles to design objects are interned.** Asking for one twice returns the same handle, and
freeing one does nothing: a handle names something that exists for the whole elaboration.
Iterators and callback handles are freed as the standard says.

**Every handle is checked before it is used.** One that is stale or was never issued fails the
call and is reported through `vpi_chk_error`, rather than being dereferenced. A defect in a
plugin must not become a crash that the user reports as one in the simulator.

### 9.3 Callbacks and the region ordering

Section 3.3 allows the PLI regions to be hook points rather than queues. These are the points:

| Reason | Fires |
|---|---|
| `cbStartOfSimulation` | once, after elaboration and before anything at time zero runs |
| `cbNextSimTime` | when time advances, before anything at the new time runs |
| `cbAfterDelay`, `cbAtStartOfSimTime` | at the start of their time slot, after `cbNextSimTime` and before the Active region is drained |
| `cbValueChange` | at the change: once the new value is in the arena, the processes it wakes are scheduled and listeners have recorded it |
| `cbReadWriteSynch` | when Active, Inactive, NBA, Observed and the Reactive family are all empty; whatever it wakes then runs, and it may fire again in the same slot |
| `cbReadOnlySynch` | after the last read-write pass of a slot, before Postponed |
| `cbEndOfSimulation` | once, when the simulation ends, after `final` blocks |

**A callback registered for the phase that is running runs at that phase's next
occurrence.** A `cbReadWriteSynch` registered from a read-write callback runs in the next
read-write pass of the same slot, once what the first one woke has settled; a
`cbReadOnlySynch` registered from a read-only callback runs in the next slot; a `cbAfterDelay`
of zero registered mid-slot runs before the Active region is next drained. Running any of them
in the pass that registered it would make the order depend on where in a list a callback
happened to be appended.

**A read-write pass counts as a delta cycle**, against the limit of 3.2. An application that
asks for read-write from read-write without ever letting time move is a delta loop, and the
limit is what finds one.

**A pending timed callback keeps the simulation running.** A design with nothing scheduled
and a `cbAfterDelay` outstanding has not finished: a testbench written in Python is exactly a
design that schedules nothing of its own.

**Value-change callbacks are delivered synchronously and may re-enter.** A callback that
writes a signal with `vpiNoDelay` runs that signal's callbacks before its own write returns,
as every other simulator does; cocotb queues for exactly this reason.

Callbacks of one reason fire in the order they were registered (3.8).

### 9.4 Writing values

`vpi_put_value` is the kernel's write operations of 10.4, not a second implementation of
them:

| Flag | Effect |
|---|---|
| `vpiNoDelay` | written now, waking whatever watches the signal: a deposit |
| `vpiInertialDelay`, `vpiTransportDelay`, `vpiPureTransportDelay` with no delay | written in the NBA region of the current slot, as a non-blocking assignment is |
| the same, with a delay | written at the start of the slot the delay reaches, before that slot's `cbAfterDelay` |
| `vpiForceFlag` | a force |
| `vpiReleaseFlag` | a release; `value_p` receives the value the signal keeps |

**The three delay modes are not told apart.** Each schedules its write and none cancels a write
already pending, so an inertial write behaves as a transport one. Modelling the cancellation
needs a per-signal record of pending writes that nothing in the M4 subset would exercise;
cocotb writes with no delay.

`vpiCancelEvent` and `vpiReturnEvent` are refused, and so is any write during
`cbReadOnlySynch`, as the standard requires: a read-only phase that could be written would
report values that depend on the order its callbacks ran in.

### 9.5 Ending

**`vpi_control(vpiFinish)` is `$finish`.** The run ends when the current callback returns,
`final` blocks run, `cbEndOfSimulation` fires and the exit status is 0.

**`vpi_control(vpiStop)` stops the run at the next time-slot boundary**, exactly as the
front end's Stop does, and for the same reason: that is the only boundary a run can be resumed
from with the same result (section 2.2, and `RunInterrupt`). A served session then waits for
its client. A batch run has nothing that could resume it, so it ends there.

**`cbEndOfSimulation` fires once, whichever way the simulation ends**: by `$finish`, by
running out of events, by an error, or by the process ending while the simulation could still
have continued — a batch run's `--run` limit. An application that writes its results from
that callback, as cocotb does, otherwise never writes them.

**A design cannot be restarted while an application is loaded.** Putting it back to time zero
would leave the application holding the state of a simulation that no longer exists, and
there is no callback in the standard that tells it so.

### 9.6 cocotb

cocotb chooses a simulator by the name of a makefile in its own directory and has no way to
add one from outside. `tools/cocotb/Makefile.enzan` is that makefile, and
`tools/cocotb/makefiles.sh <dir>` writes a copy of cocotb's makefile directory with it added,
for `make SIM=enzan COCOTB_MAKEFILES_DIR=<dir>`. The harness of 14.6 runs cocotb's own suite
this way, so what is tested is what a user runs.

**The VPI library is `libcocotbvpi_dsim`**, which is cocotb's VPI layer compiled with no
simulator-specific workaround in it. Enzan is held to what cocotb expects of a standard
simulator rather than to whatever another simulator's quirks taught it to tolerate.

`COCOTB_HDL_TIMEPRECISION` becomes `--timescale`, since Enzan counts every delay in one unit
(3.1). It must name a single unit, `1ns` and not `10ns`, which is all `--timescale` accepts.

**A design's own delays are read in that unit too.** The frontend does not yet honour
`` `timescale `` or a module's `timeunit`, which section 3.1 requires: `#10` is ten of whatever
`--timescale` says, so a design written for nanoseconds and run at cocotb's default picosecond
precision runs a thousand times fast. A testbench in cocotb that drives every event itself, as
most do, is unaffected; a design with delays of its own needs `COCOTB_HDL_TIMEPRECISION` set to
the unit it was written in until the frontend converts delays as 3.1 says it must.

### 9.7 VHPI

VHPI is how cocotb reaches a VHDL toplevel (M6), as VPI is how it reaches a SystemVerilog one.
What VHPI names that VPI does not is the VHDL type of everything: a signal's type and its base
type, an array's bounds, direction and element type, a record's fields, an enumeration's
literals, a scalar's range. cocotb reads them to decide what a handle is, `std_logic` or a
vector of it, an integer, a record whose fields it can name, before it reads a value.

**The types live in EIR**, as `eir::VhdlType` in a table of each definition's that its objects
and generics index, filled by the VHDL adapter from the layouts it gives them (5.2). They change
nothing about how a design behaves, as an enumeration's table changes nothing; they say what
the bits mean to something outside the simulation, and are serialized with the definition, so a
library holds them. The analysis that knows them runs in a child process (5.2) and is gone by
the time an application asks, so EIR is the only place they can be kept.

**VHPI is VPI's context with a second set of routines.** `--vhpi <library>[:<entry>]` loads an
application as `--vpi` does, running its `vhpi_startup_routines`, into the one `PliContext`: the
kernel calls one set of hooks, and cocotb loads both its layers for a design of both languages,
which must see one simulation. Its header is the IEEE P1076 working group's `vhpi_user.h`, which
the NVC the VHDL frontend is built on carries, so VHPI is built only with that frontend.

- **Regions** are VPI's scopes: the root instance, named for its entity; component instances;
  and the blocks and generate iterations the frontend flattens into names, an indexed one
  (`g(0)`) a for-generate iteration and any other a block.
- **Objects** are a region's signals and ports, every one with a VHDL type, a real's included,
  and its generics. An element of an array is an indexed name and a field of a record a
  selected one, each a window of its object's bits; an array's elements are numbered row by
  row from the left, which is its highest bits.
- **Types** come from the table: `vhpiBaseType` a type declaration of the type's kind, with an
  array's base type unconstrained, `vhpiType` the object's subtype with its bounds,
  `vhpiElemType` an array's element, `vhpiConstraints` its ranges, `vhpiEnumLiterals` an
  enumeration's literals. Names are basic identifiers in upper case, `vhpiCaseNameP`'s as well
  as `vhpiNameP`'s: neither NVC's analysis nor Enzan keeps the case a name was declared in, and
  NVC gives the same.
- **Values** are read from and written to the bits in VHPI's formats by the type: a
  std_logic or a bit and arrays of them in the Logic formats, other enumerations' positions,
  integers, reals, strings of characters, binary strings. A record is read and written field by
  field.
- **A deposit is an update of the time step**, applied after the callback that made it returns,
  as VPI's write with no delay is: a deposit applied inside the call would wake what watches the
  signal inside the application's own call, which cocotb, writing a batch of values, does not
  expect. A force and a release are of the whole signal.
- **Callbacks** share VPI's dispatch: the last known delta cycle of a time step is VPI's
  read-write synchronisation, its end the read-only one, and the next time step VPI's next
  simulation time. A repetitive callback, `vhpiCbRep...`, fires at each occurrence until it is
  removed, waiting for the next time step after each.

**cocotb's VHPI library is `libcocotbvhpi_nvc`**, whose one workaround, to compare names
without their case, answers the one way Enzan differs from the standard, the declared case it
does not keep (section 9.6 chose the VPI library by the same rule). The toplevel's language
decides which is loaded, `--vpi` or `--vhpi`. A design of both languages is not simulated yet,
and the makefile skips it as cocotb's makefiles skip a language a simulator lacks; when it is
(M7), cocotb loads the other language's library itself, named in `GPI_EXTRA`, as it does
elsewhere.

A VHDL signal of a null range holds nothing, and a bit vector has at least one bit, so its
object has a placeholder bit that nothing reads, writes or records (`eir::Object::holdsNothing`);
VHPI shows it as the empty array its type says it is.

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

Docking uses `QDockWidget`. Layout is persisted and is definable from a Tcl
configuration script.

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
  the same traces one line up.
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

Dark throughout. **Every colour MUST come from the theme configuration file**;
no colour literals in widget code.

Waveform view defaults: high-contrast black background, light green traces.

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
  remove. It is a menu rather than a set of window-level commands because every
  entry is about *that* cursor, and a command that acted on "the active one"
  would need the reader to make it active first and then remember that they had.
- A cursor is added from the toolbar, beside the zoom controls, because adding
  one is part of measuring rather than part of configuring the window.
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
  them, and Ctrl+V pastes them beside whatever is current — inside it when that
  is a group. A copy keeps its radix, its highlight and how far it was unpacked,
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
  subscription: the kernel streams the signal once however many of its bits are
  on screen, exactly as it already does for an extracted bit range (10.3). Arrays
  gain further levels of unpacking later, and the mechanism is built for that now
  so that it does not have to be built twice.
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
  step between and it is *not* clipped at the recorded extent the way a trace is
  — a trace stopping there says "nothing has happened here yet", which is true of
  a signal and false of a parameter, and before the first run the clip would hide
  it altogether. Immutable: it has nothing behind it to unpack into, so it offers
  no expander; a `+` that did nothing would be worse than no `+`.

  It has no slot, no trace and nothing to subscribe to, so its value travels with
  the row — including onto the clipboard, because there is nowhere for the other
  end to look it up. A parameterised module is already one analysed definition
  per parameterisation (4.4), so what each one resolved to is a fact the frontend
  records about that definition and nothing else could reconstruct.

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
  rendering fault.

**What this does not change.** Panning with the middle button, the keyboard zoom
and pan, cursor locking, grouping and per-row radix all stay as they are.

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
- **A signal with `[h:l]` or `[n]` after it** adds a row showing that bit range (10.3). The
  whole path is looked up as a signal first, so a name that itself ends in brackets is never
  taken for a range.
- **A scope** adds what *Add to wave* on that scope in the Structure window adds: a group
  named by the scope's path, holding its signals. With `-r` it adds what *Add to wave
  recursive* adds: one such group for each scope of the subtree, side by side, in the
  Structure window's order. Both are defined as those menu entries rather than beside them,
  so that the two cannot come to disagree.

`group` adds a group named `name` holding the rows its body describes. The body is a
signal list of its own — `add wave` and `group` commands — and it may be empty, which is
why a group is a block rather than an option on each of the lines inside it.

| Option | Meaning |
|---|---|
| `-radix bin\|oct\|dec\|hex\|ascii` | the radix, and the number rather than an enumeration's name, as choosing a radix from the menu does; `ascii` shows the bytes as characters |
| `-symbolic` | an enumeration's names after all; applied after `-radix` wherever it is written |
| `-label <text>` | the row's name, for a single signal or a non-recursive scope |
| `-highlight <1–5>` | the row's background (10.7) |
| `-expand` | unpacked into its bits, exactly as unpacking leaves it |
| `-contents <body>` | unpacked, with exactly the rows `body` describes beneath it |
| `-r` | for a scope path, the whole subtree |

An option applies to every row its command adds. `group` takes `-radix`, `-symbolic` and
`-highlight`, which are what a group's own line carries.

An unpacked signal is a body rather than a list of bit numbers because that is what the
window lets it become: its bits can be deleted and reordered, and other rows can be dragged
into it (10.7). `-contents` describes any of that with the same two commands, each bit with
its own radix, label and highlight, so nothing the window can show is beyond the file.

**Saving writes what the window shows.** *Save signal list…* writes every row, top to
bottom, so that loading the file rebuilds the same list:

- a label only where the row was renamed, `-expand` for a signal exactly as unpacking left
  it and `-contents` for one that has been changed since;
- signals, ranges and parameters by path, never a scope or `-r`, because by then the rows
  exist one by one and may have been rearranged;
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

*Load signal list…* replaces the window's list. Adding one list to another is what copying
and pasting are for.

**A mistake in the file is an error; a path the design lacks is not.** An unknown command,
an unknown option, a bad value or an unbalanced brace is reported with its file and line,
and nothing is loaded. A path that names nothing in this design is a warning, with its file
and line, and that row is skipped while the rest load. A list outlives the version of the
design it was saved from, and refusing a whole arrangement because one signal was renamed
would make the file useless at exactly the moment it is needed.

**From the command line, `--wave-list <file>`**, an option of `enzan` forwarded to the front
end as `--theme` is (8.4): `enzan run --gui --wave-list <file> <top>`, or with `--gui
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
├── scripts/                  repository checks, runnable locally and by CI
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

Per `IDEA.md`: every public class and every non-trivial public method MUST
carry a Doxygen `///` comment.

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
count.

`tests/conformance/ghdl/run-suite.sh` runs GHDL's own regression tests, its `testsuite/gna`, from
GHDL at the revision VESTs is run from. Each test's `testsuite.sh` runs unchanged under GHDL's
`testenv.sh`, with `ghdl-shim.sh` as the `ghdl` it calls: since Enzan analyses and runs VHDL in
one process rather than through libraries on disk, the shim's `-a` analyses every file recorded
so far with the new ones and records them, and `-e` and `-r` elaborate or run a top over all of
them, `--expect-failure` inverting what Enzan's exit says. A test passes when its script exits 0.
A test of what only GHDL has, synthesis, VPI or VHPI, GHW waveforms, PSL, a C compiler or a
feature it probes for, is in neither count; one comparing GHDL's own messages, or using its
vendors' IEEE packages or several work libraries, fails. So does one expecting an analysis
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
  M6.1.

Start with the small ones and add larger designs as capability grows.

NEORV32's is held to GHDL's reports and to the CPU trace and UART logs its testbench writes, for
the 160 us its on-chip debugger sequence takes, since those hold every instruction each core
retired; on the main branch only, as it takes minutes.

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
run under a second simulator, and it must pass there too.

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
  SystemVerilog frontend, since a second job would build slang a second time to run them.
- The design regression of 14.5 for the designs that run quickly: `aes` and `common_cells`.

Jobs on `main` only:

- `clang-tidy`, which analyses every source file.
- The design regression for `taxi`, whose cocotb testbenches take minutes.

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
configurations in 389, while NEORV32 does none of these. The verification libraries are
M6.1, so that the VHDL frontend, the simulation cycle and VHPI are proven on designs before
they are asked to carry a library's runtime.

**Access types are in M6 only as far as `std.textio` needs them.** NEORV32 cannot be
simulated without `line`: its UART prints what the program sends through `write` and
`writeline` in every simulation, from its RTL rather than its testbench, and the testbench
logs both UARTs to files and can load a memory from one. So M6 builds the heap of 4.3 —
access values are handles in the arena into a heap the kernel owns, since 4.3 lays out every
piece of state at elaboration and 13.4 forbids allocating on the write path — and makes it
work for an access to an array, as `std.textio` uses one, with textio's subprograms run
natively over it (4.3). M6.1 uses the same heap for everything else.

The exit designs need no program compiled for them, for the reason M5 gives:

- **NEORV32**, simulated by its own testbench with the program image its repository
  commits. The testbench drives the on-chip debugger over JTAG — halting the CPU, writing
  memory through it, resetting and resuming it — and asserts each step.
- **cocotb's VHDL designs**, through its own test cases with a VHDL toplevel (14.6). These
  are what VHPI is measured by, as cocotb's suite measured VPI in M4.
- **VESTs and GHDL's and NVC's own suites** (14.4). Most of each needs nothing M6.1 brings,
  and a pass list per suite lets the rest wait without hiding a regression.

**Exit:** NEORV32's testbench completes its debugger sequence under Enzan with no assertion
failing, and its transcript is GHDL's line for line; cocotb's suite runs with a VHDL
toplevel with a recorded pass rate; VESTs and GHDL's and NVC's suites run with recorded pass
rates.

### M6.1 — VHDL verification libraries

What VUnit and OSVVM are built from: access types as a general heap, with `deallocate`;
protected types and shared variables of them; generic packages and their instantiation;
configurations; external names. VUnit's simulator interface for Enzan, which VUnit chooses
by name as cocotb chooses a makefile (9.6).

Numbered with the VHDL milestone and placed straight after it, while the frontend is warm.
Nothing in M7 depends on it: a mixed-language design is RTL meeting RTL.

**Exit:** VUnit's own suite and OSVVM's regression each run with a recorded pass rate.

### M7 — Mixed language

A design whose hierarchy crosses between SystemVerilog and VHDL, in either direction and at
any depth, simulated by the one kernel of 3.5. Everything below the frontends already serves
both: EIR, elaboration, drivers and resolution (3.7), the kernel, the waveform database and
the PLI context hold one design whatever language each definition came from. What M7 adds is
the meeting of the two frontends and of the two interfaces.

**Analysis of both languages in one session.** The session analyses SystemVerilog through
slang and VHDL through the NVC child into one `eir::Design`; it refuses a mixed source set
today. VHDL goes into Enzan's libraries as SystemVerilog does, `--out-lib` and `--in-lib`
(8.2), and a design may draw on several VHDL libraries at once, not only the one `--work`
names, since a mixed design is usually built as a testbench in one library and its DUT in
another. Both are also what the GHDL suite's multi-library tests and cocotb's VHDL designs in
their own libraries need (14.4, 14.6).

**Instances across the boundary.**

- *SystemVerilog instantiating a VHDL entity.* The entities' interfaces, their generics and
  ports with their types, are given to slang as the modules it would otherwise not find, so
  that the instance type-checks; its parameter values are the generics' values, and the
  architecture is the entity's default one or the one a library binding names.
- *VHDL instantiating a SystemVerilog module.* A component with no VHDL entity to bind is
  bound to the module of its name (the default binding of IEEE 1076 section 7.3.3 extended
  to the other language, as commercial tools extend it), and `entity work.m` names a module
  the same way; the component's generics are the module's parameter overrides.
- *Specialisation across the boundary.* VHDL generics are folded by the adapter (5.2) and
  SystemVerilog parameters by slang, so each side's instance asks the other's frontend for
  the specialisation its values make. The session alternates between the frontends until
  neither asks for one the other has not given, as elaboration asks the VHDL frontend for
  resolution functions (3.7), with a bound on the rounds so that a hierarchy recursing
  through both languages is reported rather than followed for ever.

**Ports across the boundary.** A port whose two sides are of different languages is
connected by a zero-delay continuous assignment that converts between them and consumes one
delta cycle (3.5), in each direction its mode needs:

- `std_ulogic` codes to four-state bits with `L`→`0`, `H`→`1` and `W`, `U`, `-`→`X` (3.6),
  and `0`, `1`, `X`, `Z` back to their codes; `bit` and `boolean` to two-state bits and back;
- `std_logic_vector` and its kin to packed vectors, left element to the most significant bit;
  integers to `int` or `longint` by their range; reals to `real`; arrays of these to unpacked
  arrays and records to packed structs, field by field, where the shapes agree;
- an `inout` port as two conversions, each a driver on its own side, so that each language's
  resolution sees the other's contribution as a driver: a `std_logic` signal resolves the
  SystemVerilog side's value with its own function, and a net resolves the VHDL side's as a
  driver of its kind. A port no rule covers is refused by name.

**The interfaces across the boundary.** VPI and VHPI share the one PLI context (9.7), and
each sees the whole hierarchy: VPI shows a VHDL region as a module and its objects as nets of
their four-state bits; VHPI shows a SystemVerilog region as one of kind `vhpiVerilog`, which
cocotb leaves to VPI. cocotb runs a mixed design as it does elsewhere: the toplevel's
language's library is loaded with `--vpi` or `--vhpi`, and the other's is named in
`GPI_EXTRA`, which `Makefile.enzan` sets in place of the skip it has today (9.7).

**Reference simulation.** No free simulator takes SystemVerilog and VHDL together. NVC
simulates Verilog with VHDL, and is the reference for the mixed cases its Verilog subset
covers, its own `mixed` regression tests among them (14.4). The rest are checked by
splitting: the same design with the boundary moved — the DUT in one language driven by a
testbench in the other, against the DUT in its own language's testbench under iverilog or
GHDL — matches transcript for transcript, the boundary's delta aside.

The exit designs need no program compiled for them, for the reason M5 gives:

- **NEORV32** in a SystemVerilog testbench that drives it as its VHDL one does and checks
  its UART output, against the VHDL testbench's run under GHDL (14.5).
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

---

## 19. Risk register

| Risk | Impact | Mitigation |
|---|---|---|
| NVC internal APIs drift or prove unusable as a frontend | High — blocks VHDL | Spike first in M6; pin the revision; thin adapter in one directory; documented co-simulation fallback (5.2) |
| The predefined and foreign subprograms NVC's runtime resolved are a larger body of work than estimated | High — inflates M6 | Enumerate the set during the M6 spike by walking the analysed tree, before the milestone is planned (5.2) |
| VHDL generic and subtype elaboration must be reimplemented rather than reused from NVC | Medium — inflates M6 | Sized by the M6 spike, which also decides whether generics are folded by the adapter or by elaboration (5.2); the tree boundary is reconsidered if it proves worse than the alternative (5.2) |
| A defect inherited from NVC's frontend is invisible to differential testing against NVC | High — silent wrong results | GHDL is the primary VHDL reference and both conformance suites are run (14.3, 14.4) |
| Mixed-language delta semantics differ from commercial tools | High — silent wrong results | Specify explicitly (3.5); golden tests against references before declaring it working |
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
