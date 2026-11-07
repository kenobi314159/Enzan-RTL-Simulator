# Vendor simulation libraries: the Altera spike

What Enzan does with Altera's simulation models, measured before the work that makes it read them,
as [the Xilinx spike](vendor_libraries_spike.md) did for AMD's: which constructs stop each model,
what the VHDL component packages need, how a component reaches its Verilog model, and what a
functional-simulation netlist of a real design amounts to. Nothing vendor-made is in this
repository; everything here was run against a local installation, and *Repeating it* says how.

## Release

**Quartus Prime 25.1std.0** (Build 1129, 21 October 2025), Lite Edition, with Cyclone IV,
Cyclone 10 LP, Cyclone V, MAX 10 and Arria II device support. The sources read are in
`eda/sim_lib`: `altera_primitives.v`, `220model.v` (LPM), `altera_mf.v`, `sgate.v`,
`altera_lnsim.sv`, `cyclone10lp_atoms.v` and `cycloneive_atoms.v`, and in VHDL `220pack.vhd`,
`220model.vhd`, `altera_mf_components.vhd`, `altera_mf.vhd`, `sgate_pack.vhd`, `sgate.vhd` and
`altera_lnsim_components.vhd`. The build measured is Enzan at `0348f81`.

## The Verilog libraries

Each file holds up to 90 modules, and Enzan's analysis stops at the first construct it refuses, so
each library was preprocessed, split into one file per module and analysed a module at a time:
with `--top` naming it and `-y` over its own library and the ones it instantiates from. The ten
user-defined primitives are not counted on their own, since no primitive is a top level; each is
analysed in the cells that instantiate it, all of which pass.

| Library | Modules | Analyse |
|---|---|---|
| `altera_primitives.v` | 39 | 39 |
| `220model.v` | 33 | 28 |
| `altera_mf.v` | 90 | 72 |
| `sgate.v` | 19 | 19 |
| `altera_lnsim.sv` | 56 | 44 |
| `cyclone10lp_atoms.v` | 43 | 40 |
| `cycloneive_atoms.v` | 43 | 40 |

The first column of counts below is of the modules each construct stops first. The second counts
them again from scratch copies in which the first six constructs are stubbed out: the file names
`$fopen` is given, each `disable`, each `pmos`, `$fgetc`, `$timeformat` and `$printtimescale`.

| Construct | Stops first | With the first six stubbed |
|---|---|---|
| slang: `$fopen` of a file name held in a vector | 13 | — |
| `disable` of a named block enclosing it | 6 | — |
| the switch `pmos` | 4 | — |
| `$fgetc` | hidden | — |
| `$timeformat`, `$printtimescale` | 1 each | — |
| An event control on an expression (`@(pipe[head] or head)`) | 6 | 7 |
| A real parameter read by a process | 2 | 2 |
| A module no plain-text source defines | 7 | 7 |
| An unpacked array of `real` | 1 | 1 |

- **`$fopen` of a vector** stops every memory that can be initialised from a file: `altsyncram`,
  `altdpram`, `alt3pram`, `altera_syncram`, `lpm_ram_dq`, `lpm_ram_io`, `lpm_rom` and their kin,
  through the module each library reads Memory Initialization Files with
  (`ALTERA_MF_MEMORY_INITIALIZATION` and its copies `LPM_` and `ALTERA_LNSIM_`). It opens the file a
  vector names, `reg [2048:1]` (`[1024:1]` in `220model.v`). IEEE 1800 section 21.3.1 allows an
  integral file name, which slang 11 types as `string` and refuses; slang offers no override of a
  built-in task, and the only relaxation, `RelaxStringConversions`, relaxes every conversion between
  strings and integers.
- **Reading a Memory Initialization File** is that module converting it: it reads the `.mif` with
  `$fgetc`, 17 times, ends an iteration of its read loop by `disable` of the loop body's named
  block, 19 times, writes the words out as a `.ver` file with `$fwrite` and `$fdisplay`, and the
  memory then reads that with `$readmemh`. With those two stubbed, each reader translates.
