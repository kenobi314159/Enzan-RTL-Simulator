# SPDX-License-Identifier: GPL-3.0-or-later

import cocotb
from cocotb.triggers import Timer


@cocotb.test()
async def named_vhdl_library_reaches_the_top(dut):
    await Timer(1, unit="ns")
    assert dut.q.value.to_unsigned() == 15
