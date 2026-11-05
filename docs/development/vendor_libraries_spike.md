# Vendor simulation libraries: the Xilinx spike

What Enzan does today with AMD Xilinx's simulation models, measured before the work that makes it
read them: which constructs stop each model, what the VHDL component packages need, how generics
cross into the Verilog models, how module paths behave in the reference simulators, and what a
functional-simulation netlist of a real design amounts to. Nothing vendor-made is in this
repository; everything here was run against a local installation, and *Repeating it* says how.

## Release

**Vivado 2026.1** (SW Build 6511674, 16 June 2026), with 7-series device support. The sources
read are `data/verilog/src` (`unisims`, `unimacro`, `unifast`, `retarget`, `glbl.v`),
`data/ip/xpm`, `data/vhdl/src/unisims` and `data/vhdl/src/unimacro`, and
`data/rsb/busdef/vivado_interfaces.svh`. The build measured is Enzan at `d81245a`.

## The Verilog unisims

Each of the 501 models (496 `.v`, 5 `.sv`) was analysed on its own. `glbl.v` itself is refused,
for its `tri (weak1, strong0)` and `assign (strong1, weak0)` drivers, and a model whose `glbl`
fails reports nothing of its own, so each was analysed beside a copy of `glbl.v` with the
strengths removed. **152 analyse.** A model is translated until its first unsupported construct,
so the table counts the models each construct stops, and a later construct in the same model
stays hidden until the earlier one is supported.

| Construct | Models it stops | Models it alone stops |
|---|---|---|
| A module the sources do not define | 144 | 143 |
| A module path with a delay | 97 | 71 |
| A gate primitive | 66 | 54 |
| A `real` variable, or another type that is not packed integral | 23 | 10 |
| A delayed write to part of a signal (`assign #1 v[0] = ...`) | 16 | 14 |
| Real arithmetic, `$time/1000.0` in a `$display` | 9 | 6 |
| A procedural `assign`/`deassign` | 8 | 8 |
| A user-defined primitive | 5 | 3 |
| A drive strength | 5 | 3 |
| slang: a parallel path between terminals of different widths | 4 | |
| slang: a field width on `%m` | 2 | 2 |
| `$sformat` | 2 | |
| `$fscanf`, `NamedValue` of a real, separate rise/fall/turn-off delays | 1 each | |

- **The modules nobody defines** are 153 names. 26 are other unisims, which `-y` over the
  directory would find: `PLLE2_BASE` instantiates `PLLE2_ADV`, the `DSP58` family its `DSP_*`
  blocks, and so on. The other 127 are the encrypted `secureip` cores (`SIP_*`, `B_*`) and
  Versal's behavioural NoC and memory-controller models (`BM_*`), which no plain-text source
  defines.
- **A procedural `assign`/`deassign` is how the flip-flops apply GSR**:
  `always @(glblGSR) if (glblGSR) assign Q_out = INIT; else deassign Q_out;`. 45 models use it
  outside `XIL_TIMING`; 8 show it first. 7 use `force`, as `xpm_memory` does (below), which slang
  represents as the same statement.
- **`LUT1`, `LUT2` and `LUT3` are user-defined primitives** (`x_lut1_mux2` and kin), as are the
  latches.

The cells a netlist and an RTL design use most, as they stand:

| Model | First blocked by |
|---|---|
| `LUT4`, `LUT5`, `LUT6`, `CARRY4`, `CARRY8`, `BUFGCE`, `BUFGCTRL` | nothing: they analyse |
| `FDRE`, `FDCE`, `FDPE`, `FDSE` | procedural `assign`/`deassign` |
| `LUT1`–`LUT3` | a user-defined primitive |
| `SRL16E`, `SRLC32E`, `RAMB18E1`, `RAMB36E1`, `RAMB36E2` | module paths with a delay |
| `DSP48E1`, `IDDR`, `ODDR` | gate primitives and module paths |
| `DSP48E2` | real arithmetic |
| `MMCME2_ADV`, `MMCME4_ADV`, `PLLE2_ADV` | `real`, and module paths |
| `BUFG`, `IBUF`, `OBUF`, `IOBUF`, `PULLUP`, `PULLDOWN`, `KEEPER`, `INV` | gate primitives |

