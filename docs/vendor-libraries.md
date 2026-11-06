# Vendors' simulation libraries

Enzan simulates designs that instantiate an FPGA vendor's own simulation models, read from your
installation of the vendor's tools. Nothing vendor-made comes with Enzan: the models are under the
vendor's licence, so you point Enzan at the copy you have.

## AMD Xilinx, from SystemVerilog and Verilog

The unisims, `glbl` and the XPM macros are read straight from Vivado's sources; no library has to be
built first. A file list such as this one names them, with `XILINX_VIVADO` the Vivado directory:

```
# xilinx.f
-y ${XILINX_VIVADO}/data/verilog/src/unisims
+libext+.v
${XILINX_VIVADO}/data/verilog/src/glbl.v
${XILINX_VIVADO}/data/ip/xpm/xpm_cdc/hdl/xpm_cdc.sv
${XILINX_VIVADO}/data/ip/xpm/xpm_memory/hdl/xpm_memory.sv
${XILINX_VIVADO}/data/ip/xpm/xpm_fifo/hdl/xpm_fifo.sv
```

```
enzan --top tb --top glbl -f xilinx.f tb.sv design.sv
```

- **`glbl` is a top level of its own**, beside your testbench, as in every simulator: the models
  reach its reset and tristate by name, and without `--top glbl` the run stops at the first, a
  reference to `/glbl/GSR` that names nothing. The reset, `GSR`, holds every flip-flop at its
  initial value for the first 100 ns.
- **`-y` finds each model a design instantiates** in the directory, by its module name, and reads
  only those. With `--out-lib`, what it found is stored there with the rest.
- **The XPM FIFOs that speak AXI** need the interfaces Vivado keeps beside its own directory, in
  `${XILINX_VIVADO}/../data/rsb/busdef/vivado_interfaces.svh`, given before `xpm_fifo.sv`.
- **The models' clock-to-output delays are run**, 100 ps on most of them, as their module paths
  give them; a pulse shorter than a path's delay reaches the output, as the models ask with
  `PATHPULSE$ = 0`. Timing checks are not run, so a notifier never toggles, and the run says so
  once.
- **An initial value is no change.** A model waiting for its inputs to change, as the LUTs do, does
  not see a value a testbench gave a variable where it declared it, and so shows `x` until an input
  first changes, as IEEE 1800 says. Assign the inputs in an `initial` block instead, or drive them
  from the design.

The release tested is the one `tests/designs/vendor-xilinx/release.txt` names. That harness runs
Enzan's testbenches of the commonest primitives, against Icarus Verilog, when `XILINX_VIVADO` is
set:

```
XILINX_VIVADO=/opt/Xilinx/Vivado/2026.1 tests/designs/vendor-xilinx/run.sh enzan iverilog work/
```

### Not yet

- The models from VHDL, through the `unisim` and `xpm` component libraries.
- Functional-simulation netlists written by Vivado. A netlist is ordinary Verilog over the unisims,
  so a small one may work; none has been held to a reference yet.
- A few UltraScale+ and Versal primitives, `RAMB18E5` and `RAMB36E5` among them, whose module paths
  differ from bit to bit of an output: Enzan refuses them by name.
- The `secureip` models, which are encrypted, and SDF back-annotation.

## Altera

Not yet: the Altera libraries come after AMD's.
