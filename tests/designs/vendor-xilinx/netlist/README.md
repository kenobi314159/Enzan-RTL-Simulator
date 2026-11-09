# NEORV32's functional-simulation netlists

`make.sh` makes the netlists that `../run.sh` simulates: NEORV32, at the design regression's
revision and configured as its testbench is, synthesised by Vivado for a 7-series part and written
by `write_verilog -mode funcsim` and `write_vhdl -mode funcsim`. Vivado runs once per release; the
netlists are written outside the repository, as AMD's models are read from outside it.

```
make.sh /path/to/netlists
XILINX_VIVADO=... XILINX_NETLISTS=/path/to/netlists ../run.sh enzan iverilog work/
```

`wrap.py` writes the top Vivado synthesises, `neorv32_netlist`, around `neorv32_top`: Vivado makes
a port of a record type one port per field, with names NEORV32's VHDL testbench cannot connect
to, so the top's ports are NEORV32's with each trace record a vector. The trace port is on, so that
the netlist shows what each core retires. `wrap.py` also writes the entity that testbench binds to
in the netlist's place, which writes each core's trace log from that port as the core does in
simulation. All of it is read from NEORV32's sources and from `../../neorv32/tb/neorv32_tb.v`,
whose configuration is the testbench's.

Vivado's module names but the top's are prefixed with `netlist_`, so that none is the name of an
entity of NEORV32's, which the run under NEORV32's testbench reads beside the netlist.

## What Vivado needs

Vivado 2026.1 supports a few distributions; on another, its `bin/` scripts treat it as an unknown
one and never add the directory that holds its own copies of `libncurses.so.5` and `libtinfo.so.5`,
which it needs. Give them on `LD_LIBRARY_PATH`, from
`<Vivado>/lib/lnx64.o/Ubuntu/24` or the distribution's compatibility packages, and always run
Vivado through `bin/vivado`, never `bin/unwrapped/`, which cannot find its own libraries. Synthesis
takes a few minutes and about 3 GB of memory.