- **Every `disable`, 65 in the libraries, names a block that encloses it**, and so ends that block
  and goes on after it: with the next iteration in the readers, whose block is the loop's body, and
  past the loop in the PLL models (`cyclone10lp_pll`, `cycloneive_pll`, `MF_cycloneiii_pll`...),
  whose block encloses the search for their counter settings, and in `altfp_mult`.
- **`pmos (o, i, 1'b0)`** is how `cyclone10lp_io_ibuf` and `cyclone10lp_io_obuf` drive their
  outputs: a switch held on, which passes `z` through where a buffer would make it `x`.
- **An event control on an expression**: `altmult_add`, `altmult_accum` and `altaccumulate` wait
  on an element chosen by a variable, `@(result_pipe[head] or head)`; behind the `$fopen` it stops
  `lpm_ram_dp` too. `MF_stratix_pll` and `MF_stratixii_pll` wait on `@((vco_out[i] && is_fast_pll)
  or ...)`, and `altpll` instantiates them at its default family.
- **A real parameter read by a process**: `altlvds_rx` and `altlvds_tx` compute a phase shift as a
  real and pass it by `defparam` to `MF_stratix_pll`, which multiplies it. X0 found the same in one
  unisim.
- **The modules nobody defines** are other families' atoms, which `altera_lnsim.sv`'s PLLs for
  Stratix V, Arria V and the 20 nm and 14 nm families instantiate; the unpacked `real` array is
  `altera_pll`'s. Neither is in this plan.
- **Cyclone 10 LP's atoms are Cyclone IV E's**, renamed: the two files differ in 52 lines once the
  family names are replaced.

The user-defined primitives are the flip-flops of `dffeas`, which every netlist register is
(`PRIM_GDFF_LOW` and kin), and the atoms' own: sequential, with an `initial` statement, edge rows
`(01)`, `(?0)`, `(x?)`, `(??)` and `(?x)`, `*`, `?` and `-`, and a `notifier` input that only a
timing check would toggle. `dffeas` connects their inputs to expressions,
`!clrn_in || !devpor_in || !devclrn_in`. All of them translate.

### Elaborated

Each module elaborated as a top level with its defaults, from the copies with the first six
constructs stubbed out, shows what elaboration adds:

| Library | Modules | Elaborate |
|---|---|---|
| `altera_primitives.v` | 39 | 39 |
| `220model.v` | 33 | 29 |
| `altera_mf.v` | 90 | 70 |
| `sgate.v` | 19 | 19 |
| `altera_lnsim.sv` | 56 | 47 |
| `cyclone10lp_atoms.v`, `cycloneive_atoms.v` | 43 each | 43 each |

**A call to a function of another instance does not elaborate when the function has a static
variable.** 16 models stop on it: `scfifo`, every `dcfifo`, `altdpram`, `alt3pram`, the `altddio`
cells and `lpm_fifo` and its kin call the functions of the `ALTERA_DEVICE_FAMILIES` (or
`LPM_DEVICE_FAMILIES`) they instantiate as `dev`, `dev.FEATURE_FAMILY_STRATIX(family)`, and
`altera_syncram` and `altera_syncram_derived` call their memory reader's `mem.ecc_parity`. The
static variable, `reg is_stratix`, becomes a reference `dev/IS_FAMILY_STRATIX.is_stratix`, which
names nothing (ENZ3011): a call is inlined into its caller, and the definition of `dev` declares a
subroutine's static variable only where its own code calls the subroutine, which it never does. The
same happens from VHDL. `altsyncram` elaborates, since each of its calls is in a parameter, which
slang folds to a constant.

## VHDL

- **The libraries analyse**, each in about a second: `lpm` from `220pack.vhd` and `220model.vhd`,
  `altera_mf` from `altera_mf_components.vhd` and `altera_mf.vhd`, `sgate` from `sgate_pack.vhd`
  and `sgate.vhd` against both, and `altera_lnsim_components.vhd`. None is VITAL.
