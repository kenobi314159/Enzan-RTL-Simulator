#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare a run of a VHDL design under Enzan against the same under GHDL.

Differential testing against a reference simulator is what section 14.3 of DESIGN_GUIDE.md asks of
every golden case, and GHDL is M6's reference for VHDL. Three things are compared:

* **What the design reported**, in order: each `report` and failed `assert`, as its severity and
  its message. Where each simulator prints it, and how, is its own business; what the design said
  is not.
* **What the design wrote to its output**, line by line: what std.textio's `writeline` put on
  `output`. Enzan's standard output holds nothing else, and GHDL's is the rest of its transcript
  once its reports and its own `ghdl:` lines are taken out.
* **What every signal GHDL records did**, as a waveform: the value it settled on at each time step.
  A VCD records settled values, so two correct simulators agree on it whatever order they ran their
  processes in within a step (section 14.3). A generate block is a scope of GHDL's, `g(0)`, and part
  of a name of Enzan's, `g(0).u`, so both are paths `g(0)/u`.

GHDL writes `std_logic` as its nine values and Enzan as the four-state bits they stand for, L as 0,
H as 1, and U, W and - unknown (section 3.6), so GHDL's are mapped the same way before comparing.
GHDL also shortens a vector whose leading bits are 0, which VCD allows; both are padded back to the
declared width. A real is compared as the number it is, whatever digits each wrote it in. A signal GHDL does not record, an enumeration or a time, is not compared. Nor is the
time step a run finishes in: GHDL stops before it writes that step out, and Enzan writes it.

Exits non-zero with every difference listed.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# What a std_logic value, or a bit of a VCD vector, shows as in four-state form.
NORMALISED = {"U": "x", "X": "x", "x": "x", "0": "0", "1": "1", "Z": "z", "z": "z",
              "W": "x", "L": "0", "H": "1", "-": "x"}


def normalise(value: str, width: int) -> str:
    bits = "".join(NORMALISED.get(bit, "?") for bit in value)
    if len(bits) < width:
        # A VCD vector extends leftwards with 0 below a 0 or 1, and with its own leftmost bit
        # otherwise (IEEE 1364 section 18.2.3.4).
        fill = bits[0] if bits and bits[0] in "xz" else "0"
        bits = fill * (width - len(bits)) + bits
    return bits


def read_vcd(path: Path) -> dict[str, list[tuple[int, str]]]:
    """Each declared signal's settled value at every time step it changed, by its path."""
    tokens = path.read_text().split()
    names: dict[str, list[str]] = {}
    widths: dict[str, int] = {}
    reals: set[str] = set()
    scopes: list[str] = []
    changes: dict[str, list[tuple[int, str]]] = {}
    time = 0
    position = 0

    def record(identifier: str, value: str) -> None:
        if identifier not in widths:
            return
        # A real is a number, which each simulator may write in a form of its own.
        settled = repr(float(value)) if identifier in reals else normalise(value,
                                                                           widths[identifier])
        history = changes.setdefault(identifier, [])
        if history and history[-1][0] == time:
            history[-1] = (time, settled)
        elif not history or history[-1][1] != settled:
            history.append((time, settled))

    while position < len(tokens):
        token = tokens[position]
        if token == "$scope":
            scopes.append(tokens[position + 2])
            position = tokens.index("$end", position) + 1
        elif token == "$upscope":
            scopes.pop()
            position = tokens.index("$end", position) + 1
        elif token == "$var":
            width = int(tokens[position + 2])
            identifier = tokens[position + 3]
            if tokens[position + 1] == "real":
                reals.add(identifier)
            name = re.sub(r"\[.*\]$", "", tokens[position + 4]).lower()
            # Enzan flattens a generate block into the design unit it is in, and names what it
            # declares `g(0).u`; GHDL nests a scope for it. No VHDL name has a dot, so a dot is
            # a scope either way.
            path = "/".join([*scopes, name]).replace(".", "/")
            names.setdefault(identifier, []).append(path)
            widths[identifier] = width
            position = tokens.index("$end", position) + 1
        elif token in ("$date", "$version", "$timescale", "$comment"):
            position = tokens.index("$end", position) + 1
        elif token.startswith("#"):
            time = int(token[1:])
            position += 1
        elif token[0] in "bBrR":
            record(tokens[position + 1], token[1:])
            position += 2
        elif token.startswith("$"):
            position += 1
        else:
            record(token[1:], token[0])
            position += 1

    return {name: changes.get(identifier, []) for identifier, paths in names.items()
            for name in paths}


