# Mixed-language designs

How a design of both SystemVerilog and VHDL is simulated: what crosses between the languages,
what the crossing costs, and what does not cross yet.

## Running one

Name the files of both languages in one invocation. A file ending in `.vhd` or `.vhdl` is VHDL,
analysed into the library `--work` names (`work` by default); every other file is SystemVerilog.
The top may be of either language.

```sh
enzan --top tb_aes_core_vhdl tb_aes_core.vhd aes_core.v    # VHDL around SystemVerilog
enzan --top neorv32_tb -f rtl.f models.vhd neorv32_tb.v   # SystemVerilog around VHDL
```

The build needs both frontends; `enzan --version` lists them under *Optional components*.

## Instances across the boundary

**SystemVerilog instantiating a VHDL entity** writes the instance as it would a module's, by the
entity's name, with its generics as parameters. The entity's default architecture is the one
simulated: choosing another takes a `config`, which Enzan does not read.

**VHDL instantiating a SystemVerilog module** declares a component of the module's name, with its
parameters as generics, and instantiates the component. With no entity of that name to bind it
to, the component is bound to the module. A name VHDL reserves is written as an extended
identifier: `\next\` names the module's port `next`.

Each instance is specialised by its values, whichever language set them. A VHDL port whose width
depends on generics takes the width a translation for the instance's values gives it, which takes
the analysis a second round; a hierarchy that keeps asking for new specialisations through both
languages is stopped after eight rounds (ENZ6030).

A parameter or a generic crosses when it is an integer, a `bit`, `boolean` or `std_ulogic`, a
vector of these, a string, or a real; a `boolean` meeting a parameter that takes a string crosses
as `"TRUE"` or `"FALSE"`, which is how vendors' models write one. Any other kind of generic set
from SystemVerilog is refused (ENZ2005).

## Ports across the boundary

**Each crossing costs one delta cycle.** A port whose two sides are of different languages is
connected by a zero-delay assignment that converts the value. Its far side changes one delta
cycle after its near side, in each direction the port's mode needs.

A synchronous design does not see the delta: a clock that crosses arrives a delta late, and so does
every value launched by its edge on the other side. What does see it is a clock that crosses and
comes back, or a clock that reaches registers by two routes, one crossing more often than the
other. A register clocked through the longer route samples its data after the shorter route's
registers have updated it:

```vhdl
-- pass is a SystemVerilog module: assign o = i;
u : pass port map (i => clk, o => clk_back);
process (clk)      begin if rising_edge(clk)      then q1 <= d;  end if; end process;
process (clk)      begin if rising_edge(clk)      then q3 <= q1; end if; end process; -- old q1
process (clk_back) begin if rising_edge(clk_back) then q2 <= q1; end if; end process; -- new q1
```

`clk_back` changes two delta cycles after `clk`, and `q1` one after, so `q2` takes the value `q1`
has just taken, a cycle early. It is the hazard of a VHDL clock passed through a signal
assignment. Clock every register from the same side of the boundary, or give a clock that has to
come back a delay.

**Values** convert as follows:

| VHDL | SystemVerilog |
|---|---|
| `std_ulogic`, `std_logic` | a four-state bit: `'0'`, `'L'` → `0`; `'1'`, `'H'` → `1`; `'Z'` → `z`; `'X'`, `'W'`, `'U'`, `'-'` → `x`. Back, `0`, `1`, `x`, `z` → `'0'`, `'1'`, `'X'`, `'Z'` |
| `bit`, `boolean` | a two-state bit |
| `std_logic_vector`, `unsigned`, `bit_vector` and their kin | a packed vector, the left element its most significant bit |
| an integer type | `int`, or `longint` when its range needs it |
| an array of these | an unpacked array |
| a record of these | a packed struct, field by field |

**Strength crosses into VHDL, not out of it.** A value SystemVerilog drives weakly, a pull-up's,
reaches VHDL as `'L'`, `'H'` or `'W'` when every driver of the port in its module drives it weakly;
one an instance inside drives, or a process writes, crosses as strong. A weak VHDL value reaches
SystemVerilog as a strong one, `'H'` as `1`, so a VHDL `'H'` meeting a SystemVerilog driver of `0`
on one net is a conflict there, `x`, where it would have resolved to `0` in VHDL. Model pulls on
the side that resolves them: a `tri1` or `tri0` net or a `pullup` in SystemVerilog, an `'H'` or
`'L'` driver in VHDL.

An `inout` port converts both ways. The inner side's own drivers, resolved, are one driver of the
outer signal, which resolves them with its other drivers by its own language's rule; the outer
signal's value is what the inner side reads. A port released to `'Z'` on one side therefore lets
the other side's drivers through.

A VHDL port that a SystemVerilog instance leaves unconnected keeps its default value, as a VHDL
port left `open` does, and Enzan warns that it does (ENZ3019).

## What does not cross yet

Each is refused by name rather than simulated wrongly:

- a port of a real type, an enumeration other than `std_ulogic`, `bit` and `boolean`, or a
  physical type such as `time` (ENZ3016);
- a VHDL port of an unconstrained type, such as `std_logic_vector` with no range, instantiated
  from SystemVerilog, where nothing gives it bounds (ENZ2005);
- `entity work.m` naming a SystemVerilog module: declare a component instead;
- choosing an architecture other than the default from SystemVerilog;
- storing a design of both languages in a library (`--out-lib`), and `-G` for one.

## VPI and VHPI

The two interfaces share one design, and each shows its own language:

- VPI finds no VHDL object by name. An iteration that meets a VHDL instance returns it named,
  as a `vpiUnknown`.
- VHPI shows a SystemVerilog region as one of cocotb's class `vhpiVerilog`. Under a SystemVerilog
  top there is no VHPI root instance.
- A name either is asked for may begin in the other language's hierarchy, and its VHDL part is
  matched without regard to case.

This is what cocotb expects of a simulator of both languages. It takes a handle VPI cannot type and
asks VHPI for it by name. For cocotb, Enzan's makefile (see [cocotb](cocotb.md)) loads the toplevel
language's library with `--vpi` or `--vhpi` and names the other in `GPI_EXTRA`.

## Diagnostics

| Code | Meaning |
|---|---|
| ENZ2005 | a generic set from SystemVerilog, or a port of an unconstrained type, that the VHDL frontend cannot take |
| ENZ3016 | a port whose type no conversion covers |
| ENZ3017 | a module has no port of the name the instance of the other language connects |
| ENZ3018 | a port's direction differs from the one the instance of the other language declares |
| ENZ3019 | a VHDL port left unconnected by SystemVerilog keeps its default |
| ENZ6030 | the two languages still ask each other for units after eight rounds |
