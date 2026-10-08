# SPDX-License-Identifier: GPL-3.0-or-later
#
# The VPI behaviour cocotb relies on, pinned against a design Enzan can analyse
# (DESIGN_GUIDE.md section 14.6): cocotb's own suite mostly cannot reach VPI until M5, and this
# can whatever its pass rate is.

import cocotb
from cocotb.clock import Clock
from cocotb.handle import Force, Release
from cocotb.triggers import ClockCycles, FallingEdge, ReadOnly, RisingEdge, Timer
from cocotb.utils import get_sim_time


async def reset(dut):
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    dut.enable.value = 1
    dut.rst.value = 1
    await ClockCycles(dut.clk, 2)
    dut.rst.value = 0


@cocotb.test()
async def the_design_is_seen_as_it_was_written(dut):
    assert dut._name == "counter"
    assert len(dut.count) == 8
    assert {"clk", "rst", "enable", "count"} <= {handle._name for handle in dut}


@cocotb.test()
async def counts_on_each_rising_edge(dut):
    await reset(dut)
    for expected in range(1, 6):
        await RisingEdge(dut.clk)
        await ReadOnly()
        assert int(dut.count.value) == expected


@cocotb.test()
async def a_bit_has_edges_of_its_own(dut):
    # Bit 0 of a counter rises every other cycle and bit 1 every fourth; a watcher of the bit
    # that heard every change of the vector would see every cycle.
    await reset(dut)
    await RisingEdge(dut.count[0])
    first = get_sim_time("ns")
    await RisingEdge(dut.count[0])
    assert get_sim_time("ns") - first == 20

    await RisingEdge(dut.count[1])
    first = get_sim_time("ns")
    await RisingEdge(dut.count[1])
    assert get_sim_time("ns") - first == 40


@cocotb.test()
async def a_force_holds_until_released(dut):
    await reset(dut)
    await FallingEdge(dut.clk)
    dut.count.value = Force(100)
    await ClockCycles(dut.clk, 3)
    await ReadOnly()
    assert int(dut.count.value) == 100

    await FallingEdge(dut.clk)
    dut.count.value = Release()
    await RisingEdge(dut.clk)
    await ReadOnly()
    assert int(dut.count.value) == 101


@cocotb.test()
async def a_timer_alone_keeps_time_moving(dut):
    # Nothing in the design is scheduled while the clock is stopped, so only the pending timer
    # keeps the simulation alive (section 9.3).
    start = get_sim_time("ns")
    await Timer(15, unit="ns")
    assert get_sim_time("ns") - start == 15
