#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# VHDL in Enzan's libraries (DESIGN_GUIDE.md sections 4.4 and 8.2), driven as a Makefile drives
# them: a package and a counter analysed into one library, a testbench analysed against it into
# another, and runs from both. What a library is for is that one process analyses and another
# runs, so it cannot be checked from inside one invocation.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <enzan>" >&2
    exit 2
fi

enzan=$1

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"

fail() {
    echo "FAIL vhdl library: $1" >&2
    exit 1
}

# Runs a command and returns its status without `set -e` ending the script first, which is
# what every check for a refusal below needs.
status_of() {
    local status=0
    "$@" >/dev/null 2>&1 || status=$?
    echo "$status"
}

# When NVC's analysis of a unit was last written, which an analysis that reuses it leaves alone.
written() {
    stat -c %Y "$1"
}

cat >pkg.vhd <<'EOF'
package ip_pkg is
    constant WIDTH : integer := 8;
end package;
EOF

cat >counter.vhd <<'EOF'
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.ip_pkg.all;

entity counter is
    generic (W : integer := WIDTH);
    port (clk : in std_logic; q : out unsigned(W - 1 downto 0));
end entity;

architecture rtl of counter is
    signal count : unsigned(W - 1 downto 0) := (others => '0');
begin
    process (clk) begin
        if rising_edge(clk) then count <= count + 1; end if;
    end process;
    q <= count;
end architecture;
EOF

# A component the package declares, which an instance binds by default in the package's library.
cat >components.vhd <<'EOF'
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

package ip_components is
    component counter is
        generic (W : integer);
        port (clk : in std_logic; q : out unsigned(W - 1 downto 0));
    end component;
end package;
EOF

# Bound by default (IEEE 1076 section 7.3.3): a component a use clause makes an entity of its name
# directly visible for binds to that entity, and one a package of another library declares binds
# to the entity of its name there, rather than in the work library, which has neither.
cat >binding.vhd <<'EOF'
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
library ip;
use ip.counter;

entity binding is end entity;

architecture test of binding is
    component counter is
        generic (W : integer);
        port (clk : in std_logic; q : out unsigned(W - 1 downto 0));
    end component;
    signal clk : std_logic := '0';
    signal visible, declared : unsigned(3 downto 0);
begin
    u_visible: counter generic map (W => 4) port map (clk => clk, q => visible);
    u_declared: ip.ip_components.counter generic map (W => 4) port map (clk => clk, q => declared);
    process begin
        for i in 1 to 3 loop
            clk <= '1'; wait for 5 ns; clk <= '0'; wait for 5 ns;
        end loop;
        assert visible = 3 and declared = 3 report "an instance was left unbound" severity failure;
        report "PASS binding";
        wait;
    end process;
end architecture;
EOF

cat >tb.vhd <<'EOF'
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
library ip;
use ip.ip_pkg.all;

entity tb is end entity;

architecture test of tb is
    signal clk : std_logic := '0';
    signal q : unsigned(WIDTH - 1 downto 0);
begin
    u: entity ip.counter port map (clk => clk, q => q);
    process begin
        for i in 1 to 5 loop
            clk <= '1'; wait for 5 ns; clk <= '0'; wait for 5 ns;
        end loop;
        assert q = 5 report "q is " & integer'image(to_integer(q)) severity failure;
        report "PASS library";
        wait;
    end process;
end architecture;
EOF

# --- a library takes VHDL as it takes SystemVerilog ---------------------------------------------

"$enzan" lib create libs/ip >/dev/null || fail "libs/ip could not be created"
"$enzan" analyse --out-lib libs/ip pkg.vhd counter.vhd || fail "analysis into libs/ip failed"

listing=$("$enzan" lib list libs/ip)
grep -q 'counter' <<<"$listing" && grep -q 'ip_pkg' <<<"$listing" \
    || fail "the library does not list the units analysed into it: $listing"

# The library's name is the one a design refers to it by, so `--work` naming another is refused
# rather than writing units a design cannot find.
[[ $(status_of "$enzan" analyse --out-lib libs/ip --work other pkg.vhd) -eq 2 ]] \
    || fail "--work naming another library than --out-lib's was not refused"

# --- a design is analysed against a library and runs from it -------------------------------------

"$enzan" analyse --out-lib libs/ip components.vhd || fail "the package of components did not analyse"
binding=$("$enzan" --in-lib libs/ip --top binding binding.vhd 2>&1) \
    || fail "a design binding components to another library's entities failed: $binding"
grep -q 'PASS binding' <<<"$binding" || fail "default binding across libraries: $binding"

"$enzan" lib create --name tbl libs/tb >/dev/null || fail "libs/tb could not be created"
"$enzan" analyse --out-lib libs/tb --in-lib libs/ip tb.vhd \
    || fail "a testbench could not be analysed against libs/ip"

