# The graphical front end

Enzan's window shows a design's hierarchy, its signals and their waveforms while the simulation
runs, and lets you drive the run.

## Opening a design

```sh
enzan run --gui --top tb design.sv testbench.sv
```

elaborates the design and opens a window on it. Press **Run**, click a scope in **Structure**,
select signals in **Objects** and press Ctrl+W to put them in the wave window; the traces fill in
as the simulation advances. `--run 10us` alongside `--gui` asks the window to run that long once
it opens. Closing the window ends the simulation.

**F1** lists every key and what the mouse does.

## The wave window

- **The wheel** scrolls the list, **Ctrl+wheel** zooms about the pointer, and **Shift+wheel**
  pans. Dragging the **right button** right to left frames the range you swept; left to right
  zooms to fit the run.
- **A left click** selects the signal you clicked and puts the active cursor on the nearest edge.
  **Tab** and **Shift+Tab** then walk the cursor from edge to edge of the selected row, whichever
  way you selected it, a group included. To measure, lock one cursor and step another, and read
  both times off their labels. Right-click a cursor's label to rename, lock or remove it.
- **To find a value**, type it in the toolbar's *Find value* field as the selected row shows it —
  `ff` in a hexadecimal row, `S_IDLE` in an enum's, `{1}{ff}` in a group's — or as a literal such
  as `8'hff` or `'x`, and press **F3**, or Enter in the field. A digit the row shows in capitals,
  `X` or `Z`, finds any digit shown so: some of its bits unknown and not all. The active cursor
  moves to the next time the row takes that value, and pressing again goes on to the one after;
  **Shift+F3**, or Shift+Enter, goes back. A locked cursor stays put, and the status bar says what
  was found, or why nothing was. A long search runs while you carry on: move the cursor and it
  stops.
- **Rows** drag to reorder, rename in place with **F2**, and copy and paste with **Ctrl+C** and
  **Ctrl+V**. The context menu also offers a pack/unpack toggle, a radix and one of five highlight
  colours.
- **A bus expands into its bits**, and an unpacked vector is a group like any other: bits drag out
  of it and back in, and other rows can be dragged in too. A row shows what its children show,
  joined: delete a bit and its bus narrows to the rest, and a group of one-bit signals reads and
  draws as a vector. A group of wider things reads as the list of them, `{001122}{112233}`.
- **An `enum` signal shows the name of its value**, `S_SEND` rather than `3'h2`; picking a radix
  shows the number.
- **Parameters and localparams** are listed in **Objects** after a scope's signals, and drop into
  the wave window as constant rows, so `ACK_DELAY` can sit beside the traces it explains.

## Writing signals

Right-clicking in **Objects** offers **Deposit…**, **Force…** and **Release** alongside *Add to
Wave*. A deposit writes the value now, and the next thing that drives the signal overwrites it; a
force holds it against the design until it is released.

## Saving what the wave window shows

*File → Save Signal List* (Ctrl+S) writes the wave window's rows to a file, the same one each time
after the first, and *Load Signal List…* or `--wave-list signals.tcl` puts them back. Adding
signals in the window writes the matching command into the transcript, ready to copy.
[Signal lists](signal-lists.md) describes the format.

## A simulation on another machine

The simulation and the window are separate processes that talk over a Unix domain socket, so a
window on your machine can drive a simulation on another one:

```sh
# on the compute server
enzan --top tb --listen /tmp/enzan.sock design.sv testbench.sv

# locally, over a forwarded socket
ssh -L /tmp/enzan.sock:/tmp/enzan.sock server
enzan --gui --attach /tmp/enzan.sock
```

The flag you choose sets the lifetime. `enzan run --gui` starts the simulation, so closing the
window ends it too. `enzan --gui --attach` finds a simulation it did not start, so closing the
window only detaches, and the run can be looked in on again.

## Themes and settings

The window keeps its settings in `~/.config/enzan/`, or `$XDG_CONFIG_HOME/enzan/`, which it creates
the first time it opens; `enzan --help` says where it is. Nothing there changes what a simulation
computes or records.

- `config` names the theme: `theme themes/dark.tcl` for a file, or `theme -builtin light` for the
  built-in light theme. Without it the window uses the built-in dark theme.
- `themes/dark.tcl` is the dark theme written out as a starting point. A theme file is a Tcl
  script loaded over the dark theme, so a file that sets three colours changes three colours.
  `font wave 11` sets a font size.
- *View → Theme* chooses between the built-in themes and the files in `themes/`, *New Theme…*
  writes a copy of the theme in use, and *Reload Theme* reads it again after you edit it.
- `layout.ini` is where the docks were when the window last closed. Delete it to start over.
