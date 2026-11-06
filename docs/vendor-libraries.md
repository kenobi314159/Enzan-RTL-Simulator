# Vendors' simulation libraries

Enzan simulates designs that instantiate an FPGA vendor's own simulation models, read from your
installation of the vendor's tools. Nothing vendor-made comes with Enzan: the models are under the
vendor's licence, so you point Enzan at the copy you have.

## AMD Xilinx

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
  reach its reset and tristate by name, so a run without `--top glbl` stops at the first reference
  to it. Its reset, `GSR`, holds every flip-flop at its initial value for the first 100 ns.
- **`-y` reads only the models a design instantiates**, each from the file of its name.
- **The XPM FIFOs that speak AXI** need the interfaces Vivado keeps beside its own directory, in
  `${XILINX_VIVADO}/../data/rsb/busdef/vivado_interfaces.svh`, given before `xpm_fifo.sv`.
- **The models' clock-to-output delays are run**, 100 ps on most of them. Their timing checks are
  not, and a run says so once.
- **An initial value is no change.** A model waiting for its inputs to change, as the LUTs do, does
  not see a value a testbench gave a variable where it declared it, and shows `x` until an input
  first changes, as IEEE 1800 says. Assign the inputs in an `initial` block instead.

### Functional-simulation netlists

Vivado's netlists in Verilog, `write_verilog -mode funcsim`, run as any other design over the
models, with `glbl` as a second top. Expect a netlist to be slow: NEORV32's, 41,000 cells, runs
about 60 of its clock cycles a second, in 3.5 GB. Where the RTL holds a value unknown, a register no
instruction has written yet, the netlist holds what AMD's RAM primitives start with, 0.

### Not yet

- The models from VHDL, through AMD's component packages, and Vivado's netlists in VHDL.

### Not supported

The `secureip` models, which are encrypted, and SDF back-annotation.

### What is tested

`tests/designs/vendor-xilinx/run.sh` holds Enzan's testbenches of the commonest primitives to Icarus
Verilog and XPM's to Verilator when `XILINX_VIVADO` is set, and NEORV32's netlist, which
`netlist/make.sh` makes with Vivado, to Icarus Verilog and to its RTL when `XILINX_NETLISTS` names
where it is. `release.txt` beside it names the Vivado release it last passed with.

```
XILINX_VIVADO=/opt/Xilinx/Vivado/2026.1 tests/designs/vendor-xilinx/run.sh enzan iverilog work/
```

## Altera

Not yet: the Altera libraries come after AMD's.