- **The generics are of types that already cross into parameters**:

  | Package | Components | `string` | `natural` | `integer` | `positive` | `std_logic_vector` |
  |---|---|---|---|---|---|---|
  | `220pack.vhd` | 29 | 115 | 64 | | | |
  | `altera_mf_components.vhd` | 37 | 729 | 343 | 149 | 4 | 1 |
  | `altera_lnsim_components.vhd` | 21 | 1109 | | 401 | | 1 |

  The Verilog models declare them as untyped parameters with an integer or a string default.
- **A component of each package binds to its Verilog model**: a VHDL testbench instantiating
  `LPM_COUNTER`, `a_graycounter` and `generic_mux`, with the component packages as the libraries
  and the Verilog models found by `-y`, counts, counts in Gray code and selects as the models
  should. The package declares `LPM_COUNTER` in capitals, and it binds to `lpm_counter`.
  `generic_mux` declares `dout` an unconstrained `std_logic_vector` where its model has a scalar
  output; the actual constrains it to one element, which meets the scalar.
- `scfifo` from VHDL stops where it does from Verilog, on the calls into `ALTERA_DEVICE_FAMILIES`.

## A Cyclone 10 LP netlist of NEORV32

**Quartus Lite reads NEORV32's VHDL-2008.** NEORV32 at the revision the design regression uses,
inside the top the Xilinx netlist is made from (`tests/designs/vendor-xilinx/netlist/wrap.py`:
the testbench's configuration, the trace port on), synthesised for `10CL120YF780C8G`, takes
Analysis & Synthesis 2 minutes and 744 MB, and the EDA Netlist Writer's functional netlist
(`--simulation --functional --tool=questa_oem --format=verilog`) 8 seconds more. No fitter runs.

| | |
|---|---|
| The netlist | one flat module, 85 MB, `timescale 1 ps/1 ps` |
| Cell instances | 53,236, of 7 types |
| Distinct parameterisations of those cells | 1,192 (`cyclone10lp_lcell_comb` 724, `cyclone10lp_ram_block` 453) |
| Parameters set | 116,791, every one by `defparam` |

| Cell | Instances | Parameterisations |
|---|---|---|
| `cyclone10lp_lcell_comb` | 33,655 | 724 |
| `dffeas` (from `altera_primitives.v`) | 17,165 | 1 |
| `cyclone10lp_io_obuf` | 1,548 | 1 |
| `cyclone10lp_ram_block` | 453 | 453 |
| `cyclone10lp_io_ibuf` | 383 | 1 |
| `cyclone10lp_mac_mult`, `cyclone10lp_mac_out` | 16 each | 7, 5 |

- **No PLL and no encrypted cell**: the design has no clock of its own to make, and the netlist
  needs only `altera_primitives.v` and `cyclone10lp_atoms.v`.
- **Each block RAM carries its contents**, NEORV32's program included, as a `mem_init0` parameter;
  the `init_file` it also names is informational, and `cyclone10lp_ram_block` never opens it. So
  the netlist needs no Memory Initialization File, and none of the `$fopen` and `$fgetc` work.
- **The device-wide controls are `tri1` nets nothing drives**, `devclrn`, `devpor` and `devoe`,
  into inputs the atoms declare `tri1` too; Icarus says each such input "is coerced to inout".
- **Every unused output is connected to nothing explicitly**, `.cout()`, and slang warns of each
  (ENZ1002): 29,552 warnings, 26,431 of them `cout`. Enzan itself no longer reports an output left
  open; slang's own warning still does.
- **NEORV32's TRNG becomes latches** that Quartus builds from combinational loops, as Vivado's
  netlist has its ring oscillators; the program does not start the TRNG within the window.

**Icarus Verilog runs it** under the Xilinx netlist's Verilog testbench, unchanged: 380 s to
compile and 821 s to run the 160-microsecond window, about 20 clock cycles a second in 1.9 GB,
against 35 for the Xilinx netlist. The cores retire 4,831 and 50 instructions, as many as from the
Xilinx netlist, so both vendors' netlists of the one design agree under the one reference.

**Enzan analyses the netlist** with the two libraries in 20 seconds and 3 GB, and stops at the first
construct the atoms' file has that the netlist does not use (the `disable` in `cyclone10lp_pll`).
With `-y` over the atoms only the cells instantiated are read, and with `pmos` stubbed out of
them, Enzan elaborates the netlist in about 50 s and runs it at about 11 clock cycles a second in
3.7 GB. **It does not yet run it right**:

- From the first instruction, each trace's `rs2_rdata`, the register file's second read port, is
  `x` where Icarus has its value, 2,702 times nonzero. Quartus builds the register file from
  `bidir_dual_port` block RAMs, whose port B is that read port; everything else in the trace,
  times included, agrees.
- After 2.41 us both cores stop retiring, where under Icarus they stall for 110 ns, a cache
  refill, and go on. The simulation goes on without them.

Both are A1's to find; neither appears in the Xilinx netlist, whose register file is one
`RAMB36E1`, so `cyclone10lp_ram_block`, and what it asks of the simulator, is where to look first.

## A Cyclone V netlist of the same design

For what a netlist of a family whose atoms wrap encrypted cores reaches: 43 of `cyclonev_atoms.v`'s
68 atoms wrap a core that `sim_lib/mentor/` ships only encrypted, `cyclonev_clkena`,
`cyclonev_fractional_pll` and the DSP block `cyclonev_mac` among them. The same design synthesised
for `5CEFA9F23C8` (75 s, 717 MB) gives an 81 MB netlist of 42,717 cells and 3,033
parameterisations: `cyclonev_lcell_comb` (24,177), `dffeas` (15,928), `cyclonev_io_obuf` and
`cyclonev_io_ibuf`, `cyclonev_ram_block` (673), all plain, and **8 `cyclonev_mac`**, NEORV32's
multipliers, whose core is encrypted. **No `cyclonev_clkena`**: the fitter places clock buffers,
and a functional netlist is written before it runs. So a Cyclone V netlist is out of reach as soon
as the design multiplies in DSP blocks, and within reach, as far as its cells go, when it does not.

## Repeating it

The scripts are not kept here; each is a few lines.

- **The Verilog libraries:** `iverilog -E` over each file, which expands its macros and resolves
  its conditionals; split the result into a file per module, each starting with the `timescale`
  and the `$unit` imports before it, and `altera_lnsim.sv`'s packages into a file of their own;
  then for each module, `enzan analyse --top <module> -f <list>`, or `enzan elaborate <module>`,
  where the list holds `-y` over the split directories of its library and of those it
  instantiates from (`altera_primitives` for the atoms and `altera_mf`, `220model` and `altera_mf`
  for `sgate`), `+libext+.v+.sv` and the module's file. Tabulate the first `error:` line of each.
- **The VHDL:** `enzan lib create` a library per component package and `enzan analyse` it; a
  testbench using the packages runs with `--in-lib` for each and a list with `-y` over the split
  Verilog libraries.
- **The netlist:** a Quartus project of `rtl/file_list_core.f` and `wrap.py`'s top in a library
  `neorv32`, `VHDL_INPUT_VERSION VHDL_2008`, `EDA_GENERATE_FUNCTIONAL_NETLIST ON`, then
  `execute_module -tool map` and `execute_module -tool eda -args "--simulation --functional
  --tool=questa_oem --format=verilog"`; Icarus with `-g2012`, the netlist, `altera_primitives.v`,
  `cyclone10lp_atoms.v` and the Xilinx netlist's testbench, which fits it unchanged. Cyclone V's
  is the same project with `FAMILY "Cyclone V"`.
