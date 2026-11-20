-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- A VHDL real port between the SystemVerilog testbench and RealGain (real_ports.sv).

entity real_stage is
    port ( value : in real;
           result : out real );
end entity;

architecture rtl of real_stage is
    component RealGain is
        port ( value : in real;
               result : out real );
    end component;

    signal gained : real;
begin
    gain: RealGain port map ( value => value, result => gained );
    result <= gained + 2.0;
end architecture;
