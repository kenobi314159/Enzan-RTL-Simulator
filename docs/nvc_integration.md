# NVC integration

How Enzan uses NVC as its VHDL frontend: which revision, which files, how they are driven, and
what the M6 spike measured before the milestone committed to the boundary of DESIGN_GUIDE.md
section 5.2. The guide is normative; this file records what was found and how to repeat it.

## Revision

NVC **r1.23.0** (`cba10a2`, 19 September 2026), upstream rather than a fork: nothing below needs a
change to NVC's sources. The next upgrade repeats the measurements in this file against the new
revision before the pin moves (see *Upgrading*).

## The boundary

Enzan uses NVC's lexer, parser, semantic analysis, simplification and bounds checking, and
consumes the `tree_t` each design unit leaves behind. It does not use NVC's elaboration, lowering,
code generation, JIT, runtime or scheduler (section 5.2).

Analysing a file is the sequence NVC's own `nvc -a` runs, and nothing else:

```c
term_init();
thread_init();
set_default_options();
intern_strings();
set_standard(STD_08);
lib_add_search_path(<directory holding std, ieee and nvc>);
lib_set_work(lib_new("<name>:<directory>"));

input_from_file(path);
while ((unit = parse()) != NULL) {        // one design unit at a time
    lib_put(work, unit);
    simplify_local(unit, jit, registry, mir);
    bounds_check(unit);
}
lib_save(work);
```

## NVC's constant evaluator is most of NVC

`simplify_local` folds every locally static function call and type conversion through
`eval_try_fold`. NVC implements that by lowering the expression through its MIR and running it on
its JIT interpreter, so analysis as NVC builds it links its lowering, the JIT (without LLVM), part
of its runtime model, its VHPI and VPI object models, its coverage database and `libffi`. Linked
statically against `libnvc.a`, a driver doing nothing but the sequence above pulls in **80 of the
102 objects, about 142,000 of NVC's 172,000 lines of C**. That is not the small subset 5.2
counted on, and it is exactly the path 5.2 says Enzan does not use.

**The seam is three functions.** `simp.c` reaches the evaluator only through `eval_possible`,
`eval_try_fold` and `eval_must_fold`. Enzan does not compile `eval.c` and supplies those three
itself; with `eval_possible` answering no, analysis needs **34 objects, about 66,000 lines**, and
none of the lowering, JIT, runtime or `libffi`. No NVC source changes.

What that costs was measured on NEORV32 (61 files, 160 design units). NVC's evaluator folds 159
expressions during its analysis: 135 implicit type conversions, such as the bounds of `for i in 0
to 31 loop`, and 24 function calls. Without it the same 160 units analyse with no errors, in under
a second either way. The trees therefore reach the adapter with those expressions unfolded, and
the adapter folds them as it translates. A static bound error that NVC would have reported during
analysis by folding is left for Enzan to find when it folds.

### The files

From `src/`: `bounds.c`, `common.c`, `debug.c`, `diag.c`, `fbuf.c`, `hash.c`, `ident.c`,
`lexer.l`, `lib.c`, `mask.c`, `names.c`, `object.c`, `option.c`, `parse.c`, `printf.c`, `scan.c`,
`sem.c`, `simp.c`, `thread.c`, `tree.c`, `type.c`, `util.c`, `psl/psl-node.c`, `psl/psl-sem.c`,
`rt/mspace.c`, `sdf/sdf-parse.c`, `sdf/sdf-util.c`, `vhdl/vhdl-inst.c`, `vhdl/vhdl-util.c`,
`vlog/vlog-node.c`, `vlog/vlog-number.c`, `vlog/vlog-parse.c`, `vlog/vlog-symtab.c`,
`vlog/vlog-util.c`. From `thirdparty/`: `cpustate.c`, `fastlz.c`, `sha1.c`.

The PSL, SDF and Verilog files are there because the parser and the library reference them: PSL
is part of VHDL-2008's syntax, and NVC's library format holds Verilog units beside VHDL ones.

`lexer.l` is a flex source. NVC's build runs flex on it, and so must any build of this subset.

## The standard libraries

NVC ships its own sources for `std`, `ieee` and `nvc`. The 2008 `ieee` bodies are not standalone:
`numeric_std` and `numeric_bit` use `nvc.sim_pkg`, and `std.textio`'s body uses `nvc.polyfill`, so
the `nvc` library is built with them. Every analysis also loads `nvc.psl_support` of its own accord,
and stops with a fatal error when it is missing.

