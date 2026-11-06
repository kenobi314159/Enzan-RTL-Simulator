#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Holds a log NEORV32's testbench wrote around the netlist to the one it wrote around the RTL.

    compare-logs.py <rtl-log> <netlist-log>

The two must be the same line for line, but where the RTL's shows a value as unknown, `0x????????`
in a CPU trace: a register no instruction has written yet, which the RTL's register file holds as
unknown and the netlist's, built of AMD's RAM primitives, as they start, 0. The netlist's may show
any value there. Exits 1, saying where, at the first other difference.
"""

import re
import sys


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit("usage: compare-logs.py <rtl-log> <netlist-log>")
    with open(sys.argv[1], encoding="utf-8") as rtl_file:
        rtl = rtl_file.read().splitlines()
    with open(sys.argv[2], encoding="utf-8") as netlist_file:
        netlist = netlist_file.read().splitlines()
    for number, (expected, got) in enumerate(zip(rtl, netlist), start=1):
        pattern = re.escape(expected).replace(re.escape("????????"), "[0-9a-f?]{8}")
        if re.fullmatch(pattern, got) is None:
            print(f"line {number} differs:\n  RTL:     {expected}\n  netlist: {got}",
                  file=sys.stderr)
            sys.exit(1)
    if len(rtl) != len(netlist):
        print(f"the RTL's log has {len(rtl)} lines and the netlist's {len(netlist)}",
              file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
