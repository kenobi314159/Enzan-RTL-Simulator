# `src/frontend/vhdl` — the VHDL frontend

Turns VHDL source text into EIR, and nothing else, on top of NVC's analysis. Built unless
configured with `-DENZAN_ENABLE_VHDL=OFF`.

## Responsibility

- Drive NVC's analysis over the sources — lexing, parsing, semantic checking, simplification
  and bounds checking — into NVC's units in a library, and translate NVC's diagnostics into
  Enzan's diagnostic engine (ENZ2001–ENZ2004, and ENZ2008 for a design naming VITAL).
- Walk NVC's analysed trees and emit EIR, one unit per entity, architecture, set of generic
  values and set of bounds its unconstrained ports take, folding generics as it goes, from the top
  down the hierarchy.

## How NVC is run

**In a child process, one per analysis.** NVC keeps its state in globals for the life of a
process, and ends the process on a fatal error, a library a design names and nobody built among
them. The child is forked, runs NVC, and writes each diagnostic back to the parent through a pipe
as NVC reports it; the parent reports them as though NVC had run in it. A child that NVC ends
before the analysis finishes is reported as ENZ2004 with the message NVC printed on its way out.

**From Enzan's buffers.** Each source is handed to NVC as the text the source manager already
holds, rather than as a file for NVC to open, so a location in a diagnostic is a location in the
same buffer every other diagnostic quotes.

**In dependency order.** `SourceOrder.cpp` scans each source for the units it declares and those
of the work library it names, and NVC analyses each after the sources declaring them, in the order
given otherwise.

**With its constant evaluator replaced**, by `third_party/nvc-analysis`, so analysis folds nothing
through NVC's JIT and this adapter folds as it translates. `docs/development/nvc_integration.md`
measures what that changes.

**Translating in the same child.** `analyse` runs the analysis and then the translation in one
child, and the child sends the design back in EIR's own encoding: NVC's trees never reach the
parent. A design's work library is NVC's in-memory one, written nowhere, unless analysis writes
into an Enzan library, whose `vhdl/` directory then holds NVC's units; the libraries a design is
analysed against reach NVC by name (`UnitOptions::libraries`).

## Files

- `VhdlFrontend.h/.cpp` — the interface the rest of Enzan sees, and the child process.
- `NvcAnalysis.h/.cpp` — NVC set up for one run: its analysis, and its locations in Enzan's
  buffers.
- `SourceOrder.h/.cpp` — the order sources are analysed in: each after those declaring the units
  it names.
- `Translator.h/.cpp` — the hierarchy walk: which architecture an entity is bound to, generics
  bound and folded, one definition per architecture and values, ports, signals and instances.
- `BodyTranslator.h/.cpp` — a process's or a subprogram's statements and expressions, as EIR-B,
  and calls: callable, or inlined.
- `Folder.h/.cpp` — static evaluation: generics, constants, bounds and delays.
- `Textio.cpp` — std.textio and VHDL's files, run natively by the runtime.
- `Strings.cpp` — strings whose lengths only the run knows, held in the runtime's scratch.
- `Types.h/.cpp` — how a VHDL type is held in EIR.

## What is translated

So far: entities, architectures, entity instances, component instances bound by default or by
configuration specifications, or with no entity to bind to the SystemVerilog module of their name,
which the instance asks the session for (`eir::UnitRequest`); entities SystemVerilog instantiates,
described as `eir::UnitInterface`s and translated for the values it asks for; generics of scalar, array and record types, null strings among them;
deferred constants; signals and ports of enumeration, integer, physical and real types, of arrays of
them, of one or more dimensions, with static bounds, and of records; ports of unconstrained array
types of one dimension, which take their bounds from their actuals; ports, inout ones among them,
associated with signals, parts of them, expressions and conversion functions, whole or in parts;
`if` and `for` generate statements and blocks, with generics and ports; constant functions;
processes with variables, `if`, `case`, loops, `exit`, `next`, `wait` with a sensitivity, a
condition, a timeout or any of them together, signal assignments with inertial and transport delay,
to whole signals, to their parts and to aggregates of them, as variable assignments may be too,
`assert`, `report` with messages built from any strings and characters and the `'image` of integers
and times, `std.env.finish` and `stop`; aggregates, strings, elements and slices, with loops
unrolled for slices that move with their parameter and variable slices of run-time lengths,
concatenation, the predefined operators on scalars and arrays, `'event`, `'active` and
`'last_value` of whole signals; aliases of objects and of their parts, of constants, subprograms and
types; functions and procedures of packages and architectures, std_logic_1164's and numeric_std's
among them. std.textio's `write` of strings, characters and integers to a line, `writeline`,
`readline` and `read` of characters and strings from a line, the predefined `write` and `read` of
files of text and of other types, and `endfile`, run natively by the runtime. A subprogram's
strings whose lengths only the run knows are held in the runtime's scratch: their bounds, elements
and static slices, comparisons, textio's `read` into them and `write` of them, and passing them to
subprograms, as slices of run-time length of other strings are. Anything else is refused with
ENZ2005 naming it, never left out.

## Rules

- **NVC's types stay in this directory.** Only `NvcApi.h` includes NVC's headers, and only files
  here include it; `VhdlFrontend.h` is the whole of what the rest of Enzan sees.
- **NVC's headers are the C++ copies** that `third_party/nvc-analysis` generates, never the
  originals, which C++ rejects.

## Allowed dependencies

`common` and `eir`, as every frontend, and NVC's analysis (`enzan::nvc`).