`unifast` (5 models) is stopped by the same constructs, or by the encrypted `B_*_FAST` cores.

## unimacro

The Verilog macros default to `DEVICE = "VIRTEX5"`, whose primitives (`DSP48E`, `RAMB18`,
`FIFO36`...) the release no longer has, so they are only meaningful with `DEVICE = "7SERIES"`.
So set, with the unisims they instantiate added as `-y` would add them, **every macro translates**,
and each is stopped only by its unisims: `DSP48E1` (gate primitives, module paths) for the
arithmetic and counter macros, `RAMB18E1`, `FIFO18E1` and `FIFO36E1` (module paths) for the
others.

## XPM

- **`xpm_cdc` analyses**, beside a `glbl`. Its assertions are generate branches that
  `SIM_ASSERT_CHK = 0`, the default, rules out, so none is elaborated.
- **`xpm_memory`, and `xpm_fifo` through it, stop first on a procedural `force`**, which models a
  write collision by forcing the data to `X`, in a branch every common-clock memory of equal widths
  takes; and on real arithmetic, `$time/1.0`, in the clock-period measurement. What lies behind
  them is not yet known.
- **Assertions:** `xpm_memory` has 134 concurrent assertions, `xpm_fifo` 12 and `xpm_cdc` 11.
  `DISABLE_XPM_ASSERTIONS` removes all of `xpm_fifo`'s and leaves 62 of `xpm_memory`'s, which are
  generate branches that `MESSAGE_CONTROL = 1` selects; at the default of 0, none is elaborated.
  Enzan refuses a concurrent assertion by name, so a design leaving either setting on is told so.
- **`glbl` has to be a top level.** With one top, slang reports every `xpm_cdc` module the design
  does not instantiate, since its `glbl.GSR` cannot resolve; a top named `glbl` is what makes it
  analyse at all.
- `xpm_fifo_axis` and kin need `vivado_interfaces.svh`, which slang reads.

## VHDL

- **`unisim_VCOMP.vhd` overflows NVC's arena for one unit**: "memory exhausted while creating unit
  VCOMPONENTS", at NVC's default of 16 MB. 32 MB is not enough; 64 MB is, and the package then
  analyses in about a second into a 2.1 MB library. NVC sets the size with `-M`; Enzan does not
  expose it, so it has to set it.
- **The package to build `unisim` from is `unisim_retarget_VCOMP.vhd`**, plain text in this
  release beside the encrypted `.vhdp`. It declares 1364 components: `unisim_VCOMP.vhd`'s 501
  and the 863 legacy names of `retarget_VCOMP.vhd`. The VHDL `unimacro` names legacy components
  in branches for older devices, and VHDL needs every component declared whichever branch is
  taken, so against `unisim_VCOMP.vhd` it fails, and against the retarget package **all 12 macros
  analyse**. The Verilog `retarget` directory (871 models) is what those legacy names bind to.
- `xpm_VCOMP.vhd` analyses.

## Generics across the boundary

The VHDL components declare their generics with VHDL types; the Verilog models declare the
parameters of the same names in a style of their own. A VHDL testbench instantiating components
of `unisim` bound to the Verilog models, and one instantiating a stand-in with the remaining
styles, gave:

