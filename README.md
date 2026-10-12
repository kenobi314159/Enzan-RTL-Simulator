# Enzan RTL Simulator

Enzan is an event-driven, mixed-language RTL simulator for SystemVerilog and
VHDL, with an interactive graphical debugging environment. It aims to fill a
gap in the open-source tooling landscape: fast, event-driven, mixed-language
simulation behind a usable GUI, with no commercial license required.

The name is Japanese and carries two readings: *distant peaks* (遠山), for the
mountain-like horizon of a screen full of waveforms, and *calculation* (演算),
for the computational work the simulator performs.

**Status: milestone M4 complete.** Enzan simulates a subset of SystemVerilog, writes VCD and
FST waveforms, comes with an interactive graphical debugging environment, and runs cocotb
testbenches through VPI. A parameterised
counter, a synchronous FIFO, and a packet router
three levels deep that pushes four hundred and eighty transfers through three channel instances
all run, self-check, and agree with Icarus Verilog. See [what works
today](#what-works-today) and the roadmap below.

## Building

Enzan targets **Linux only**; Windows and macOS support are explicit non-goals.

### Prerequisites

| Requirement | Minimum | Notes |
|---|---|---|
| C++ compiler | GCC 13 or Clang 17 | C++20 is required by slang |
| CMake | 3.24 | presets are used throughout |
| Ninja | any recent | the presets all use the Ninja generator |
| zlib | 1.2.11 | FST waveform compression |
| Python 3 | 3.8 | optional; only for the waveform structure checks in the test suite |
| GoogleTest | 1.12 | built from source automatically if absent |
| Qt 6 | 6.4 | only for the GUI, which is off by default |
| Tcl | 8.6 | the GUI theme and signal lists; needed only with `-DENZAN_BUILD_GUI=ON` |
| flex | 2.6 | generates NVC's lexer; needed only with `-DENZAN_ENABLE_VHDL=ON` |
| zstd | 1.4 | NVC's library files; needed only with `-DENZAN_ENABLE_VHDL=ON` |
| ccache | any | optional; used automatically when present |

On Arch and derivatives:

```sh
sudo pacman -S --needed base-devel cmake ninja zlib gtest tcl ccache
# for the GUI:
sudo pacman -S --needed qt6-base
# for the VHDL frontend:
sudo pacman -S --needed flex zstd
```

On Debian and Ubuntu:

```sh
sudo apt install build-essential cmake ninja-build zlib1g-dev libgtest-dev tcl-dev ccache
# for the GUI:
sudo apt install qt6-base-dev
# for the VHDL frontend:
sudo apt install flex libzstd-dev
```

On Fedora:

```sh
sudo dnf install gcc-c++ cmake ninja-build zlib-devel gtest-devel tcl-devel ccache
# for the GUI:
sudo dnf install qt6-qtbase-devel
# for the VHDL frontend:
sudo dnf install flex libzstd-devel
```

If a dependency is missing, CMake stops with a message naming the package to
install for your distribution rather than with a link error.

Tcl is needed only for the GUI, whose theme is a Tcl script and whose signal lists are read
with Tcl's parser.

The VHDL frontend, which is arriving with milestone M6 and is off by default, is built on
[NVC](https://github.com/nickg/nvc), which is a git submodule. Fetch it before configuring with
`-DENZAN_ENABLE_VHDL=ON`:

```sh
git submodule update --init third_party/nvc
```

### Running a simulation

```sh
enzan --top tb --timescale ns --fst wave.fst design.sv testbench.sv
gtkwave wave.fst
```

`--vcd` writes a VCD instead, and both may be given at once. Prefer FST unless you want to read or
diff the file yourself: it is far smaller on a real design and a viewer can seek in it.

`enzan --help` lists the options.

### Running a simulation under the GUI

The GUI is built with `-DENZAN_BUILD_GUI=ON` — off by default, because Qt 6 is a sizeable
thing to install for someone working on the kernel. With it built:

```sh
enzan run --gui --top tb design.sv testbench.sv
```

which elaborates the design, opens a window on it and attaches. `--gui` is an option of `enzan`
rather than a separate program: the socket, the listening and the attaching are arranged for you. Press **Run**, click a scope in
**Structure**, select signals in **Objects** and press Ctrl+W to put them in the wave window;
the traces fill in as the simulation advances.

In the wave window: the wheel scrolls, **Ctrl+wheel** zooms about the pointer and **Shift+wheel**
pans. Dragging the **right button** right-to-left frames the range you swept; left-to-right zooms
to fit. A **left click** selects the signal you clicked and puts the cursor on the nearest edge,
and **Tab** and **Shift+Tab** then walk that cursor from edge to edge — lock a second cursor and
read both times off their labels to measure. Right-click a cursor's label to rename, lock or
remove it.

Selecting a signal in the list does the same thing as clicking its trace, so Tab works whichever
way you picked it — and it works on a group too.

Rows drag to reorder, rename in place with **F2**, copy and paste with **Ctrl+C** and **Ctrl+V**,
and the context menu offers those, a pack/unpack toggle, a radix and one of five highlight
colours. An unpacked vector is a group like any other: bits drag
out of it and back into it, and anything else can be dragged in too. A bus expands into its bits, and a row showing bits shows
what is left of them: delete one and its bus narrows to the rest. The same rule works the other
way round, so a group of one-bit signals reads and draws as a vector; a group of wider things
reads as the list of them, `{001122}{112233}`, nesting as `{{11}{22}}{{22}{33}}`. A signal whose
type is an `enum` shows the name of its value — `S_SEND`, not `3'h2` — and picking a radix shows
the number instead. A scope's parameters and localparams are listed in **Objects** after its
signals and drop into the wave window as constant rows, so `ACK_DELAY` can sit beside the traces
it explains. The full model is [`DESIGN_GUIDE.md` §10.7](DESIGN_GUIDE.md).

The kernel and the GUI are separate processes and talk over a Unix domain socket, so a window on
your machine can drive a simulation on another one:

```sh
# on the compute server
enzan --top tb --listen /tmp/enzan.sock design.sv testbench.sv

# locally, over a forwarded socket
ssh -L /tmp/enzan.sock:/tmp/enzan.sock server
enzan --gui --attach /tmp/enzan.sock
```

The flag you choose is the lifetime. `enzan run --gui` starts the kernel, so closing the window
ends it too. `enzan --gui --attach` finds a kernel it did not start, so closing the window only
detaches and the run is left alone to be looked in on again.

Right-clicking in **Objects** offers **Deposit…**, **Force…** and **Release** alongside *Add to
Wave*. A deposit writes the value now and the next thing that drives the signal overwrites it; a
force holds it against the design until released.

Colours come from a Tcl theme file and nowhere else. `enzan --write-theme mine.tcl` writes
the built-in dark theme out as a starting point, and `enzan --gui --theme mine.tcl` — or dropping it at
`~/.config/enzan-gui/theme.tcl` — loads yours over it, so a file that sets three colours changes
three colours.

What the wave window shows can be kept. *Wave → Save Signal List…* writes it to a file, and
*Load Signal List…* or `enzan run --gui --wave-list signals.tcl` puts it back. The file is a short
list of `add wave` and `group` commands, written in Tcl's syntax but never run as Tcl, and easy
to write by hand:

```tcl
add wave /tb/clk /tb/rst_n
add wave -radix dec -label count /tb/u_dut/count
group "The FIFO" {
    add wave -r /tb/u_fifo
}
```

Adding signals in the window writes the matching command into the transcript, ready to copy.
DESIGN_GUIDE.md section 10.8 has the whole format.

### Running a cocotb testbench

VPI applications load with `--vpi <library>[:<entry>]`, and cocotb is the one this exists for. cocotb
picks a simulator by the makefiles in its own directory, so Enzan's is added to a copy of them:

```sh
pip install cocotb
tools/cocotb/makefiles.sh build/cocotb-makefiles
make SIM=enzan COCOTB_MAKEFILES_DIR=$PWD/build/cocotb-makefiles
```

run in the directory of an ordinary cocotb testbench. `ENZAN_BIN_DIR` names the directory holding
`enzan` when it is not on your `PATH`. Delays inside the design are counted in the unit
`COCOTB_HDL_TIMEPRECISION` names, because Enzan does not read `` `timescale `` yet; a testbench
that drives every event from Python, as most do, does not notice.

cocotb's own regression suite runs in CI, and `tests/conformance/cocotb/run-suite.sh` runs it
locally. Its pass rate is low until M5: most of its cases share one design that uses unpacked
arrays, `real`, `string` and structs, which Enzan does not analyse yet.

## What works today

| | |
|---|---|
| Modules, ports, hierarchy, parameters | yes — each parameterisation becomes its own analysed unit |
| `initial`, `final`, `always`, `always_comb`, `always_latch`, `always_ff` | yes |
| `assign`, blocking `=`, non-blocking `<=` | yes |
| `if`, `case`, `for`, `while`, `do..while`, `repeat`, `forever` | yes |
| Operators, packed vectors, structs, arrays and enums | yes |
| Bit and part selects, `+:`/`-:`, concatenation, replication | yes, reading and writing |
| `#delay`, `@(posedge …)`, event lists on a block header | yes |
| Four-state values with X and Z propagation | yes, throughout |
| `$display`, `$write`, `$time`, `$finish`, `$fatal`, `$error`, `$warning`, `$info` | yes |
| VCD and FST waveform output | yes, both from one run |
| Interactive GUI: hierarchy, objects, live waveform, cursors, zoom, grouping, radix | yes |
| Wave window: edge snapping, Tab between edges, drag to reorder, unpack a bus into bits, highlight a row | yes |
| Wave window: `enum` values shown by name | yes, with the radix menu to see the encoding |
| Wave window: parameters and localparams as constant rows | yes, listed in Objects beside the signals |
| Attaching a local GUI to a kernel running elsewhere | yes, over a forwarded socket |
| `force`, `deposit` and `release` from the Objects window | yes |
| `casez`, `casex`, unpacked arrays, `real`, `string`, tasks, functions | not yet — reported by name |
| Driver resolution, multiple drivers per signal | not yet — M5 |
| VPI, and cocotb testbenches through it | yes — modules, generate blocks, nets, variables, bits, parameters; every callback cocotb uses |
| `` `timescale `` and `timeunit` | not yet — every delay is counted in the `--timescale` unit |
| The Processes and Source windows, VHDL | not yet — M6 |

Anything unsupported is reported with a source location and the construct named, never
mistranslated: a partially translated design would simulate and give the wrong answer, which is
the failure mode the project weighs above all others.

### Configure, build and test

```sh
git clone https://github.com/kenobi314159/Enzan-RTL-Simulator.git
cd Enzan-RTL-Simulator

cmake --preset gcc-debug
cmake --build --preset gcc-debug
ctest --preset gcc-debug
```

The binary lands at `build/gcc-debug/bin/enzan`:

```sh
./build/gcc-debug/bin/enzan --version
```

### Presets

`cmake --list-presets` shows them all. There are `gcc-` and `clang-` variants of
`debug`, `release` and `relwithdebinfo`, plus `clang-sanitize` for the
AddressSanitizer and UndefinedBehaviorSanitizer configuration that CI runs, and
`gcc-profile` for profiling: `relwithdebinfo` with frame pointers kept everywhere, so
that `perf record --call-graph fp` and `samply` get whole stacks cheaply.
Each preset builds into its own directory under `build/`, so several can coexist.
Time a simulation in an optimised build only; a debug build is an order of magnitude
slower, and what dominates its profile is mostly what the optimiser would remove.

### First build takes a while

The SystemVerilog frontend, [slang](https://sv-lang.com/), is not packaged by any
mainstream distribution, so by default the build downloads and compiles it. That
dominates a first configure. Nothing outside the frontend links it, so work on
the kernel, the waveform database or the GUI can switch it off:

```sh
cmake --preset gcc-debug -DENZAN_ENABLE_SYSTEMVERILOG=OFF
```

### Useful options

| Option | Default | Effect |
|---|---|---|
| `ENZAN_BUILD_TESTS` | `ON` | build the unit test suites |
| `ENZAN_ENABLE_SYSTEMVERILOG` | `ON` | build the slang-based frontend (M1) |
| `ENZAN_ENABLE_VHDL` | `OFF` | build the NVC-based frontend (M6); needs flex, zstd and the `third_party/nvc` submodule |
| `ENZAN_BUILD_GUI` | `OFF` | build the Qt 6 front end; needs Qt 6 and Tcl |
| `ENZAN_ENABLE_JIT` | `OFF` | build the LLVM JIT backend (M8) |
| `ENZAN_WERROR` | `OFF` | treat warnings as errors, as CI does |
| `ENZAN_ALLOW_FETCH` | `ON` | allow missing dependencies to be built from source |
| `ENZAN_SANITIZERS` | empty | semicolon-separated sanitizer list |

## Contributing

Before changing anything, read [`DESIGN_GUIDE.md`](DESIGN_GUIDE.md). It is the
normative specification, not background reading: it fixes the simulation
semantics, the coding standards, the testing requirements and the definition of
done, and a commit that makes it wrong is a defect regardless of whether the code
works.

The checks CI runs are all runnable locally:

```sh
clang-format --dry-run --Werror $(git ls-files '*.h' '*.cpp')
./scripts/check-license-headers.sh
cmake --preset clang-debug -DENZAN_WERROR=ON -DENZAN_BUILD_GUI=ON -DENZAN_ENABLE_VHDL=ON
cmake --build --preset clang-debug && ctest --preset clang-debug
clang-tidy -p build/clang-debug --quiet $(git ls-files 'src/*.cpp' 'tools/*.cpp' 'tests/*.cpp')
```

Configure with every optional component on before running `clang-tidy`. Linting a translation
unit the configured build does not compile does not skip it: clang-tidy falls back to default
flags, fails to find the slang, NVC or Qt headers, and reports whatever its half-parsed view of the
file suggests.

The GUI's own tests need no display. They select Qt's offscreen platform from inside the test
binary rather than trusting the environment, so they behave the same on a machine with no
session as inside a desktop.

The test suite includes differential comparisons against Icarus Verilog when `iverilog` is
installed; running the same design under two simulators and comparing the result is the single
highest-value testing strategy available. Benchmarks are a separate target, because a
measurement taken alongside three hundred other tests compares with nothing:

```sh
cmake --build --preset gcc-release --target enzan-bench
```

Every source file begins with `// SPDX-License-Identifier: GPL-3.0-or-later`.
Commit messages carry a category prefix; see section 17.1 of the design guide.

## Documentation

- [`DESIGN_GUIDE.md`](DESIGN_GUIDE.md) — the normative specification and
  development guide: architecture, simulation semantics, coding standards,
  testing strategy and roadmap. Start here.
- [`docs/third_party.md`](docs/third_party.md) — dependencies, their licenses
  and their pinned revisions.
- `docs/` — human-facing documentation (user manual, tutorials), to be
  derived from the design guide as the project matures.

## Status and roadmap

Development proceeds in gated milestones. M0, the build and test foundation, is complete; so is
M1 — a SystemVerilog subset simulating to a VCD or FST waveform, with a counter and a FIFO that
self-check and agree with a reference simulator, and cycles/sec recorded per commit — and so is
M2, the interactive GUI. Broader language coverage is M5's job rather than a gap in any of them.

M2.1, the wave window's mouse and keyboard model, followed, then M3, the command line, file lists
and analysis libraries, and M4, VPI and cocotb.

Next is M5 four-state completion and language breadth, M6 VHDL via NVC, and M7 mixed-language
simulation. See [`DESIGN_GUIDE.md` §18](DESIGN_GUIDE.md#18-roadmap) for the
full roadmap and the exit criteria of each milestone.

## License

Enzan is licensed under the [GNU General Public License v3.0](LICENSE). This is a
consequence of linking NVC, which is GPLv3, to provide the VHDL frontend.
