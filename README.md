# Enzan RTL Simulator

Enzan is an event-driven, mixed-language RTL simulator for SystemVerilog and VHDL, with an
interactive graphical debugging environment: fast, event-driven, mixed-language simulation behind a
usable GUI, with no commercial license required.

The name is Japanese and carries two readings: *distant peaks* (遠山), for the mountain-like horizon
of a screen full of waveforms, and *calculation* (演算), for the work the simulator performs.

Enzan simulates SystemVerilog and VHDL, each alone or both in one design, writes VCD and FST
waveforms, and runs cocotb testbenches. The NEORV32 RISC-V processor runs its own VHDL testbench,
and the same testbench rewritten in Verilog, with GHDL's results. Performance work is next.

## Installing

Enzan runs on Linux. Each release has packages for Ubuntu 24.04, Debian 13, Fedora, RHEL 9 and
Arch Linux, and a source tarball that builds with no network: [Installing](docs/installing.md)
says how to install either. To build from a checkout, see [CONTRIBUTING.md](CONTRIBUTING.md).

## Running a simulation

```sh
enzan --top tb --timescale ns --fst wave.fst design.sv testbench.sv
gtkwave wave.fst
```

`--vcd` writes a VCD instead, and both may be given at once; FST is far smaller on a real design,
and a viewer can seek in it. A VHDL design runs the same way, from `.vhd` or `.vhdl` files in any
order. Sources that name their library, `library neorv32;`, are analysed into it with `--work`:

```sh
enzan --top tb counter.vhd counter_tb.vhd
enzan --work neorv32 --top neorv32_tb -f neorv32_files.f --ieee-asserts disable --assert-level error
```

`enzan --help` lists the options.

To open the design in a window instead, add `--gui`:

```sh
enzan run --gui --top tb design.sv testbench.sv
```

## What works today

| | |
|---|---|
| SystemVerilog modules, parameters, generate, packages, interfaces | yes |
| Processes, continuous assignments, tasks and functions, `#` delays and event controls | yes |
| Packed and unpacked arrays, structs, enums, four-state values with X and Z | yes |
| Nets with several drivers, resolved by their type | yes |
| `` `timescale `` and `timeunit` | yes, each module's delays and `$time` in its own unit |
| `real` and `string` variables and ports | not yet, reported by name |
| VHDL entities, architectures, processes, generics, generate, records, arrays | yes |
| `std_logic_1164`, `numeric_std`, `math_real`, `std.textio` and files | yes |
| VHDL access types beyond `line`, protected types, configurations | not yet, reported by name |
| A design mixing SystemVerilog and VHDL, either instantiating the other | yes |
| VCD and FST waveforms | yes |
| GUI: live waveforms, cursors, grouping, radixes, force and deposit, remote attach | yes |
| GUI: the Processes and Source windows | not yet |
| cocotb testbenches, of a SystemVerilog or a VHDL toplevel | yes |

Anything unsupported is reported with a source location and the construct named, never simulated
wrongly.

## Documentation

Using Enzan:

- [Installing](docs/installing.md): a release's packages, or its source tarball.
- [The graphical front end](docs/gui.md): the wave window, writing signals, remote simulations,
  themes and settings.
- [Signal lists](docs/signal-lists.md): the file the wave window's rows are saved to.
- [Libraries, file lists and make](docs/libraries.md): analysis libraries, `.f` files, and
  building a design with `make`.
- [Mixed-language designs](docs/mixed-language.md): what crosses between SystemVerilog and VHDL,
  and at what cost.
- [cocotb](docs/cocotb.md): running cocotb testbenches.
- [Third-party dependencies](docs/third_party.md): what Enzan is built on, and their licenses.

Developing Enzan: [CONTRIBUTING.md](CONTRIBUTING.md), which covers building from source, checking
a change as CI does, and the developer documentation.

## License

Enzan is licensed under the [GNU General Public License v3.0](LICENSE), a consequence of linking
NVC, which is GPLv3, for the VHDL frontend.
