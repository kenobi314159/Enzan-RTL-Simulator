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

The dependency stack is internally consistent under GPLv3:

| Dependency | License | Compatible |
|---|---|---|
| NVC (VHDL frontend) | GPLv3 | yes, and the reason for the choice |
| slang (SystemVerilog frontend) | MIT | yes |
| Qt 6 | LGPLv3 | yes |
| Tcl | BSD-style | yes |
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

**Elaboration** takes a named top level plus the libraries, resolves parameters
and generics, expands generate constructs, builds the instance tree, binds
ports, computes the flat state layout, and produces an executable image.
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
   │   enzan (kernel)     │ <──────────────────────────────> │   enzan-gui      │
   │  headless, batch     │      versioned binary protocol   │   Qt 6 widgets   │
   │  Tcl control layer   │                                  │   attach/detach  │
   └──────────────────────┘                                  └──────────────────┘
```

The GUI MUST be able to attach to an already-running kernel and to detach
without terminating it.

### 2.3 Component map

```
   HDL sources
        │
        ├── slang ──────────┐
        │                   ├──> frontend adapters ──> EIR (analysis output)
        └── NVC ────────────┘                               │
                                                            ▼
                                                     library store (work/)
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
                                   waveform DB    VPI / VHPI     Tcl control
                                        │             │              │
                                   VCD / FST       cocotb         GUI / scripts
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
provided the callbacks fire at the correct point in the ordering.

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

**VHDL `std_logic`** has nine values (`U X 0 1 Z W L H -`) and does not fit two
planes. It is stored as **one byte per element**, holding the enumeration
value. Conversion to the packed two-plane form happens at language boundaries,
in the waveform database, and in the PLI, with `L`→`0`, `H`→`1`, `W`/`U`/`-`→`X`.
A future optimisation MAY pack `std_logic_vector` values that provably carry
only `{0,1,X,Z}`; this is not part of the initial implementation.

Integers, reals, strings and aggregate types use native representations
described in the EIR type system (section 4).

### 3.7 Drivers and resolution

Each process that assigns to a signal owns a **driver** on that signal. A signal
with exactly one driver takes its driver's value directly. A signal with more
than one driver computes its value with a resolution function.

- VHDL resolution functions are arbitrary user code. They are compiled to EIR
  like any other subprogram and invoked by the runtime. The runtime MUST NOT
  assume resolution is a built-in table.
- SystemVerilog nets (`wire`, `wand`, `wor`, `tri`, `triand`, `trior`, `tri0`,
  `tri1`, `supply0`, `supply1`) use built-in resolution tables.
- Single-driver signals MUST bypass resolution entirely. This is the common
  case and it is on the hot path.

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
- All randomisation MUST draw from an explicit seeded generator. The seed MUST
  be recorded in the transcript and MUST be settable from the command line.
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

### 4.4 Libraries

Analysis output is serialised into a library directory (`work/` by default,
mirroring the convention users know from Questa). Entries are keyed by a hash of
the source text plus the analysis options that affect the result, so unchanged
units are never re-analysed.

**Everything that changes what analysis produces belongs in the key**, not only
the source text: the macro definitions, the include path, the serialised format
version and Enzan's own version. A define that is outside the key is a define
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

**Using a library is opt-in.** Nothing consults one until the user creates it,
exactly as Questa makes `vlib` precede `vlog`. The version is in the key and the
format version is disciplined, but a cache that can change a simulation result
is a thing a user must be able to stop doing without having to find it first.

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

slang requires **C++20**, which sets the project's language floor.

### 5.2 VHDL via NVC

