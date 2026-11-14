# Language support

What Enzan simulates of each language, and what it does not yet. Anything it does not support is
reported with its source location and the construct named, and nothing is simulated; a design is
never run with part of it left out.

## SystemVerilog and Verilog

Enzan reads SystemVerilog as IEEE 1800-2017 defines it. **A Verilog source is read as
SystemVerilog**, whatever its extension: Verilog-2005 is almost entirely a subset of SystemVerilog,
so a Verilog design runs unchanged, unless it names something with a word SystemVerilog has made a
keyword, such as `logic`, `bit`, `int` or `final`, which then has to be renamed.

### What works

- Modules, parameters and `defparam`, generate blocks, packages, interfaces and modports, and
  hierarchical references.
- `initial`, `final`, `always`, `always_comb`, `always_ff` and `always_latch`, continuous
  assignments, and tasks and functions.
- Four-state values, packed and unpacked arrays and structs of fixed size, enumerations and reals;
  streaming concatenations, `{<<{...}}` and `{>>{...}}`.
- Nets of every kind with several drivers, resolved by their kind, at strong and weak strengths;
  gates, MOS switches and user-defined primitives; specify blocks' module path delays.
- Delays, event controls, named events, `wait`, `force` and `release`, `assign` and `deassign`.
- Immediate assertions, and concurrent assertions declared in a module: `assert`, `assume` and
  `cover property` of booleans joined by fixed delays (`a ##2 b`) and consecutive repetitions
  (`b[*3]`), with one `|->` or `|=>`, `disable iff`, a default clocking block, named properties
  and sequences, and `$past`, `$rose`, `$fell`, `$stable`, `$changed` and `$sampled`.
- `$display` and its family, files written and read, `$readmemh` and `$readmemb`, `$random` and
  `$urandom`, and `$finish`, `$stop`, `$fatal`, `$error`, `$warning` and `$info`.
- VPI, as cocotb uses it.

### Not yet

- Classes, and what is built on them: UVM, `randomize` and constraints, covergroups.
- `string` variables and ports, real ports, dynamic and associative arrays, queues and unpacked
  unions.
- `fork`, a recursive function, `ref` arguments and DPI.
- Concurrent assertions beyond the subset above: a range of delays (`##[1:3]`), goto and
  non-consecutive repetitions, the property operators (`not`, `and`, `until`, `s_eventually` and
  the rest), local variables, `restrict`, and a concurrent assertion inside a procedure; deferred
  assertions (`assert #0`, `assert final`).
- An event control on an expression other than a signal or a constant select of one.
- A delayed write to part of a signal, separate rise, fall and turn-off delays on a vector, and a
  delay on a net.
- `trireg`, `interconnect`, user-defined nettypes, `highz` strengths, and the resistive and
  bidirectional switches.
- Most system functions beyond those above; any slang computes while analysing, such as `$bits`
  and `$clog2`, works.

## VHDL

Enzan reads VHDL of IEEE 1076-1993, 2000, 2002, 2008 and 2019, as `--std` says, 2008 by default.

### What works

- Entities and architectures, generics, ports of every mode, components bound by default or by a
  configuration specification, generate statements and blocks.
- Processes and every sequential statement, signal assignments with inertial and transport delay,
  resolved signals with any resolution function.
- Enumerations, integers, physical types, reals, arrays of any number of dimensions and records.
- Functions and procedures, of architectures and packages; `std_logic_1164`, `numeric_std`,
  `numeric_bit`, `math_real` and `std.env`.
- `std.textio` and files of text and of other types.
- `'image`, `'path_name` and `'instance_name` in a report, an assertion's message or a `write`.
- VHPI, as cocotb uses it.

### Not yet

- Access types beyond `std.textio`'s `line`, protected types and shared variables of them.
- Configuration declarations, external names, and generic packages and subprograms, which VUnit
  and OSVVM are built on.
- Values whose length only the run knows, beyond the strings of a subprogram.
- `'path_name` and `'instance_name` anywhere but a message or a `write`, or of what a package or a
  subprogram declares.

## Both in one design

A design may mix the two languages, either instantiating the other: see
[Mixed-language designs](mixed-language.md) for what crosses between them.
