-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Records: signals, variables and ports of them, constants, aggregates by position, by name and
-- with others, fields read and assigned, equality, and an array of records. What each check
-- expects is what GHDL 4.1 gives.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

package bus_pkg is
  type request_t is record
    addr : std_ulogic_vector(7 downto 0);
    data : std_ulogic_vector(15 downto 0);
    we : std_ulogic;
    valid : std_ulogic;
  end record;

  constant idle_c : request_t := (addr => (others => '0'), data => (others => '0'),
                                  we => '0', valid => '0');

  type response_t is record
    data : std_ulogic_vector(15 downto 0);
    ack : std_ulogic;
  end record;

  type table_t is array (0 to 3) of response_t;
end package;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.bus_pkg.all;

entity responder is
  port (clk : in std_ulogic; req : in request_t; rsp : out response_t);
end entity;

architecture rtl of responder is
  signal store : table_t := (others => (data => x"0000", ack => '0'));
begin
  process (clk)
    variable slot : integer;
  begin
    if rising_edge(clk) then
      rsp.ack <= '0';
      if req.valid = '1' then
        slot := to_integer(unsigned(req.addr(1 downto 0)));
        if req.we = '1' then
          store(slot).data <= req.data;
        else
          rsp <= (data => store(slot).data, ack => '1');
        end if;
      end if;
    end if;
  end process;
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use work.bus_pkg.all;

entity tb is
end entity;

architecture test of tb is
  signal clk : std_ulogic := '0';
  signal req : request_t := idle_c;
  signal rsp : response_t;
begin
  uut : entity work.responder port map (clk => clk, req => req, rsp => rsp);

  clk <= not clk after 5 ns;

  process
    variable r : request_t;
  begin
    r := idle_c;
    assert r = idle_c report "record constant" severity failure;
    r.addr := x"02";
    r := (r.addr, x"beef", '1', '1');
    assert r.data = x"beef" and r.addr = x"02" report "positional aggregate" severity failure;
    assert r /= idle_c report "record inequality" severity failure;

    wait until rising_edge(clk);
    req <= r;
    wait until rising_edge(clk);
    req <= (addr => x"02", data => x"0000", others => '0');
    req.valid <= '1';
    wait until rising_edge(clk);
    req <= idle_c;
    wait until rising_edge(clk);
    assert rsp.data = x"beef" and rsp.ack = '1' report "read back" severity failure;
    wait until rising_edge(clk);
    assert rsp.ack = '0' report "ack held" severity failure;
    report "PASS records";
    std.env.finish;
  end process;
end architecture;