| VHDL generic | Verilog parameter, as the unisims declare it | Crosses |
|---|---|---|
| `bit_vector` (`LUT6`'s `INIT`) | `parameter [63:0] INIT` | yes |
| `std_logic_vector` (`LUT6CY`'s `INIT`) | `parameter [63:0] INIT` | yes |
| `string` | an untyped parameter with a string default | yes |
| `integer` | `parameter integer` | yes |
| `bit` | `parameter [0:0]` | yes |
| `real` | `parameter real` | accepted; the Verilog side cannot read a real yet |
| `boolean` (`IBUFDS`'s `DIFF_TERM`) | **an untyped parameter with the default `"TRUE"`** | **no: it arrives as `""`** |

Every unisim declares a boolean that way, and checks its value, so `IBUFDS` from VHDL reports an
attribute error and calls `$finish`, whether the generic is given or left to its default. The
component's defaults are passed across too, so leaving a generic out does not avoid it.

**An initial value makes no event at time zero**, so a model that evaluates only in
`always @(I0 or I1 ...)`, as `LUT6` does, reads `X` until an input changes when its inputs hold
values given in their declarations and nothing drives them. This is IEEE 1800's rule for a
variable's declaration assignment, and the boundary keeps it: a VHDL signal declared `:= '1'`
reaches the model as a variable so initialised would. Vendors' simulators take the Verilog-2001
reading instead, in which the initialisation is an event:

| `LUT6` whose inputs are variables declared `= 1`, read at 1 ns | |
|---|---|
| Enzan, from SystemVerilog or from VHDL | `x` |
| Icarus Verilog, `-g2012` | `x` |
| Icarus Verilog, `-g2005`; xsim | `1` |

In a netlist, a cell's inputs come from flip-flops, which GSR resets, or from `VCC` and `GND`, which
are continuous assignments, so each sees an event; a testbench driving a model from initial values
alone does not.

## Module paths and pulses

164 unisims have 100 ps clock-to-output paths outside `XIL_TIMING`, and nearly all set
`specparam PATHPULSE$ = 0`, which by IEEE 1800 section 30.7 lets a pulse shorter than the delay
through. A 30 ps pulse into a cell with a 100 ps path:

| | default pulse limits | `PATHPULSE$ = 0` |
|---|---|---|
| Icarus Verilog 13, `-gspecify` | filtered | filtered |
| xsim (for information) | filtered | passed, 1100 to 1130 ps |

Icarus ignores `PATHPULSE$`. Where the two differ, Icarus is no reference: a test of a pulse
shorter than a path checks the standard's values itself.

## A 7-series netlist of NEORV32

NEORV32 at the revision the design regression uses, with its testbench's generics but the TRNG
(free-running ring oscillators, which loop at zero delay in a functional netlist) and the
simulation trace log off, synthesised out of context for `xc7a200tfbg676-2`, takes Vivado 156 s
and gives a Verilog netlist of 13 MB and a VHDL one of 16 MB.

| | |
|---|---|
| Modules the netlist defines | 221, each instantiated once |
| Cell instances | 41,355, of 21 types |
| Distinct parameterisations of those cells | 2,306 (`LUT6` 1,439, `LUT5` 512, `LUT4` 250, `LUT3` 70) |
| Of cells that read `glbl` (`FD*E`, `CARRY4`, `RAMB*`, `DSP48E1`) | 17, or 43 told apart by depth |
| Hierarchy depth | 1 to 6 |

So the netlist would be about 2,300 analysed units for its cells and 221 for its modules, against
41,355 instances. A reference from a cell to `glbl` is a path counting the cell's depth while it is
relative, which is what splits 17 parameterisations into 43.

**Icarus Verilog runs it** with `-gspecify`, `-y` over the unisims and `glbl` as a second top:
33 s to compile, about 17 s to load, and then about 2.8 s per simulated microsecond at 100 MHz,
some 35 clock cycles a second. A UART character at 19,200 baud takes about half a millisecond, so
a reference run that prints anything takes tens of minutes.

The repository's Verilog testbench for NEORV32 cannot drive the netlist under Icarus: it wraps
NEORV32's VHDL simulation models (the UART receivers, the XBUS memories, the PSRAM). The run above
used a testbench of its own, with every input idle.

## Repeating it

The scripts are not kept here; each is a few lines.

- **The unisims:** for each model, `enzan lib create` a scratch library and `enzan analyse
  --out-lib` it with the strength-free `glbl` (`sed -E "s/\((strong|weak)[01], *(strong|weak)[01]\) *//"`
  over `glbl.v`), keeping the transcript; tabulate the `error:` lines.
- **`-y` by hand:** analyse with `--top`, add the file of every `unknown module` the transcript
  names from `unisims/`, and repeat until none is new.
- **XPM:** a top module named `glbl`, holding `glbl.v`'s declarations and the XPM instances, with
  `vivado_interfaces.svh`, `xpm_memory.sv`, `xpm_fifo.sv` and `xpm_cdc.sv`.
- **The netlist:** Vivado's `read_vhdl -vhdl2008 -library neorv32` over `rtl/file_list_core.f`,
  `synth_design -mode out_of_context -generic {...}`, then `write_verilog -mode funcsim` and
  `write_vhdl -mode funcsim`; Icarus with `-g2012 -gspecify -y <unisims> -s <testbench> -s glbl`.
