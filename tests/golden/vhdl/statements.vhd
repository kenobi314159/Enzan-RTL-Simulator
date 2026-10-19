-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Sequential statements and the predefined operators on scalars. What each check expects is what
-- GHDL 4.1 gives.

entity tb is
end entity;

architecture test of tb is
  type state_t is (idle, busy, done);
  signal state : state_t := idle;
  signal tick : bit := '0';
  signal steps : natural := 0;
begin
  -- Stops by assigning the clock its own value, which deletes the toggle already scheduled: the
  -- last event is at 110 ns, not 120 (IEEE 1076 section 10.5.2.2).
  tick <= not tick after 10 ns when steps < 6 else tick;

  fsm : process (tick)
  begin
    if tick = '1' then
      case state is
        when idle => state <= busy;
        when busy => state <= done;
        when done => state <= idle;
      end case;
      steps <= steps + 1;
    end if;
  end process;

  check : process
    variable total : integer := 0;
    variable count : integer;
    variable flag : boolean := false;
  begin
    for i in 1 to 10 loop
      next when i mod 2 = 0;
      exit when i > 7;
      total := total + i;
    end loop;
    assert total = 16 report "for: " & integer'image(total) severity failure;

    count := 3;
    while count > 0 loop
      count := count - 1;
    end loop;
    assert count = 0 report "while" severity failure;

    for i in 3 downto 1 loop
      total := total - i;
    end loop;
    assert total = 10 report "downto" severity failure;

    -- The range is evaluated once: assigning its bound's variable leaves the iterations as many.
    count := 4;
    total := 0;
    for i in 1 to count loop
      count := 1;
      total := total + 1;
    end loop;
    assert total = 4 report "range evaluated once: " & integer'image(total) severity failure;

    assert (-7) mod 3 = 2 and (-7) rem 3 = -1 report "static mod" severity failure;
    count := -7;
    assert count mod 3 = 2 and count rem 3 = -1 report "mod" severity failure;
    assert 7 mod (-3) = -2 report "mod of a negative divisor" severity failure;
    assert abs count = 7 report "abs" severity failure;
    assert minimum(count, 4) = -7 and maximum(count, 4) = 4 report "minimum" severity failure;

    -- The right operand is not evaluated when the left one decides: it would divide by zero.
    assert not (flag and (1 / (count + 7) = 0)) report "short circuit" severity failure;

    wait until state = done;
    assert now = 30 ns report "done at " & time'image(now) severity failure;
    wait for 5 ns;
    assert now = 35 ns report "wait for" severity failure;
    wait until steps = 6;
    assert now = 110 ns report "six steps at " & time'image(now) severity failure;
    assert state = idle report "state " & integer'image(state_t'pos(state)) severity failure;

    wait for 50 ns;
    assert tick = '1' and steps = 6 report "the clock did not stop" severity failure;
    report "PASS statements";
    std.env.finish;
  end process;
end architecture;
