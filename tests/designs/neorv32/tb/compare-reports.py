#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare the reports of NEORV32 under the Verilog testbench with GHDL's under the VHDL one.

What the VHDL testbench reports, its on-chip debugger sequence's `[TB:JTAG]` messages, the Verilog
testbench displays, a note as its message and an error after `error: `; everything else is
reported by the processor's own VHDL in both runs. Both are compared in order, with their
severities, by the rules ../../../golden/vhdl/compare-ghdl.py reads the two transcripts with.

Exits non-zero with every difference listed.
"""

from __future__ import annotations

import argparse
import importlib.util
import sys
from pathlib import Path

_spec = importlib.util.spec_from_file_location(
    "compare_ghdl", Path(__file__).resolve().parents[3] / "golden/vhdl/compare-ghdl.py")
_compare = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_compare)

TESTBENCH = "[TB:JTAG]"


def displayed(text: str) -> list[tuple[str, str]]:
    """The testbench's displayed reports, as severities and messages."""
    reports = []
    for line in text.splitlines():
        if line.startswith("error: " + TESTBENCH):
            reports.append(("error", line.removeprefix("error: ")))
        elif line.startswith(TESTBENCH):
            reports.append(("note", line))
    return reports


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ghdl-transcript", type=Path, required=True)
    parser.add_argument("--enzan-transcript", type=Path, required=True)
    parser.add_argument("--enzan-output", type=Path, required=True)
    arguments = parser.parse_args()

    expected = _compare.ghdl_reports(arguments.ghdl_transcript.read_text())
    expected_testbench = [report for report in expected if report[1].startswith(TESTBENCH)]
    expected_design = [report for report in expected if not report[1].startswith(TESTBENCH)]
    actual_testbench = displayed(arguments.enzan_output.read_text())
    actual_design = _compare.enzan_reports(arguments.enzan_transcript.read_text())

    differences = []
    for what, want, got in (("the testbench's", expected_testbench, actual_testbench),
                            ("the design's", expected_design, actual_design)):
        if want != got:
            differences.append(f"{what} reports differ:")
            differences.extend(f"  GHDL:  {severity}: {message}" for severity, message in want)
            differences.extend(f"  Enzan: {severity}: {message}" for severity, message in got)
    for line in differences:
        print(line, file=sys.stderr)
    return 1 if differences else 0


if __name__ == "__main__":
    sys.exit(main())
