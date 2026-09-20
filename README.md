# Enzan RTL Simulator

Enzan is an event-driven, mixed-language RTL simulator for SystemVerilog and
VHDL, with an interactive graphical debugging environment. It aims to fill a
gap in the open-source tooling landscape: fast, event-driven, mixed-language
simulation behind a usable GUI, with no commercial license required.

The name is Japanese and carries two readings: *distant peaks* (遠山), for the
mountain-like horizon of a screen full of waveforms, and *calculation* (演算),
for the computational work the simulator performs.

**Status:** early design phase. No simulator code exists yet — see the
roadmap below.

## Documentation

- [`DESIGN_GUIDE.md`](DESIGN_GUIDE.md) — the normative specification and
  development guide: architecture, simulation semantics, coding standards,
  testing strategy and roadmap. Start here.
- `docs/` — human-facing documentation (user manual, tutorials), to be
  derived from the design guide as the project matures.

## Status and roadmap

Development proceeds in gated milestones, starting with build system and
test infrastructure (M0), a SystemVerilog subset simulating to a waveform
(M1), the interactive GUI (M2), and onward through VPI/cocotb support, VHDL
via NVC, and mixed-language simulation. See
[`DESIGN_GUIDE.md` §18](DESIGN_GUIDE.md#18-roadmap) for the full roadmap and
exit criteria.

## License

Enzan is licensed under the [GNU General Public License v3.0](LICENSE).
