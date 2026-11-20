# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixed cocotb callback workload, shared by Enzan and the reference simulator."""

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import ClockCycles, FallingEdge, ReadOnly, RisingEdge


CYCLES = 10000


@cocotb.test()
async def count_with_python_callbacks(dut):
    dut.rst.value = 1
    dut.enable.value = 1
    cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start(start_high=False))
    await ClockCycles(dut.clk, 2)
    await FallingEdge(dut.clk)
    dut.rst.value = 0

    for _ in range(CYCLES):
        await RisingEdge(dut.clk)

    await ReadOnly()
    actual = int(dut.count.value)
    assert actual == CYCLES % 256, f"expected {CYCLES % 256}, got {actual}"
    dut._log.info("benchmark cycles=%d count=%d", CYCLES, actual)