run_output=$("$enzan" run --in-lib libs/ip --in-lib libs/tb tb 2>&1) \
    || fail "a run from the libraries alone failed: $run_output"
grep -q 'PASS library' <<<"$run_output" || fail "the run from libraries did not reach its check"

one_shot=$("$enzan" --in-lib libs/ip --top tb tb.vhd 2>&1) \
    || fail "a one-shot run against libs/ip failed: $one_shot"
grep -q 'PASS library' <<<"$one_shot" || fail "the one-shot run did not reach its check"

# --- a source analysed already is not analysed again, and one edited is --------------------------

"$enzan" --out-lib libs/tb --in-lib libs/ip --top tb tb.vhd >/dev/null 2>&1 \
    || fail "a run into libs/tb failed"
before=$(written libs/tb/vhdl/TBL.TB)
sleep 1
"$enzan" --out-lib libs/tb --in-lib libs/ip --top tb tb.vhd >/dev/null 2>&1 \
    || fail "a second run into libs/tb failed"
[[ $(written libs/tb/vhdl/TBL.TB) -eq $before ]] \
    || fail "a source whose entry is current was analysed again"

# A unit analysed against a package is stale once the package is analysed again.
printf -- '-- edited\n' >>pkg.vhd
"$enzan" analyse --out-lib libs/ip pkg.vhd counter.vhd || fail "libs/ip could not be analysed again"
sleep 1
"$enzan" --out-lib libs/tb --in-lib libs/ip --top tb tb.vhd >/dev/null 2>&1 \
    || fail "a run into libs/tb after libs/ip changed failed"
[[ $(written libs/tb/vhdl/TBL.TB) -gt $before ]] \
    || fail "a source analysed against a library that changed was not analysed again"

# --- a top two libraries declare is named with its library ----------------------------------------

"$enzan" lib create --name other libs/other >/dev/null || fail "libs/other could not be created"
"$enzan" analyse --out-lib libs/other --in-lib libs/ip tb.vhd || fail "libs/other analysis"
[[ $(status_of "$enzan" run --in-lib libs/ip --in-lib libs/tb --in-lib libs/other tb) -eq 2 ]] \
    || fail "a top two libraries declare was not refused"
qualified=$("$enzan" run --in-lib libs/ip --in-lib libs/tb --in-lib libs/other other.tb 2>&1) \
    || fail "a top named with its library did not run: $qualified"
grep -q 'PASS library' <<<"$qualified" || fail "the qualified top did not reach its check"
[[ $(status_of "$enzan" run --in-lib libs/ip --in-lib libs/tb .tb) -eq 2 ]] \
    || fail "a top of an empty library name was not refused"

# --- without a top, analysis translates nothing and a run takes the one root ----------------------

# A wrapper nothing instantiates, whose default generic the adapter cannot compute, as OFM's MI
# splitter has one. An analysis that names no top stores it without translating it (5.2), and a
# run without a top is refused for having two candidates rather than translating both.
cat >picks.vhd <<'EOF'
package picks is
    function pick return integer;
end package;

package body picks is
    procedure choose(x : out integer) is
    begin
        x := 3;
    end procedure;

    function pick return integer is
        variable x : integer;
    begin
        choose(x);
        return x;
    end function;
end package body;

use work.picks.all;

entity wrapper is
    generic (BASE : integer := pick);
end entity;

architecture rtl of wrapper is
begin
end architecture;
EOF

cat >alone.vhd <<'EOF'
entity alone is end entity;

architecture test of alone is
begin
    process begin
        report "PASS alone";
        wait;
    end process;
end architecture;
EOF

"$enzan" lib create libs/roots >/dev/null || fail "libs/roots could not be created"
"$enzan" analyse --out-lib libs/roots picks.vhd alone.vhd \
    || fail "an analysis naming no top translated an entity nothing instantiates"
alone=$("$enzan" run --in-lib libs/roots alone 2>&1) || fail "a run of one root failed: $alone"
grep -q 'PASS alone' <<<"$alone" || fail "the run of one root did not reach its check"

refused=$("$enzan" picks.vhd alone.vhd 2>&1) && fail "a run of two roots and no top was not refused"
grep -q "ENZ3002" <<<"$refused" && grep -q "'alone', 'wrapper'" <<<"$refused" \
    || fail "a run of two roots and no top did not name them: $refused"
alone=$("$enzan" alone.vhd 2>&1) || fail "a run of its one root failed: $alone"
grep -q 'PASS alone' <<<"$alone" || fail "the one root was not taken for the top"

# --- cleaning a library empties it of VHDL too -----------------------------------------------------

"$enzan" lib clean libs/ip >/dev/null || fail "libs/ip could not be cleaned"
[[ ! -e libs/ip/vhdl ]] || fail "cleaning left NVC's units behind"
grep -q '0 units' <<<"$("$enzan" lib list libs/ip)" || fail "a cleaned library still lists units"

echo "PASS library"