# GHDL: `file:line:col:@time:(report note): message` or `(assertion error): message`.
GHDL_REPORT = re.compile(r":\((?:report|assertion) (note|warning|error|failure)\): (.*)$")

# Enzan: `file:line:col: note: message [ENZ5006]`, or without a position for one in a library.
ENZAN_REPORT = re.compile(r"(?:^|: )(note|warning|error): (.*) \[ENZ(5004|5005|5006|5012)\]$")
ENZAN_SEVERITY = {"5006": "note", "5005": "warning", "5004": "error", "5012": "failure"}


# GHDL's last line, when the design ended the run: `simulation finished @3025ns`.
GHDL_FINISHED = re.compile(r"simulation finished @(\d+)(fs|ps|ns|us|ms|sec)")
FEMTOSECONDS = {"fs": 1, "ps": 10**3, "ns": 10**6, "us": 10**9, "ms": 10**12, "sec": 10**15}


def finish_time(text: str) -> int | None:
    """When GHDL's run finished, in femtoseconds, or nothing when it did not say."""
    match = GHDL_FINISHED.search(text)
    return int(match.group(1)) * FEMTOSECONDS[match.group(2)] if match else None


def ghdl_reports(text: str) -> list[tuple[str, str]]:
    return [(match.group(1), match.group(2)) for line in text.splitlines()
            if (match := GHDL_REPORT.search(line))]


def enzan_reports(text: str) -> list[tuple[str, str]]:
    return [(ENZAN_SEVERITY[match.group(3)], match.group(2)) for line in text.splitlines()
            if (match := ENZAN_REPORT.search(line))]


def ghdl_output(text: str) -> list[str]:
    return [line for line in text.splitlines()
            if not GHDL_REPORT.search(line) and not line.startswith("ghdl:")
            and not GHDL_FINISHED.search(line)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    # A design too large to dump, as an exit design is, is compared without its waveforms.
    parser.add_argument("--enzan-vcd", type=Path)
    parser.add_argument("--ghdl-vcd", type=Path)
    parser.add_argument("--enzan-transcript", type=Path, required=True)
    parser.add_argument("--ghdl-transcript", type=Path, required=True)
    parser.add_argument("--enzan-output", type=Path, required=True)
    arguments = parser.parse_args()

    differences: list[str] = []

    expected = ghdl_reports(arguments.ghdl_transcript.read_text())
    actual = enzan_reports(arguments.enzan_transcript.read_text())
    if expected != actual:
        differences.append("the reports differ:")
        differences.extend(f"  GHDL:  {severity}: {message}" for severity, message in expected)
        differences.extend(f"  Enzan: {severity}: {message}" for severity, message in actual)

    written = ghdl_output(arguments.ghdl_transcript.read_text())
    printed = arguments.enzan_output.read_text().splitlines()
    if written != printed:
        differences.append("the output differs:")
        differences.extend(f"  GHDL:  {line}" for line in written)
        differences.extend(f"  Enzan: {line}" for line in printed)

    if (arguments.enzan_vcd is None) != (arguments.ghdl_vcd is None):
        parser.error("--enzan-vcd and --ghdl-vcd go together")
    if arguments.enzan_vcd is None:
        for line in differences:
            print(line, file=sys.stderr)
        return 1 if differences else 0

    reference = read_vcd(arguments.ghdl_vcd)
    candidate = read_vcd(arguments.enzan_vcd)
    finished = finish_time(arguments.ghdl_transcript.read_text())
    if finished is not None:
        for histories in (reference, candidate):
            for path, history in histories.items():
                histories[path] = [change for change in history if change[0] < finished]
    for path, history in sorted(reference.items()):
        if path not in candidate:
            differences.append(f"{path}: GHDL records it and Enzan does not")
            continue
        if candidate[path] != history:
            first = next((index for index, (want, got) in enumerate(zip(history, candidate[path]))
                          if want != got), min(len(history), len(candidate[path])))
            want = history[first] if first < len(history) else "nothing"
            got = candidate[path][first] if first < len(candidate[path]) else "nothing"
            differences.append(f"{path}: change {first} is {got} under Enzan and {want} under "
                               "GHDL")

    for line in differences:
        print(line, file=sys.stderr)
    return 1 if differences else 0


if __name__ == "__main__":
    sys.exit(main())
