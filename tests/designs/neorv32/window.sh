# SPDX-License-Identifier: GPL-3.0-or-later
#
# What a run of NEORV32's testbench is, sourced by each harness that runs it: how long it runs,
# through the end of the testbench's on-chip debugger sequence, which halts, programs, resets and
# resumes the CPU over JTAG; the logs it writes, each core's CPU trace and what each UART received;
# and the simulation models of NEORV32's sim/ that its VHDL testbench needs.
readonly stop_us=160
readonly logs=(neorv32_trace0.log neorv32_trace1.log tb.uart0_rx.log tb.uart1_rx.log)
readonly sim_models=(jtag_dmi_pkg.vhd sim_uart_rx.vhd xbus_memory.vhd xbus_fmem.vhd xbus_gateway.vhd
                   psram_model.vhd)
