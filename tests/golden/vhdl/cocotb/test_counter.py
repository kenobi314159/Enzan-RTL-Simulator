# SPDX-License-Identifier: GPL-3.0-or-later
#
# The VHPI behaviour cocotb relies on, pinned against a VHDL design Enzan can analyse, as
# test_counter.py beside the SystemVerilog design pins VPI's.

import cocotb
from cocotb.clock import Clock
from cocotb.handle import Force, Release
from cocotb.triggers import ClockCycles, FallingEdge, ReadOnly, RisingEdge, Timer
from cocotb.utils import get_sim_time


async def reset(dut):
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    dut.enable.value = 1
    dut.rst.value = 1
    dut.scale.value = 0.5
    await ClockCycles(dut.clk, 2)
    dut.rst.value = 0


@cocotb.test()
async def the_design_is_seen_as_it_was_written(dut):
    assert dut._name.lower() == "counter"
    assert len(dut.count) == 8
    names = {handle._name.lower() for handle in dut}
    assert {"clk", "rst", "enable", "count", "total", "mode", "pair", "width"} <= names
    assert int(dut.WIDTH.value) == 8


@cocotb.test()
async def counts_on_each_rising_edge(dut):
    await reset(dut)
    for expected in range(1, 6):
        await RisingEdge(dut.clk)
        await ReadOnly()
        assert int(dut.count.value) == expected
        assert int(dut.total.value) == expected
        assert str(dut.count.value)[-1] in "01"


@cocotb.test()
async def values_of_each_kind_read_back(dut):
    await reset(dut)
    await ClockCycles(dut.clk, 3)
    await ReadOnly()
    assert int(dut.total.value) == 3
    assert dut.scaled.value == 1.5
    assert not bool(dut.wrapped.value)
    assert str(dut.mode.value).lower() in ("counting", "1")
    assert int(dut.pair.low.value) == 3
    assert str(dut.pair.flag.value) == "1"


@cocotb.test()
async def a_string_is_written_and_read(dut):
    dut.greeting.value = b"hello"
    await Timer(1, unit="ns")
    assert dut.greeting.value == b"hello"


@cocotb.test()
async def a_bit_has_edges_of_its_own(dut):
    await reset(dut)
    await RisingEdge(dut.count[0])
    first = get_sim_time("ns")
    await RisingEdge(dut.count[0])
    assert get_sim_time("ns") - first == 20


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
    assert int(dut.count.value) != 100


@cocotb.test()
async def a_timer_alone_keeps_time_moving(dut):
    start = get_sim_time("ns")
    await Timer(15, unit="ns")
    assert get_sim_time("ns") - start == 15
