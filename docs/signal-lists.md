# Signal lists

A signal list is a file that records what the wave window shows, so that an arrangement of a
large design's signals survives from one run to the next and can be kept beside the design's
sources.

## Saving and loading

*File → Save Signal List* (Ctrl+S) writes every row of the wave window, top to bottom, to the
file the list was last loaded from or saved to, and asks for a name only the first time. *Save
Signal List As…* writes to another file. *Load Signal List…* replaces the window's list with a
file's. To add one list to another, copy and paste rows between windows.

To start a window with a list in place:

```sh
enzan run --gui --wave-list signals.tcl tb
enzan --gui --attach /tmp/enzan.sock --wave-list signals.tcl
```

`--wave-list` needs `--gui`, since without a window there is nothing to show the list in. A file
that cannot be read is reported, and the window comes up empty.

Adding signals from the Structure or Objects window writes the matching `add wave` command into
the transcript, ready to be copied into a file.

## Syntax

A signal list uses Tcl's syntax but is never run as Tcl. Quoting, braces, backslashes, line
continuation and comments work as they do in Tcl. Beyond that:

- **There are two commands**, `add wave` and `group`. Any other command is an error: there is no
  `set`, `foreach`, `proc` or `source`, and loading a list someone sent you only reads it.
- **Nothing is substituted.** `$` and `[…]` are ordinary characters, so `/tb/gen[0].q` is written
  as it is printed and needs no braces.
- **`#` starts a comment only where a command could start.** A comment after a command on the same
  line is written `;# …`.

```tcl
# The control signals, then the counters.
add wave /tb/clk /tb/rst_n
add wave -radix dec -label lin0 /tb/cntr_lin_gen[0].count
group "Recursive counters" {
    add wave -r /tb/cntr_rec_i
    add wave -radix bin -contents {
        add wave /tb/bus[7]
        add wave -highlight 2 /tb/bus[3]
    } /tb/bus
}
```

## The commands

```tcl
add wave ?options? path ?path …?
group ?options? name body
```

What `add wave` adds depends on what each path names:

- **A signal or a parameter**, by the path the Objects window shows for it, adds one row.
- **A signal followed by selects** adds a row showing what they select, indexed as the design
  declares the signal. For `logic [63:0][17:0] count`, `/tb/count[3]` is element 3,
  `/tb/count[3][7:4]` is four bits of it, and `/tb/count[7:4]` is four elements, still an array.
  For `logic [0:7] v`, `/tb/v[0]` is the most significant bit. A VHDL array is indexed as it is
  declared too. A part-select must be the last select, and it runs in its dimension's direction.
  Something with no dimensions of its own, an `int` or a record, is indexed by its bits from 0. A
  select outside the declared range is a warning that names the declared ranges.
- **A scope** adds what *Add to Wave* on that scope in the Structure window adds: a group named by
  the scope's path, holding its signals. With `-r` it adds what *Add to Wave Recursive* adds:
  - For a scope with instances below it, a group holding its own signals in a group called `$top`,
    then a group for each instance below, named by the instance.
  - For a scope without instances, the group of its signals.
  - Groups that would be empty are left out.

A path is looked up as a whole first, then with one select fewer at a time. A signal whose name
itself ends in brackets is therefore never mistaken for a select.

`group` adds a group called `name`, holding the rows its body describes. The body is a signal list
of its own and may be empty.

| Option | Meaning |
|---|---|
| `-radix bin\|oct\|dec\|hex\|ascii` | the radix; for an enumeration, the number instead of the name, as choosing a radix from the menu does |
| `-symbolic` | an enumeration's names after all; applied after `-radix` wherever it is written |
| `-label <text>` | the row's name, for a single signal or a non-recursive scope |
| `-highlight <1–5>` | the row's background colour |
| `-expand` | unpacked into its elements or bits, as unpacking in the window leaves it |
| `-contents <body>` | unpacked, with exactly the rows `body` describes beneath it |
| `-r` | for a scope, the whole subtree |

An option applies to every row its command adds. `group` takes `-radix`, `-symbolic` and
`-highlight`.

`-contents` describes an unpacked signal as the window can show one: its bits deleted or
reordered, other rows dragged in, and each row with its own radix, label and highlight.

## What saving writes

Loading a saved file rebuilds the same list. A saved file contains:

- `-label` only on rows that were renamed;
- `-expand` on a signal left exactly as unpacking made it, and `-contents` on one that was changed
  since;
- signals, selects and parameters by path, never a scope or `-r`, because by then the rows exist
  one by one and may have been rearranged. A range of bits that no select can name, such as part of
  one element and part of the next, is left out.
- words quoted only where Tcl needs them;
- nothing that varies between saves, such as a date or a user name, so an unchanged window saves to
  the same bytes and a list kept in version control changes only when the list does.

Cursors, zoom and which groups are collapsed are not part of a signal list.

## Loading

A file is loaded the way a paste is:

- nothing already in the window expands or collapses;
- rows arrive in file order;
- the whole file is read, and every path looked up, before any row is added.

**A mistake in the file is an error; a path the design lacks is not.** An unknown command or option,
a bad value or an unbalanced brace is reported with its file and line, and nothing is loaded. A path
that names nothing in the design is a warning with its file and line, and only that row is skipped.
A list outlives the version of the design it was saved from, so one renamed signal does not stop
the rest from loading.

## ModelSim's `wave.do`

The commands are shaped like ModelSim's so that they read familiarly, but `wave.do` files are not
loaded. A `wave.do` is a Tcl program, full of that window's own commands, and its paths separate a
generate block with `/` where Enzan's use `.`.
