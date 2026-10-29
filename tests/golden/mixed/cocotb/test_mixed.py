# SPDX-License-Identifier: GPL-3.0-or-later
#
# What cocotb relies on in a design of both languages: each interface shows its own language,
# cocotb hands a unit of the other to the other interface by its name, and values, deposits and
# value changes work through either.

import os

import cocotb
from cocotb.clock import Clock
from cocotb.handle import GPIDiscovery
from cocotb.triggers import ClockCycles, ReadOnly, RisingEdge

# The counter of the toplevel's language, and the one cocotb reaches through the other interface.
if os.environ.get("TOPLEVEL_LANG", "verilog") == "verilog":
    NATIVE, FOREIGN = "u_sv", "u_vhdl"
else:
    NATIVE, FOREIGN = "u_vhdl", "u_sv"


async def reset(dut):
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    dut.rst.value = 1
    await ClockCycles(dut.clk, 2)
    dut.rst.value = 0


@cocotb.test()
async def each_interface_shows_its_own_language(dut):
    # The toplevel's interface does not find a unit of the other language by its name, and cocotb
    # finds it through the other; iterating the toplevel meets both.
    assert dut._get(FOREIGN, GPIDiscovery.NATIVE) is None
    assert dut._get(FOREIGN, GPIDiscovery.AUTO) is not None
    assert dut._get(NATIVE, GPIDiscovery.NATIVE) is not None
    assert {"u_sv", "u_vhdl"} <= {handle._name.lower() for handle in dut}


@cocotb.test()
async def both_counters_count_from_one_clock(dut):
    await reset(dut)
    for expected in range(1, 6):
        await RisingEdge(dut.clk)
        await ReadOnly()
        assert int(dut.u_sv.count.value) == expected
        assert int(dut.u_vhdl.count.value) == expected


@cocotb.test()
async def a_deposit_and_a_value_change_cross_into_either_language(dut):
    # Each counter's limit lowered by a deposit, and its wrap waited for on its own signal, in the
    # interface its language has.
    await reset(dut)
    dut.u_sv.limit.value = 3
    dut.u_vhdl.Limit.value = 5
    await RisingEdge(dut.u_sv.wrapped)
    await ReadOnly()
    assert int(dut.u_sv.count.value) == 3
    await RisingEdge(dut.u_vhdl.wrapped)
    await ReadOnly()
    assert int(dut.u_vhdl.count.value) == 5
