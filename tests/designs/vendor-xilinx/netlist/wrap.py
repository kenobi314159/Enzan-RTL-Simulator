#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes what puts NEORV32 into a functional-simulation netlist and the netlist back into
NEORV32's testbench, from NEORV32's own sources.

    wrap.py <neorv32-source> <verilog-testbench> <output-directory>

Vivado flattens a port of a record type into one port per field, with names a VHDL testbench
cannot connect to, so the netlist is synthesised from a top of its own, `neorv32_netlist`, whose
ports are NEORV32's with each trace record a vector. Four files are written:

- neorv32_netlist_pkg.vhd: the trace record as a vector and back, its first field highest, and
  neorv32_netlist.vh, the vector's width for a Verilog testbench.
- neorv32_netlist.vhd: the top synthesised, around `neorv32_top` configured as the Verilog
  testbench (tests/designs/neorv32/tb/neorv32_tb.v) configures it, but with the trace port on, so
  that the netlist shows each core's trace, and no trace log, which synthesis has no use for.
- neorv32_top.vhd: an entity `neorv32_top` as NEORV32 declares it, which NEORV32's testbench binds
  to, around the netlist: it writes each core's trace log from the netlist's trace port, as the
  core does in simulation, by NEORV32's own logger.

The entity, the record and the configuration are all read from the sources, never restated.
"""

import ast
import operator
import re
import sys
from pathlib import Path


def entity_text(top: str) -> str:
    """The declaration of `neorv32_top`, from `entity` to `end entity;`."""
    match = re.search(r"^entity neorv32_top is.*?^end entity;", top, re.S | re.M)
    if match is None:
        sys.exit("wrap.py: neorv32_top.vhd declares no entity neorv32_top")
    return match.group(0)


def clause(entity: str, keyword: str) -> list[tuple[str, str, str]]:
    """The declarations of the entity's generic or port clause: name, mode (empty for a generic)
    and type, one per line as NEORV32 writes them, defaults dropped."""
    match = re.search(rf"^  {keyword} \((.*?)^  \);", entity, re.S | re.M)
    if match is None:
        sys.exit(f"wrap.py: neorv32_top has no {keyword} clause")
    found = []
    for line in match.group(1).splitlines():
        line = line.split("--")[0].strip().rstrip(";")
        if not line:
            continue
        name, rest = (part.strip() for part in line.split(":", 1))
        rest = rest.split(":=")[0].strip()
        mode = ""
        if keyword == "port":
            mode, rest = rest.split(None, 1)
        found.append((name, mode, rest.strip()))
    return found


def record_fields(package: str) -> list[tuple[str, int]]:
    """The fields of `trace_port_t` and their widths, in declaration order."""
    match = re.search(r"type trace_port_t is record(.*?)end record;", package, re.S)
    if match is None:
        sys.exit("wrap.py: neorv32_package.vhd declares no trace_port_t")
    fields = []
    for line in match.group(1).splitlines():
        line = line.split("--")[0].strip()
        if not line:
            continue
        name, kind = (part.strip().rstrip(";") for part in line.split(":", 1))
        vector = re.fullmatch(r"std_ulogic_vector\((\d+) downto 0\)", kind)
        if vector is not None:
            fields.append((name, int(vector.group(1)) + 1))
        elif kind == "std_ulogic":
            fields.append((name, 0))
        else:
            sys.exit(f"wrap.py: trace_port_t's field {name} is of a type not handled: {kind}")
    return fields


OPERATORS = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul}


def arithmetic(text: str, names: dict[str, int]) -> int:
    """An integer expression of the Verilog testbench: numbers, its localparams and + - *."""

    def value(node: ast.AST) -> int:
        if isinstance(node, ast.Constant) and isinstance(node.value, int):
            return node.value
        if isinstance(node, ast.Name) and node.id in names:
            return names[node.id]
        if isinstance(node, ast.BinOp) and type(node.op) in OPERATORS:
            return OPERATORS[type(node.op)](value(node.left), value(node.right))
        raise ValueError(text)

    return value(ast.parse(text, mode="eval").body)


def configuration(testbench: str) -> dict[str, str]:
    """The parameters the Verilog testbench gives `neorv32_top`, as written there."""
    match = re.search(r"neorv32_top #\((.*?)\) neorv32_top_inst", testbench, re.S)
    if match is None:
        sys.exit("wrap.py: the Verilog testbench instantiates no neorv32_top")
    return dict(re.findall(r"\.(\w+)\(([^()]*)\)", match.group(1)))


def vhdl_value(name: str, kind: str, given: str, names: dict[str, int]) -> str:
    """A Verilog parameter's value as a VHDL generic's of type `kind`."""
    if kind == "boolean":
        if given not in ("1'b0", "1'b1"):
            sys.exit(f"wrap.py: {name} is boolean but given {given}")
        return "true" if given == "1'b1" else "false"
    if kind.startswith("natural"):
        return str(arithmetic(given, names))
    vector = re.fullmatch(r"std_ulogic_vector\((\d+) downto 0\)", kind)
    based = re.fullmatch(r"(\d+)'h([0-9a-fA-F_]+)", given)
    if vector is not None and based is not None and int(based.group(1)) == int(vector.group(1)) + 1:
        return f'x"{based.group(2).replace("_", "").upper()}"'
    sys.exit(f"wrap.py: {name}'s value {given} cannot be given a generic of type {kind}")


def main() -> None:
    if len(sys.argv) != 4:
        sys.exit("usage: wrap.py <neorv32-source> <verilog-testbench> <output-directory>")
    source, testbench_path, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    core = source / "rtl" / "core"
    entity = entity_text((core / "neorv32_top.vhd").read_text())
    generics = clause(entity, "generic")
    ports = clause(entity, "port")
    fields = record_fields((core / "neorv32_package.vhd").read_text())
    testbench = testbench_path.read_text()
    names = {name: int(value) for name, value in
             re.findall(r"localparam integer (\w+) = (\d+);", testbench)}

    # The testbench's configuration, but the trace port on and the trace log off.
    given = configuration(testbench)
    given["TRACE_PORT_EN"] = "1'b1"
    given["TRACE_SIMLOG_EN"] = "1'b0"
    kinds = {name: kind for name, _, kind in generics}
    unknown = sorted(set(given) - set(kinds))
    if unknown:
        sys.exit(f"wrap.py: neorv32_top has no generic {', '.join(unknown)}")

    width = sum(max(bits, 1) for _, bits in fields)
    header = ("-- Generated by tests/designs/vendor-xilinx/netlist/wrap.py from NEORV32's sources; "
              "not to be edited.\n\nlibrary ieee;\nuse ieee.std_logic_1164.all;\n\n"
              "library neorv32;\nuse neorv32.neorv32_package.all;\n")
    out.mkdir(parents=True, exist_ok=True)

    # The record as a vector and back.
    pieces, assignments, high = [], [], width - 1
    for name, bits in fields:
        pieces.append(f"t.{name}")
        if bits == 0:
            assignments.append(f"    t.{name} := v({high});")
            high -= 1
        else:
            assignments.append(f"    t.{name} := v({high} downto {high - bits + 1});")
            high -= bits
    (out / "neorv32_netlist.vh").write_text(
        "// Generated by tests/designs/vendor-xilinx/netlist/wrap.py; not to be edited.\n"
        f"// The width of a core's trace port on neorv32_netlist.\n`define TRACE_WIDTH {width}\n")
    (out / "neorv32_netlist_pkg.vhd").write_text(
        header + "\npackage neorv32_netlist_pkg is\n"
        f"  constant trace_width_c : natural := {width};\n"
        "  function trace_pack(t : trace_port_t) return std_ulogic_vector;\n"
        "  function trace_unpack(v : std_ulogic_vector(trace_width_c-1 downto 0)) "
        "return trace_port_t;\nend package;\n\npackage body neorv32_netlist_pkg is\n"
        "  function trace_pack(t : trace_port_t) return std_ulogic_vector is\n  begin\n"
        f"    return {' & '.join(pieces)};\n  end function;\n\n"
        "  function trace_unpack(v : std_ulogic_vector(trace_width_c-1 downto 0)) "
        "return trace_port_t is\n    variable t : trace_port_t;\n  begin\n"
        + "\n".join(assignments) + "\n    return t;\n  end function;\nend package body;\n")

    # The ports of the netlist's top, each trace record a vector.
    def plain(kind: str) -> str:
        return "std_ulogic_vector(trace_width_c-1 downto 0)" if kind == "trace_port_t" else kind

    plain_ports = ";\n".join(f"    {name} : {mode} {plain(kind)}" for name, mode, kind in ports)
    traces = [name for name, _, kind in ports if kind == "trace_port_t"]
    use_package = "use neorv32.neorv32_netlist_pkg.all;\n"

    generic_map = ",\n".join(f"      {name} => {vhdl_value(name, kinds[name], value, names)}"
                             for name, value in given.items())
    wrapper_map = ",\n".join(
        f"      {name} => {name + '_record' if name in traces else name}" for name, _, _ in ports)
    (out / "neorv32_netlist.vhd").write_text(
        header + use_package + "\nentity neorv32_netlist is\n  port (\n" + plain_ports
        + "\n  );\nend entity;\n\narchitecture wrapper of neorv32_netlist is\n"
        + "".join(f"  signal {name}_record : trace_port_t;\n" for name in traces)
        + "begin\n  top: entity neorv32.neorv32_top\n    generic map (\n" + generic_map
        + "\n    )\n    port map (\n" + wrapper_map + "\n    );\n"
        + "".join(f"  {name} <= trace_pack({name}_record);\n" for name in traces)
        + "end architecture;\n")

    # NEORV32's entity around the netlist.
    netlist_map = ",\n".join(
        f"      {name} => {name + '_vector' if name in traces else name}" for name, _, _ in ports)
    logs = []
    for index, name in enumerate(traces):
        enabled = "TRACE_SIMLOG_EN" if index == 0 else "TRACE_SIMLOG_EN and DUAL_CORE_EN"
        logs.append(
            f"  {name}_log: if {enabled} generate\n"
            f"    logger: entity neorv32.neorv32_cpu_trace_simlog\n"
            f"      generic map (LOG_FILE => \"neorv32_trace{index}.log\")\n"
            f"      port map (clk_i => clk_i, rstn_i => rstn_sys, trace_i => {name}_record);\n"
            f"  end generate;\n")
    (out / "neorv32_top.vhd").write_text(
        header + use_package + "\n" + entity + "\n\n"
        "-- The netlist of neorv32_netlist, which neorv32_netlist.vhd configures, whatever the\n"
        "-- generics here say; its trace port shows each core's trace, which this entity's port\n"
        "-- gives when TRACE_PORT_EN asks, and from which it writes each core's trace log, as\n"
        "-- the core does in simulation, when TRACE_SIMLOG_EN asks.\n"
        "architecture netlist of neorv32_top is\n  component neorv32_netlist is\n    port (\n"
        + ";\n".join(f"      {name} : {mode} {plain(kind)}" for name, mode, kind in ports)
        + "\n    );\n  end component;\n\n"
        + "".join(f"  signal {name}_vector : std_ulogic_vector(trace_width_c-1 downto 0);\n"
                  f"  signal {name}_record : trace_port_t;\n" for name in traces)
        + "  signal rstn_seq : std_ulogic_vector(2 downto 0);\n  signal rstn_sys : std_ulogic;\n"
        "begin\n  netlist: neorv32_netlist\n    port map (\n" + netlist_map + "\n    );\n"
        + "".join(f"  {name}_record <= trace_unpack({name}_vector);\n"
                  f"  {name} <= {name}_record when TRACE_PORT_EN else trace_port_terminate_c;\n"
                  for name in traces)
        + "\n  -- The system reset the cores run under, which the netlist keeps inside, as\n"
        "  -- neorv32_sys_reset makes it: from the outputs that say a debugger or watchdog reset,\n"
        "  -- which come a clock after what the generator sees, so its sequence is a stage\n"
        "  -- shorter. Exact on release; a clock late on assertion, when the cores retire\n"
        "  -- nothing.\n"
        "  sequencer: process(rstn_i, clk_i)\n  begin\n    if (rstn_i = '0') then\n"
        "      rstn_seq <= (others => '0');\n      rstn_sys <= '0';\n"
        "    elsif rising_edge(clk_i) then\n"
        "      if (rstn_wdt_o = '0') or (rstn_ocd_o = '0') then\n"
        "        rstn_seq <= (others => '0');\n      else\n"
        "        rstn_seq <= rstn_seq(1 downto 0) & '1';\n      end if;\n"
        "      rstn_sys <= and rstn_seq;\n    end if;\n  end process;\n\n"
        + "\n".join(logs) + "end architecture;\n")


if __name__ == "__main__":
    main()
