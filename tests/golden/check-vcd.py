#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate that a VCD file is well formed.

M1's exit criteria include waveforms opening in GTKWave (DESIGN_GUIDE.md section 18). A viewer
needs a display and cannot run in CI, so this checks the properties a viewer relies on instead,
which is both automatable and stricter than "it opened":

* the header declares a timescale and ends with ``$enddefinitions``;
* every ``$scope`` is closed by an ``$upscope``, and none is closed twice;
* every identifier used in a value change was declared by a ``$var``;
* every value change has the width its ``$var`` declared;
* timestamps never go backwards, which a viewer's time index depends on;
* the first timestamp is present, so traces are defined from the start rather than undefined
  until their first change.

Exits non-zero with a message naming the line when any of that fails.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

SCALAR_VALUES = set("01xXzZ")


def fail(line_number: int, message: str) -> None:
    print(f"{line_number}: {message}", file=sys.stderr)
    raise SystemExit(1)


def check(path: Path) -> None:
    widths: dict[str, int] = {}
    scope_depth = 0
    saw_timescale = False
    saw_enddefinitions = False
    last_time: int | None = None
    changes = 0

    var_pattern = re.compile(r"^\$var\s+(\S+)\s+(\d+)\s+(\S+)\s+(.+?)\s+\$end$")

    for line_number, raw in enumerate(path.read_text().splitlines(), start=1):
        line = raw.strip()
        if not line:
            continue

        if line.startswith("$timescale"):
            saw_timescale = True
            continue
        if line.startswith("$scope"):
            scope_depth += 1
            continue
        if line.startswith("$upscope"):
            scope_depth -= 1
            if scope_depth < 0:
                fail(line_number, "$upscope without a matching $scope")
            continue
        if line.startswith("$enddefinitions"):
            if scope_depth != 0:
                fail(line_number, f"{scope_depth} scope(s) still open at $enddefinitions")
            saw_enddefinitions = True
            continue
        if line.startswith("$var"):
            match = var_pattern.match(line)
            if match is None:
                fail(line_number, f"malformed $var: {line}")
            width = int(match.group(2))
            identifier = match.group(3)
            # Several $var declarations may share an identifier — a collapsed port has two
            # names for one slot — but they must agree about the width.
            if identifier in widths and widths[identifier] != width:
                fail(line_number, f"identifier {identifier!r} declared with two widths")
            widths[identifier] = width
            continue
        if line.startswith("$"):
            continue

        if line.startswith("#"):
            time = int(line[1:])
            if last_time is not None and time < last_time:
                fail(line_number, f"time went backwards, {last_time} then {time}")
            last_time = time
            continue

        # A value change: either `<value><id>` for one bit or `b<bits> <id>` for a vector.
        if line[0] in "bB":
            parts = line.split()
            if len(parts) != 2:
                fail(line_number, f"malformed vector change: {line}")
            bits, identifier = parts[0][1:], parts[1]
            if identifier not in widths:
                fail(line_number, f"value change for undeclared identifier {identifier!r}")
            if len(bits) != widths[identifier]:
                fail(
                    line_number,
                    f"{identifier!r} is {widths[identifier]} bits but the change has {len(bits)}",
                )
            if any(character not in SCALAR_VALUES for character in bits):
                fail(line_number, f"invalid bit in {bits!r}")
            changes += 1
            continue

        if line[0] in SCALAR_VALUES:
            identifier = line[1:]
            if identifier not in widths:
                fail(line_number, f"value change for undeclared identifier {identifier!r}")
            if widths[identifier] != 1:
                fail(line_number, f"{identifier!r} is a vector but was written as a scalar")
            changes += 1
            continue

        fail(line_number, f"unrecognised line: {line}")

    if not saw_timescale:
        fail(0, "no $timescale in the header")
    if not saw_enddefinitions:
        fail(0, "no $enddefinitions")
    if last_time is None:
        fail(0, "no timestamps at all")
    if not widths:
        fail(0, "no signals were declared")
    if changes == 0:
        fail(0, "no value changes were recorded")

    print(f"{path.name}: {len(widths)} signals, {changes} value changes, ends at #{last_time}")


def main() -> None:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <file.vcd>", file=sys.stderr)
        raise SystemExit(2)
    check(Path(sys.argv[1]))


if __name__ == "__main__":
    main()
