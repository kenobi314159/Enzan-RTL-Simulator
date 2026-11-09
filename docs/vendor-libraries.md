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

### From VHDL

A VHDL design reaches the same Verilog models through AMD's component packages, analysed once into
libraries of their names: `unisim` from `unisim_retarget_VCOMP.vhd`, `xpm` from `xpm_VCOMP.vhd`,
and AMD's VHDL `unimacro`, in the order its `vhdl_analyze_order` gives, against `unisim`:

```
enzan lib create --name unisim libs/unisim
enzan analyse --out-lib libs/unisim ${XILINX_VIVADO}/data/vhdl/src/unisims/unisim_retarget_VCOMP.vhd
enzan lib create --name xpm libs/xpm
enzan analyse --out-lib libs/xpm ${XILINX_VIVADO}/data/ip/xpm/xpm_VCOMP.vhd
enzan --in-lib libs/unisim --in-lib libs/xpm --top tb --top glbl tb.vhd design.vhd -f xilinx.f
```

A component is bound to the Verilog model of its name, which `-y` in the file list finds, and its
generics set the model's parameters. A value a model drives weakly, a `PULLUP`'s, reaches the VHDL
signal as `L` or `H`, so that it yields to the pad's other drivers.

### Functional-simulation netlists

Vivado's netlists, `write_verilog -mode funcsim` or `write_vhdl -mode funcsim`, run as any other
design over the models, with `glbl` as a second top; one in VHDL reaches them through `unisim`.
Expect a netlist to be slow: NEORV32's, 41,000 cells, runs about 60 of its clock cycles a second, in
3.5 GB, and a third as fast in VHDL, where every cell crosses the language boundary. Where the RTL
holds a value unknown, a register no instruction has written yet, the netlist holds what AMD's RAM
primitives start with, 0.

### Not supported

The `secureip` models, which are encrypted, and SDF back-annotation.

### What is tested

`tests/designs/vendor-xilinx/run.sh` holds Enzan's testbenches of the commonest primitives to Icarus
Verilog, XPM's to Verilator and the same testbenches in VHDL to their Verilog counterparts when
`XILINX_VIVADO` is set, and NEORV32's netlists in Verilog and in VHDL, which `netlist/make.sh` makes
with Vivado, to its RTL when `XILINX_NETLISTS` names where they are. `release.txt` beside it names
the Vivado release it last passed with.

```
XILINX_VIVADO=/opt/Xilinx/Vivado/2026.1 tests/designs/vendor-xilinx/run.sh enzan iverilog work/
```

## Altera

Quartus keeps its models in `${QUARTUS_ROOTDIR}/eda/sim_lib`, a file per library holding every model
of the library, with `QUARTUS_ROOTDIR` the Quartus directory. `-v` names such a file; Enzan reads it
when the design needs a module from it, and takes only the modules the design uses.

### The IP libraries

`altera_mf.v`, `220model.v` (LPM) and `sgate.v`, over `altera_primitives.v`:

```
# altera.f
-v ${QUARTUS_ROOTDIR}/eda/sim_lib/altera_primitives.v
-v ${QUARTUS_ROOTDIR}/eda/sim_lib/220model.v
-v ${QUARTUS_ROOTDIR}/eda/sim_lib/sgate.v
-v ${QUARTUS_ROOTDIR}/eda/sim_lib/altera_mf.v
```

```
enzan --top tb -f altera.f tb.v design.v
```

A memory initialised from a file, as `altsyncram` is by its `init_file`, reads it from the
directory the run is in and writes a `.ver` file beside it, as the models do under every simulator.
`altsyncram` with a Memory Initialization File, `scfifo` and the LPM arithmetic run as they do under
Icarus Verilog; every other model elaborates, but none is held to a reference yet.

### From VHDL

A VHDL design reaches the same Verilog models through Altera's component packages, analysed once
into libraries of their names: `altera_mf` from `altera_mf_components.vhd` and `lpm` from
`220pack.vhd`:

```
enzan lib create --name altera_mf libs/altera_mf
enzan analyse --out-lib libs/altera_mf ${QUARTUS_ROOTDIR}/eda/sim_lib/altera_mf_components.vhd
enzan lib create --name lpm libs/lpm
enzan analyse --out-lib libs/lpm ${QUARTUS_ROOTDIR}/eda/sim_lib/220pack.vhd
enzan --in-lib libs/altera_mf --in-lib libs/lpm --top tb tb.vhd design.vhd -f altera.f
```

A generic a component declares and its model lacks, as `altsyncram`'s
`sim_show_memory_data_in_port_b_layout` is, sets nothing where the design leaves it at its default.

Altera's own VHDL models analyse into the same libraries in place of the Verilog ones:
`altera_mf.vhd` after `altera_mf_components.vhd`, and `220model.vhd` after `220pack.vhd`. `scfifo`,
`dcfifo`, the LPM counter and the memories `altsyncram`, `altdpram` and the LPM ROM run on them as
they do under GHDL, each memory reading a Memory Initialization File, and `altsyncram` an Intel HEX
file too.

### Functional-simulation netlists

A Cyclone 10 LP netlist that Quartus writes for simulation, a `.vo` file, runs over the device's
atoms and `altera_primitives.v`:

```
# cyclone10lp.f
-v ${QUARTUS_ROOTDIR}/eda/sim_lib/cyclone10lp_atoms.v
-v ${QUARTUS_ROOTDIR}/eda/sim_lib/altera_primitives.v
```

```
enzan --top tb -f cyclone10lp.f tb.v design.vo
```

Each block RAM carries its contents as a parameter, so a netlist needs no initialisation file.
Quartus connects every output it does not use to nothing by name, `.cout()`, and slang warns of
each (ENZ1002): tens of thousands of lines for a processor, which change nothing in the run.
NEORV32's netlist, 53,000 cells, runs as it does under Icarus Verilog.

### Not yet

- **`altera_lnsim.sv`**, the models of Cyclone V and later families: most elaborate, none is held
  to a reference yet, and `altera_pll`'s arrays of `real` are refused.
- **An Intel HEX file read by LPM's VHDL memories**, which build a vector whose length only the run
  knows. The device atoms' VHDL is written with VITAL, which Enzan does not simulate.
- **Families whose atoms wrap encrypted cores**, as most of Cyclone V's do: a source holding an
  encrypted envelope is refused.
