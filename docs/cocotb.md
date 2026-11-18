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

Supported makefile parameter variables:

- `TOPLEVEL_LANG=vhdl` runs a VHDL toplevel, which needs an Enzan built with the VHDL frontend.
- `ENZAN_BIN_DIR` names the directory holding `enzan` when it is not on your `PATH`.
- `ENZAN_OUT_LIB` opts the main HDL sources into Enzan's separate analysis-library flow. Its value
  is the library directory; its logical name is `TOPLEVEL_LIBRARY`, `work` by default. Without it,
  Enzan analyses the main sources and runs them in one process.
- `ENZAN_INPUT_LIBRARIES` is a space-separated list of existing Enzan libraries, in search order.
  An item is a path or `name=path`, with the same meaning as `--in-lib`.
- `ENZAN_CONTEXT_SOURCES` lists SystemVerilog sources analysed as context but not stored, as
  `--context` does.
- `ENZAN_LIBRARY_ROOT` is where libraries from `VHDL_LIB_ORDER` are built; it defaults to
  `$(SIM_BUILD)/enzan-libraries`.
- `GUI=1` runs the simulation in Enzan's window, which needs the graphical front end installed.
  The tests run once you start the run there, or at once with `SIM_ARGS += --run all`, and
  closing the window ends the simulation.
- `EXTRA_ARGS += --top <module>` adds a top level beside the toplevel, which stays what cocotb
  drives.
- The run counts time in the unit `COCOTB_HDL_TIMEPRECISION` names, a single unit such as `1ns`,
  and that is also the unit of a module that states no `` `timescale ``.
- A design of both languages works as it does elsewhere: the toplevel's language is reached
  through its own interface, and the other's through `GPI_EXTRA`, which Enzan's makefile sets.
  [Mixed-language designs](mixed-language.md) says what each interface shows.

With `ENZAN_OUT_LIB`, `make analyse` updates the library without running the test. Every generated
library has a stamp and an Enzan dependency file, so `make regression` analyses and runs only when
an HDL source, included header, context source or prerequisite library changed. `make sim` still
runs the test every time, as cocotb defines it. `COMPILE_ARGS` go to analysis, `SIM_ARGS` and
plusargs to the run, and `EXTRA_ARGS` to both.

For example:

```make
TOPLEVEL_LIBRARY = work
ENZAN_OUT_LIB = $(SIM_BUILD)/enzan-libraries/work
ENZAN_INPUT_LIBRARIES = vendor=/opt/vendor/sim/work build/ip
ENZAN_CONTEXT_SOURCES = rtl/shared_pkg.sv

# If another rule builds build/ip, tell make when that library changes.
CUSTOM_COMPILE_DEPS += build/ip/.stamp
```

cocotb's standard `VHDL_LIB_ORDER` and `VHDL_SOURCES_<library>` variables build named VHDL
libraries incrementally in the order given. Each one is analysed against `ENZAN_INPUT_LIBRARIES`
and the VHDL libraries before it; the main library is analysed against all of them.

`ENZAN_OUT_LIB` currently refuses a main design containing both languages, because Enzan cannot
store a mixed-language design yet. Leave it unset for that design: auxiliary and input libraries
remain available, while the main sources use the supported one-shot mixed-language flow.

Other VPI applications load with `enzan --vpi <library>[:<entry>]`, and VHPI ones with `--vhpi`.