[NVC](https://github.com/nickg/nvc) provides the VHDL frontend. It supports
almost all of VHDL-2008 and is actively maintained.

**Understand the integration honestly before working on it.** NVC is not a
frontend library; it is a complete simulator written in C that lowers VHDL
through its own `vcode` IR to LLVM and runs it on its own scheduler with its own
VHPI runtime. There is no stable public API for "give me the elaborated VHDL".

The integration plan is therefore:

- Vendor NVC as a git submodule pinned to a specific revision.
- Use its lexer, parser and semantic analysis, consuming the resulting `tree_t`
  after its simplification and bounds-check passes.
- **Do not** use its `lower`/`vcode`/LLVM path, its runtime, or its scheduler.
- `src/frontend/vhdl/` walks the analysed tree and emits EIR, exactly as the
  SystemVerilog adapter does.

Known risks, to be confirmed by a spike at the start of M6: NVC's internal APIs
are not stability-guaranteed and will drift; NVC is C and uses its own arena
allocators and global state, so reentrancy and ownership need checking; and the
C/C++20 boundary needs careful `extern "C"` wrapping.

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

## 8. Control layer

### 8.1 Tcl

Tcl is the control and configuration language, matching the convention users
already know from Questa, Vivado and Xcelium. Tcl 8.6 or later is embedded in
the kernel, and it is **not optional**: from M3 every path into the simulator —
a batch run, a served kernel, a script — goes through a control session, and a
build without the interpreter would be a configuration nobody runs and nothing
would notice breaking.

The command vocabulary SHOULD follow Questa/Modelsim naming where practical, so
that existing user muscle memory and existing scripts transfer with minimal
friction: `vlib`, `vlog`, `vcom`, `vsim`, `run`, `restart`, `add wave`, `log`,
`force`, `examine`, `radix`, `find`, `when`, `quit`, and for source debugging
`bp`, `bd`, `enablebp`, `disablebp` and `step`.

Where Enzan has no equivalent of a Questa concept, the command is **absent
rather than approximated**. A script that relies on one then fails at the
command, which the user can see and fix, instead of producing a different
simulation, which they cannot. `vcom` is refused with the milestone it arrives
at rather than ignored, for exactly this reason.

Three of the commands have a definition worth writing down, because the obvious
reading of each is wrong:

- **`run` with no argument runs to the end**, the same as `run -all`. Questa
  advances by a configurable default run length instead; Enzan has no such
  setting, and inventing one so that the two spellings could differ would be a
  setting nobody asked for.
- **A time in a coarser unit than the precision is converted; a finer one is
  refused.** `run 1ps` on a nanosecond design would round to `run 0`, and a
  script that appears to run and does nothing is worse than one that stops. The
  conversion is integer throughout, because 3.1 puts no floating-point time
  anywhere in the kernel.
- **`examine` with no radix names an enumerated value** — `S_SEND`, not
  `3'h2` — which is what the wave window shows (10.3) and what a reader asking
  about a state register means. A radix asks for the encoding instead, and
  `radix symbolic` goes back.

**A front end's command entry sends text, not named commands.** The kernel
evaluates it through the same interpreter a `do` file goes through, against the
same session. A protocol with a request per command would be a second vocabulary
to keep in step with the first, which is the whole thing this section exists to
prevent.

A command may run the design, and while it does the socket is serviced from
inside the kernel's interrupt poll — so the kernel MUST report itself as running
for the whole evaluation. Without that, a second command arriving mid-run is
dispatched and reads a design half way through a time slot.

A command reports a user error through its Tcl result rather than through the
diagnostic engine, and a failure of the phase it invoked through the engine.
The distinction matters: a script probing for a signal that may not exist can
catch a Tcl error, and reporting it as a diagnostic would raise the error count
and make an otherwise clean run exit as a failed one.

**All user configuration is Tcl.** There is no second configuration format. The
GUI theme is a Tcl script setting colour variables; the window layout is a Tcl
script. This keeps one configuration language across the whole tool and adds no
dependency, and it is what commercial EDA tools do.

Three properties of the embedding are normative, because each one is invisible
in a passing test and expensive to retrofit.

**Commands are C++, not Tcl procedures.** The vocabulary is installed as native
commands so that the same operation is reachable from the GUI and from VPI,
neither of which goes through Tcl. A command parses its arguments, calls the
component that does the work, and turns the answer into a result or a
diagnostic; a command that implements semantics of its own has put behaviour
somewhere only one of the three control surfaces can reach it.

**Script output goes to the transcript.** `puts` is replaced by one that writes
where diagnostics write, because a GUI attached to a kernel (2.2) would
otherwise see nothing a script said — the text would land on a terminal on
another machine. Tcl's own `puts` is kept and delegated to for any other
channel, so a script writing to a file it opened still works.

**`quit` MUST NOT end the process from inside the interpreter.** Tcl's `exit`
does exactly that, and the kernel has a waveform file to finish and a socket to
unlink. `quit` records the request and the driving loop unwinds, so the exit
status is the same and nothing is lost.

The control layer is built as two libraries, and the split is a design
constraint rather than a build detail. The interpreter and the file-list
machinery depend on `common` alone; the command vocabulary, which owns a
session, depends on the kernel. The GUI links only the first, for the theme
script: a front end that linked the kernel to read a list of colours would
quietly undo the process separation of 2.2.

**A session owns the simulation, and there is one of it per kernel process.** A
batch run, a served kernel and a `-do` script are the same pipeline through the
same object. A server that built a kernel of its own would make two, and a
command typed at a front end would then act on a different simulation from the
one the front end is showing — which is the failure mode that makes a debugger
untrustworthy rather than merely wrong.

A consequence worth stating because it is easy to get wrong: whatever registers
itself against a kernel — a value-change listener, the interrupt poll — must
take that registration back before it is destroyed, because the kernel now
outlives it. A listener destroyed while still registered is a dangling pointer
the *next* run calls through.

### 8.2 File lists

Both forms MUST be supported:

- **`.f` files** (`-f list.f`), the de-facto industry standard. Nearly every
  open-source design on GitHub ships one. Support `-v`, `-y`, `+incdir+`,
  `+define+`, `-sv`, comments and nested `-f`. Requiring users to write Tcl
  wrappers around designs that already ship `.f` lists is pointless friction.
- **Tcl file lists**, for designs and flows that prefer them.

Three details of the `.f` reader are normative, because each is a place where
the obvious behaviour and the compatible one differ.

**Relative paths follow the convention the existing files were written
against.** A path inside a list reached by `-f` resolves against the process's
working directory; one inside a list reached by `-F` resolves against that
list's own directory. Resolving everything against the containing file would be
the better rule and would break the designs this feature exists to read.

**An option the reader does not recognise is an error.** File lists carry
tool-specific switches that change what is compiled — a timescale, a
compilation-unit mode, a language version — and skipping one produces a design
that analyses differently from what was asked for, with nothing on screen to
say so. The position of the option is reported, so the fix is a visible edit
rather than a hunt. `-sv` is the one switch accepted and dropped, since it asks
for the only language the reader reads.

**`$VAR` and `${VAR}` are substituted from the environment, and an unset
variable is an error.** These files name paths through variables constantly.
Expanding an unset one to nothing turns `$RTL_ROOT/cpu.sv` into `/cpu.sv`,
which is then reported as a missing source file rather than as the missing
variable that caused it.

Beyond the switches named above the reader also takes `+libext+`,
double-quoted arguments for paths containing spaces, and `/* */` alongside `//`.

### 8.3 Compile-order inference

Users MUST NOT be required to specify compile order manually.

A fast pre-scan pass extracts defined and referenced symbols from each file —
modules, packages, interfaces, programs, entities, architectures,
configurations — and builds a dependency graph. Files are then analysed in
topological order. Genuine dependency cycles are reported as errors with the
cycle spelled out; legal mutual references that do not require ordering MUST NOT
be reported as cycles.

The pre-scan MUST be much cheaper than full parsing, since it runs over every
file before any analysis happens. One linear pass over the bytes, skipping
comments and string literals, recognising identifiers, and tracking with a small
stack whether a keyword sits at file scope or inside something.

**Instantiating a module is not a dependency.** Elaboration resolves an
instance against the library by name (2.1), long after both files were analysed,
so two modules that instantiate each other are exactly the legal mutual
reference this section forbids reporting. In SystemVerilog the constraint that
does exist is the package: a unit that imports one, or names a member of one
through `::`, cannot be analysed before it. The scan is written so that this
falls out rather than being special-cased away afterwards.

The scan is approximate and the direction of the approximation is chosen. A
declaration it invents costs nothing, because a declaration only ever *satisfies*
a dependency. A reference it invents could invent a constraint, so a reference
becomes an edge only when the name it uses is a package that another scanned
file declares: `some_class::CONST` orders nothing, because no file declares a
package by that name.

Conditional compilation is not evaluated: text inside a `` `ifdef `` that would
be false is scanned like any other. Analysing a package nothing ends up
importing is harmless; failing to analyse one that something does is not.

Where nothing orders two files, they keep the order they were given. Analysis
order is observable — in diagnostics, and in which of two duplicate definitions
is reported as the redefinition — and 3.8 makes observable ordering
reproducible, which neither a hash-ordered nor a stack-ordered traversal would
be.

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

Required features: multiple named cursors, cursor locking, zoom and pan, signal
grouping, extracting a bit range of a signal into a separate displayed signal,
and per-signal value radix selection.

A bit range is *extracted* rather than dialled in. The window reaches one by
unpacking the signal, removing the bits that are not wanted and, when the range
should read as one value, grouping what is left — each step being a thing the
reader can already do to any row, and the result being a row that shows the
range on its own line (10.7). A dialog asking for `high:low` was the first way
this worked and has been withdrawn: it was a second way to reach the same place,
it could only ever produce a contiguous range, and it put a number between the
reader and a thing they can point at. The model keeps the operation, because the
control language's `add wave` takes a range as text (8.1) and will call it.

Defaults, as specified: multi-bit values display in **hexadecimal**; single-bit
values display as a line high or low.

What the mouse and the keyboard do with all of this is section 10.7.

### 10.4 Debugging features

The waveform is the headline feature but not the whole product. Also required:
source stepping, breakpoints on HDL source lines, run/continue/step controls,
and `examine`, `force` and `deposit` on signals from both the GUI and Tcl. These
are what make a simulator usable for debugging rather than merely for watching.

**These operations live on the kernel, not in the control layer.** The GUI does
not go through Tcl and VPI will not either, and a second implementation of
"write a signal from outside" would be a second set of semantics to keep in
step. A Tcl command's job is to parse its arguments and call one of them (8.1).

**Position is carried on EIR-B, and as a line table on the bytecode.** Every
EIR instruction records the statement it came from; codegen turns that into a
table with one entry per *change* of location, sorted by address. The bytecode
instruction the VM executes gains nothing, so the interpreter's inner loop
never touches it and a JIT keeps the same table against its own addresses. The
granularity is the statement, because a source-line debugger stops on lines and
a per-expression table would be larger and no more useful.

**Debugging costs a stopped simulation nothing.** The execution path tests one
boolean that is false whenever no breakpoint exists and no step is pending;
breakpoints are resolved to addresses when they are *set*, not searched for
when they might fire. That is what keeps 13.4 intact while a line breakpoint
exists at all.

**A breakpoint stops before its line runs**, so continuing executes that line
exactly once and the line reported is the one about to run rather than the one
just finished. The paused process goes back to the *head* of the region it came
from, so a resumed run produces what an uninterrupted one would have — which is
3.8, and would not be true of re-queueing it behind whatever was waiting.

**A breakpoint on a line that compiled to no instructions is refused**, rather
than recorded as one that can never fire. A marker in the source window that
silently does nothing is worse than being told the line has no code on it.

A step runs until the source *line* changes, not until the next instruction: a
statement compiles to a run of instructions, and stepping through them one at a
time would be stepping through something the user never wrote.

**A front end asks the kernel for source text; it MUST NOT read the file
itself.** The kernel is the end that knows what it analysed, and it may be on
another machine entirely (2.2). A window that opened the path would show
whatever is at that path on the machine running the window, which is either the
same file or a quietly different one — and the second case is a debugger
pointing at the wrong line with no way to tell.

**Where a run stopped travels with the fact that it stopped.** The two are one
fact, and a client that learned them from separate messages could draw its
current-line marker against a run state it no longer holds.

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

**Error codes are permanent.** A code appears in user documentation, in
suppression lists in users' scripts, and in bug reports, so reusing a number for
a different meaning silently changes what a user's suppression suppresses. A code
is retired by deleting it and never reusing the number; codes are not renumbered.
Numbers are allocated in ranges by the component that emits the diagnostic — 1–999
for the driver and source handling, 1000–1999 for the SystemVerilog frontend,
2000–2999 for VHDL, 3000–3999 for elaboration, 4000–4999 for codegen, 5000–5999
for the runtime, 6000–6999 for the control layer, 7000–7999 for the waveform
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
is the expected case, since it is consumed as a pinned fork rather than as a
released library.

Adding a dependency requires: an entry in this section, a `find_package` path
with a good error message, a `FetchContent` fallback, CI installation, and a
license check against section 1.4.

### 11.3 Current dependencies

| Dependency | Purpose | Acquisition |
|---|---|---|
| slang | SystemVerilog frontend | system, FetchContent fallback |
| NVC | VHDL frontend (M6) | vendored submodule, pinned |
| Tcl 8.6+ | control language | system |
| Qt 6 | GUI (M2) | system |
| GoogleTest | unit tests | system, FetchContent fallback |
| LLVM | JIT backend (M8, optional) | system, off by default |
| zlib | FST compression | system |

### 11.4 Build configurations

`Debug`, `Release`, `RelWithDebInfo`, plus an `ASan+UBSan` sanitiser
configuration used by CI. A `TSan` configuration is added when threading
arrives.

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
│                             (vpi_user.h, vhpi_user.h, svdpi.h, plugin ABI)
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
│   ├── control/              Tcl interpreter, commands, file lists
│   ├── ipc/                  kernel <-> GUI protocol
│   ├── pli/                  VPI and VHPI
│   └── gui/                  Qt 6 application code
├── tools/
│   ├── enzan/                headless kernel CLI
│   └── enzan-gui/            Qt 6 front end
├── tests/
│   ├── unit/                 GoogleTest, mirrors src/ layout
│   ├── golden/               self-checking HDL cases and expected output
│   ├── conformance/          harnesses for sv-tests, ivtest, VHDL suites
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

### 14.4 Conformance suites

Run against public suites and **track the pass rate as a number over time**:
`sv-tests` (CHIPS Alliance) for SystemVerilog, `ivtest` for Verilog, and the
GHDL/NVC VHDL suites plus VUnit and OSVVM for VHDL. A published compliance
percentage is also a credible public signal of maturity.

**The pass rate MUST NOT regress.** This is a ratchet: a change that lowers it
is a defect unless the regression is explicitly justified.

### 14.5 Design regression

Real designs, fetched at pinned revisions:

- SystemVerilog: picorv32, VexRiscv (generated), Ibex, CVA6.
- VHDL: NEORV32, plus the VUnit and OSVVM suites.

Start with the small ones and add larger designs as capability grows.

### 14.6 cocotb regression

Once VPI lands (M4), cocotb's own regression suite runs in CI and its pass rate
is tracked like any other conformance number.

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
cycles/sec per commit. A regression beyond an agreed threshold fails the
nightly job.

---

## 16. Continuous integration

**GitHub Actions.**

Per-commit jobs:

- Build matrix: {GCC, Clang} × {Debug, Release}.
- Unit and golden test suites.
- Sanitiser build (ASan+UBSan) over unit and golden suites.
- `clang-format` and `clang-tidy` checks.

Nightly jobs:

- Conformance suites, publishing pass rates.
- Design regression suite.
- Benchmark run, publishing cycles/sec.

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
`control`, `wavedb`, `pli`, `runtime`, `codegen`, `test`, `bench`, `fix`,
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

Placed here, between the GUI and the control layer, for three reasons. It is
contained entirely in the components M2 has just built, while that code is still
warm. Nothing later blocks on it, so putting it first costs no other milestone
anything — while M3's command vocabulary will want a settled notion of the
active cursor and the selected signal, which is an argument for fixing them
before the commands are written against them rather than after. And design
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

### M3 — Control layer and source debugging

Embedded Tcl with the Modelsim-style command vocabulary; `.f` and Tcl file
lists; compile-order inference; library persistence and reuse; Source window
with breakpoints, stepping, `examine`, `force` and `deposit`.

**Exit:** a design driven end to end from a script; libraries reused across
runs without re-analysis.

### M4 — VPI and cocotb

VPI subset per section 9; `vpi_user.h`; callback plumbing into the scheduler
regions; loading PLI shared objects.

**Exit:** cocotb's own regression suite runs with a recorded pass rate.

### M5 — Four-state completion and language breadth

Full four-state throughout; X-propagation; nets, drivers and resolution;
tasks, functions, packages, interfaces, generate, structs, enums, arrays;
`sv-tests` pass rate tracked.

**Exit:** picorv32 and Ibex simulate correctly against a reference.

### M6 — VHDL via NVC

NVC integration spike first, to confirm the approach in 5.2 before committing
to it; vendored pinned fork; NVC tree to EIR adapter; `std_logic` value model;
VHDL simulation cycle; VHPI subset for cocotb.

**Exit:** NEORV32 simulates; the VUnit suite runs.

### M7 — Mixed language

Boundary port mapping and value conversion; the cross-language delta semantics
of 3.5; mixed-language golden tests.

**Exit:** a SystemVerilog testbench drives a VHDL DUT and vice versa, matching
a reference simulator.

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

**Analysis** — compiling one design unit to EIR, independent of any top level.
Cacheable and reusable.

**Region** — one of the ordered phases within a SystemVerilog time slot.

**State arena** — the single contiguous allocation holding all mutable
simulation state.

**Time precision** — the global integer unit in which simulated time is
counted.

**VPI / VHPI / DPI** — VPI and VHPI are introspection and callback APIs used by
tools such as cocotb to drive a simulation. DPI is the separate mechanism for
calling C from SystemVerilog.
