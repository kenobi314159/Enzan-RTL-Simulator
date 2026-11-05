# Running cocotb testbenches

[cocotb](https://www.cocotb.org/) drives a simulation from Python. Enzan supports it through VPI
for a SystemVerilog toplevel and through VHPI for a VHDL one.

cocotb picks a simulator by the makefiles in its own directory, so Enzan's makefile is added to a
copy of them:

```sh
pip install cocotb
share/enzan/cocotb/makefiles.sh ~/cocotb-makefiles     # under the prefix Enzan is installed in
make SIM=enzan COCOTB_MAKEFILES_DIR=$HOME/cocotb-makefiles
```

Run the `make` in the directory of an ordinary cocotb testbench. In a source tree the script is
`tools/cocotb/makefiles.sh`.

- `TOPLEVEL_LANG=vhdl` runs a VHDL toplevel, which needs an Enzan built with the VHDL frontend.
- `ENZAN_BIN_DIR` names the directory holding `enzan` when it is not on your `PATH`.
- `EXTRA_ARGS += --top glbl` adds a top level beside the toplevel, as a design using AMD's
  simulation models needs; the toplevel stays what cocotb drives.
- The run counts time in the unit `COCOTB_HDL_TIMEPRECISION` names, a single unit such as `1ns`,
  and that is also the unit of a module that states no `` `timescale ``.
- A design of both languages works as it does elsewhere: the toplevel's language is reached
  through its own interface, and the other's through `GPI_EXTRA`, which Enzan's makefile sets.
  [Mixed-language designs](mixed-language.md) says what each interface shows.

Other VPI applications load with `enzan --vpi <library>[:<entry>]`, and VHPI ones with `--vhpi`.