With the evaluator replaced, the subset analyses from source everything NEORV32 needs: all of
2008's `std`, `std.standard` in NVC's bootstrap mode; `nvc`'s `sim_pkg`, `polyfill`, `text_util`,
`psl_support`, `random` and `cover_pkg`; and `ieee`'s `std_logic_1164`, `numeric_std`,
`numeric_bit`, `math_real`, `numeric_std_unsigned`, `std_logic_textio` and `ieee_std_context`. Each
takes a fraction of a second, and NEORV32 then analyses against those libraries with no error, so
nothing in the chain needs NVC's own build.

**Each `ieee` file is analysed by a separate run**, as NVC's own build does. Analysing several of
them in one run reports eight errors in `numeric_std`'s body, eight `?=` and `?/=` operators on
`std_ulogic` that do not resolve. NVC's own `nvc -a` analyses the same files in one run without
error, and the evaluator is not the cause, since a driver with NVC's evaluator linked fails the same
way. Something in NVC's driver that the sequence above leaves out makes the difference, and it
should be found before Enzan analyses a user's library of packages in one run. NEORV32's 61 files,
its package among them, analyse in one run without error.

## What Enzan must implement

Walking every call reachable from NEORV32's RTL and testbench, transitively through the packages
they use, finds **186 distinct callees** that NVC does not compile from VHDL as written:

| Group | Count | Enzan's work |
|---|---|---|
| `std_logic_1164` and `numeric_std` functions NVC replaces with builtins | 93 | none: each has a VHDL body, which Enzan translates; a builtin is an optimisation |
| Implicit operations of a type: comparisons, arithmetic, logical and matching operators, `minimum`, `maximum` | 85 | implemented by Enzan, in about 40 kinds, each kind once for every type |
| File operations, `endfile`, `deallocate`, `to_string` of `real`, `to_hstring`, `to_ostring` | 8 | implemented by Enzan |

No subprogram NEORV32 reaches carries a `foreign` attribute. The libraries do declare some: one in
2008's `std.env` body, behind `stop` and `finish`, 33 in 2019's, and eleven in the `nvc` library,
`sim_pkg` among them.

## What elaboration has to evaluate

NEORV32 has 77 entities, 60 of them with generics, 610 generics in all. Outside processes and
subprogram bodies — in port and signal subtypes, generate conditions and ranges, generic maps and
constants — its architectures refer to their generics 802 times, contain 209 generate statements,
and call user functions 61 times, such as `index_size_f(IMEM_SIZE)` in a subtype. Its testbench's
memory model initialises a signal from an impure function that opens a file when it is given a name,
which it is not by default.

Whichever way generics are folded (section 5.2), something has to evaluate those 61 calls with the
generics' values, and 5.2 rules out NVC's evaluator for it.

## Embedding

- **Global state.** NVC keeps its identifier table, library registry, options and diagnostic state
  in globals, so there is one analysis context per process.
- **Threads.** `thread_init` is required, and analysis starts no thread.
- **Diagnostics.** `diag_set_consumer` receives every error, warning and note as a `diag_t`, with
  its level, text, location and hints through `diag_get_text`, `diag_get_loc`, `diag_hints` and
  `diag_get_hint`, which is what translating them into ENZ2000–2999 needs (section 10.6).
- **Fatal errors end the process.** `fatal`, `fatal_at`, `fatal_errno` and `fatal_trace` detach the
  consumer, print to standard error and call `exit(1)`, which is Enzan's status for a wrong command
  line (section 8.1). An unreadable source is one such path, which Enzan's own reading of the file
  forestalls; the rest are NVC's internal errors. Containing them takes running the analysis in a
  child process, which also bounds NVC's global state to one analysis.

## Upgrading

1. Move the pin, then build NVC as its own README says, without LLVM.
2. Link a driver doing only the sequence above, with Enzan's three evaluator functions, against
   `lib/libnvc.a` with `-Wl,-Map=<file>`. The archive members the map lists as included are the
   file list; compare them with the list above.
3. Repeat the NEORV32 measurements above, and run the VHDL golden tests (section 5.2 relies on them
   to catch drift).
