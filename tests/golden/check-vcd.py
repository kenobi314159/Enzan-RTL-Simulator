#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate that a VCD file is well formed.

A waveform has to open in GTKWave. A viewer needs a display and cannot run in CI, so this checks the
properties a viewer relies on instead, which is both automatable and stricter than "it opened" — a
viewer will happily skip a line it does not understand, which is exactly how a malformed writer goes
unnoticed:

* the header declares a timescale and ends with ``$enddefinitions``;
* every ``$scope`` is closed by an ``$upscope``, and none is closed twice;
* every identifier used in a value change was declared by a ``$var``;
* every change of bits has the width its ``$var`` declared;
* a real changes by a number, ``r2.5 !``, and nothing else does;
* two ``$var`` declarations sharing an identifier — which is how a collapsed port appears — agree
  about the width and about whether it is a real;
* timestamps never go backwards, which a viewer's time index depends on;
* there is a timestamp and at least one value change.

VCD is a stream of whitespace-delimited tokens, not a line-based format: a ``$date`` or
``$version`` section may span several lines, and ``fst2vcd`` writes them that way. This parses
tokens for that reason — the first version of this checker was line-based and rejected GTKWave's
own output.

Exits non-zero with a message naming the line when anything fails.
"""

from __future__ import annotations

import sys
from pathlib import Path

SCALAR_VALUES = set("01xXzZ")

# The variable types whose values are numbers rather than bits. A real's declared width says
# nothing about its changes: Enzan declares one as 64 bits, Icarus Verilog as 1.
REAL_TYPES = {"real", "realtime"}

# Sections whose contents are free text to be skipped rather than parsed.
TEXT_SECTIONS = {"$date", "$version", "$comment", "$timescale"}

# Sections whose contents are value changes, so they are parsed as such.
DUMP_SECTIONS = {"$dumpvars", "$dumpall", "$dumpon", "$dumpoff"}


class Checker:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.tokens: list[tuple[str, int]] = []
        for number, line in enumerate(path.read_text().splitlines(), start=1):
            self.tokens.extend((token, number) for token in line.split())
        self.position = 0

        self.widths: dict[str, int] = {}
        self.reals: set[str] = set()
        self.scope_depth = 0
        self.saw_timescale = False
        self.saw_enddefinitions = False
        self.last_time: int | None = None
        self.changes = 0

    def fail(self, line: int, message: str) -> None:
        print(f"{self.path.name}:{line}: {message}", file=sys.stderr)
        raise SystemExit(1)

    def next_token(self) -> tuple[str, int] | None:
        if self.position >= len(self.tokens):
            return None
        token = self.tokens[self.position]
        self.position += 1
        return token

    def expect(self, wanted: str, line: int) -> None:
        token = self.next_token()
        if token is None:
            self.fail(line, f"file ended where {wanted} was expected")
        if token[0] != wanted:
            self.fail(token[1], f"expected {wanted}, found {token[0]!r}")

    def skip_to_end(self, line: int) -> list[str]:
        """Consumes tokens up to and including ``$end``, returning what was skipped."""
        skipped: list[str] = []
        while True:
            token = self.next_token()
            if token is None:
                self.fail(line, "file ended before $end")
            if token[0] == "$end":
                return skipped
            skipped.append(token[0])

    def declare_var(self, line: int) -> None:
        fields = self.skip_to_end(line)
        if len(fields) < 4:
            self.fail(line, f"$var needs a type, width, identifier and name, got {fields}")

        try:
            width = int(fields[1])
        except ValueError:
            self.fail(line, f"$var width {fields[1]!r} is not a number")
        identifier = fields[2]
        real = fields[0] in REAL_TYPES

        # Several $var declarations may share an identifier — a collapsed port is one piece of
        # storage under two names — but they must agree about the width and about being a real.
        if identifier in self.widths and self.widths[identifier] != width:
            self.fail(line, f"identifier {identifier!r} declared with two widths")
        if identifier in self.widths and (identifier in self.reals) != real:
            self.fail(line, f"identifier {identifier!r} declared both as a real and as bits")
        self.widths[identifier] = width
        if real:
            self.reals.add(identifier)

    def record_change(self, token: str, line: int) -> None:
        if token[0] in "bB":
            bits = token[1:]
            if not bits:
                following = self.next_token()
                if following is None:
                    self.fail(line, "a vector change with no value")
                bits = following[0]
            identifier_token = self.next_token()
            if identifier_token is None:
                self.fail(line, "a vector change with no identifier")
            identifier = identifier_token[0]

            if identifier not in self.widths:
                self.fail(line, f"value change for undeclared identifier {identifier!r}")
            if identifier in self.reals:
                self.fail(line, f"{identifier!r} is a real but was written as bits")
            if len(bits) != self.widths[identifier]:
                self.fail(
                    line,
                    f"{identifier!r} is {self.widths[identifier]} bits "
                    f"but the change has {len(bits)}",
                )
            if any(character not in SCALAR_VALUES for character in bits):
                self.fail(line, f"invalid bit in {bits!r}")
            self.changes += 1
            return

        if token[0] in "rR":
            number = token[1:]
            if not number:
                following = self.next_token()
                if following is None:
                    self.fail(line, "a real change with no value")
                number = following[0]
            identifier_token = self.next_token()
            if identifier_token is None:
                self.fail(line, "a real change with no identifier")
            identifier = identifier_token[0]

            if identifier not in self.widths:
                self.fail(line, f"value change for undeclared identifier {identifier!r}")
            if identifier not in self.reals:
                self.fail(line, f"{identifier!r} is not a real but was written as a number")
            try:
                float(number)
            except ValueError:
                self.fail(line, f"invalid real {number!r}")
            self.changes += 1
            return

        # A scalar change is the value and the identifier with no separator.
        identifier = token[1:]
        if not identifier:
            self.fail(line, f"scalar change {token!r} has no identifier")
        if identifier not in self.widths:
            self.fail(line, f"value change for undeclared identifier {identifier!r}")
        if identifier in self.reals:
            self.fail(line, f"{identifier!r} is a real but was written as a bit")
        if self.widths[identifier] != 1:
            self.fail(line, f"{identifier!r} is a vector but was written as a scalar")
        self.changes += 1

    def run(self) -> None:
        while True:
            entry = self.next_token()
            if entry is None:
                break
            token, line = entry

            if token in TEXT_SECTIONS:
                if token == "$timescale":
                    self.saw_timescale = True
                self.skip_to_end(line)
                continue

            if token in DUMP_SECTIONS:
                # The contents are value changes, so they fall through to the ordinary handling
                # below; only the closing $end needs recognising, which it is as a stray keyword.
                continue

            if token == "$scope":
                self.skip_to_end(line)
                self.scope_depth += 1
                continue

            if token == "$upscope":
                self.expect("$end", line)
                self.scope_depth -= 1
                if self.scope_depth < 0:
                    self.fail(line, "$upscope without a matching $scope")
                continue

            if token == "$var":
                self.declare_var(line)
                continue

            if token == "$enddefinitions":
                self.expect("$end", line)
                if self.scope_depth != 0:
                    self.fail(line, f"{self.scope_depth} scope(s) still open at $enddefinitions")
                self.saw_enddefinitions = True
                continue

            if token == "$end":
                # Closes a dump section, which has no other terminator.
                continue

            if token.startswith("$"):
                # An unknown keyword section. Skipped rather than rejected, so that a writer using
                # a VCD extension this checker predates does not fail the suite.
                self.skip_to_end(line)
                continue

            if token.startswith("#"):
                try:
                    time = int(token[1:])
                except ValueError:
                    self.fail(line, f"malformed timestamp {token!r}")
                if self.last_time is not None and time < self.last_time:
                    self.fail(line, f"time went backwards, {self.last_time} then {time}")
                self.last_time = time
                continue

            if token[0] in SCALAR_VALUES or token[0] in "bBrR":
                self.record_change(token, line)
                continue

            self.fail(line, f"unrecognised token {token!r}")

        if not self.saw_timescale:
            self.fail(0, "no $timescale in the header")
        if not self.saw_enddefinitions:
            self.fail(0, "no $enddefinitions")
        if self.scope_depth != 0:
            self.fail(0, f"{self.scope_depth} scope(s) left open at end of file")
        if self.last_time is None:
            self.fail(0, "no timestamps at all")
        if not self.widths:
            self.fail(0, "no signals were declared")
        if self.changes == 0:
            self.fail(0, "no value changes were recorded")

        print(
            f"{self.path.name}: {len(self.widths)} signals, {self.changes} value changes, "
            f"ends at #{self.last_time}"
        )


def main() -> None:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <file.vcd>", file=sys.stderr)
        raise SystemExit(2)
    Checker(Path(sys.argv[1])).run()


if __name__ == "__main__":
    main()
