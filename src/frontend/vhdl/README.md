# `src/frontend/vhdl` — the VHDL frontend

Turns VHDL source text into EIR, and nothing else (DESIGN_GUIDE.md section 5), on top of NVC's
analysis (section 5.2). Built only with `-DENZAN_ENABLE_VHDL=ON`.

## Responsibility

- Drive NVC's analysis over the sources — lexing, parsing, semantic checking, simplification
  and bounds checking — into NVC's units in a library, and translate NVC's diagnostics into
  Enzan's diagnostic engine (ENZ2001–ENZ2004).
- Walk NVC's analysed trees and emit EIR, one unit per entity, architecture, set of generic
  values and set of bounds its unconstrained ports take, folding generics as it goes (section
  5.2), from the top down the hierarchy.

## How NVC is run

**In a child process, one per analysis.** NVC keeps its state in globals for the life of a
process, and ends the process on a fatal error, a library a design names and nobody built among
them. The child is forked, runs NVC, and writes each diagnostic back to the parent through a pipe
as NVC reports it; the parent reports them as though NVC had run in it. A child that NVC ends
before the analysis finishes is reported as ENZ2004 with the message NVC printed on its way out.

**From Enzan's buffers.** Each source is handed to NVC as the text the source manager already
holds, rather than as a file for NVC to open, so a location in a diagnostic is a location in the
same buffer every other diagnostic quotes.

**With its constant evaluator replaced**, by `third_party/nvc-analysis`, so analysis folds
nothing through NVC's JIT and this adapter folds as it translates. `docs/nvc_integration.md`
measures what that changes.

**Translating in the same child.** `analyse` runs the analysis and then the translation in one
child, and the child sends the design back in EIR's own encoding: NVC's trees never reach the
parent. A design's work library is NVC's in-memory one, written nowhere.

## Files

- `VhdlFrontend.h/.cpp` — the interface the rest of Enzan sees, and the child process.
- `NvcAnalysis.h/.cpp` — NVC set up for one run: its analysis, and its locations in Enzan's
  buffers.
- `Translator.h/.cpp` — the hierarchy walk: which architecture an entity is bound to, generics
  bound and folded, one definition per architecture and values, ports, signals and instances.
- `BodyTranslator.h/.cpp` — a process's or a subprogram's statements and expressions, as EIR-B,
  and calls: callable, or inlined.
- `Folder.h/.cpp` — static evaluation: generics, constants, bounds and delays.
- `Types.h/.cpp` — how a VHDL type is held in EIR (section 3.6).

## What is translated

So far: entities, architectures, entity instances, component instances bound by default or by
configuration specifications, generics of scalar and array types, null strings among them; signals
and ports of enumeration, integer, physical and real types, of arrays of them, of one or more
dimensions, with static bounds, and of records; ports of unconstrained array types of one
dimension, which take their bounds from their actuals; ports associated with signals, parts of them,
expressions and conversion functions, whole or in parts; `if` and `for` generate statements and
blocks, with generics and ports; constant functions; processes with variables, `if`, `case`, loops,
`exit`, `next`, `wait` with a sensitivity, a condition, a timeout or any of them together, signal
assignments with inertial and transport delay, to whole signals, to their parts and to aggregates of
them, as variable assignments may be too, `assert`, `report` with messages built from any strings
and characters and the `'image` of integers and times, `std.env.finish` and `stop`; aggregates,
strings, elements and slices, with loops unrolled for slices that move with their parameter and
variable slices of run-time lengths, concatenation, the predefined operators on scalars and arrays,
`'event`, `'active` and `'last_value`; functions and procedures of packages and architectures,
std_logic_1164's and numeric_std's among them. std.textio's `write`, `writeline`, `readline` and
`read` of lines of strings, characters and integers, files of text and of other types and `endfile`,
run natively by the runtime. Anything else is refused with ENZ2005 naming it, never left out.

## Rules

- **NVC's types stay in this directory.** Only `NvcApi.h` includes NVC's headers, and only files
  here include it (section 17.4); `VhdlFrontend.h` is the whole of what the rest of Enzan sees.
- **NVC's headers are the C++ copies** that `third_party/nvc-analysis` generates, never the
  originals, which C++ rejects.

## Allowed dependencies

`common` and `eir`, as every frontend (section 5.3), and NVC's analysis (`enzan::nvc`).
