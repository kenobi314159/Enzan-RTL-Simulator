# `src/gui` — the Qt 6 front end

The interactive debugging environment: the design hierarchy, the objects in a scope, the
waveform view and the transcript (DESIGN_GUIDE.md section 10).

## Responsibility

- **The theme** (`Theme.h`) — the colours everything here paints with, loaded from a Tcl script.
- **Signal lists** (`SignalList.h`) — reading, resolving and writing the wave window's list as a
  file (DESIGN_GUIDE.md section 10.8).
- **Tcl's syntax** (`TclSyntax.h`) — splitting a file into commands with Tcl's parser, evaluating
  nothing, and quoting a word so that it reads back. Signal lists are read with it.
- **The session** (`KernelSession.h`) — the GUI's end of the connection, on Qt's event loop, and
  the client-side copy of the waveform it is drawing.
- **The models** (`DesignTreeModel.h`, `ObjectsModel.h`, `WaveformModel.h`) — the hierarchy, the
  objects of a scope, and what the wave window shows.
- **The time axis** (`TimeAxis.h`) — the mapping between simulated time and pixels, separated out
  because it is the part of the view that can be wrong without anyone noticing by eye.
- **The waveform canvas** (`WaveformView.h`) — the custom painting, the cursors and the zoom.
- **The transcript** (`TranscriptView.h`) and **the window** (`MainWindow.h`).

## Allowed dependencies

`common`, `eir`, `ipc`, `wavedb`, Qt 6 and Tcl.

**Not `runtime`, `elab`, `codegen` or the frontends.** The GUI is a separate process from the
kernel (section 2.2) and must stay separable from it: everything it knows about a simulation
arrives over the socket. It uses `wavedb` for the client-side copy of the traces it is drawing —
the same database class the kernel fills, fed from the protocol instead of from an arena.

## Design notes

**Every colour comes from the theme file, and there is no fallback in C++.** Section 10.5
forbids colour literals in widget code, and a default written in C++ would be exactly that. The
shipped `themes/dark.tcl` is compiled in as a resource, loaded first, and a user theme is
layered over it, so a file that sets three colours changes three colours. A key added to
`ThemeColors.def` without being added to the theme file fails a test rather than producing a
black rectangle on someone's screen.

**A theme can change while the window is open**, so it reaches the window one way only:
`MainWindow::applyTheme`, which opening the window calls too. Whatever keeps a colour past the
moment it was painted has to be told — the canvas's drawn traces (`WaveformView::themeChanged`),
the transcript's lines, which remember which of the theme's colours they are in rather than the
colour, and the highlight swatches. Sizes are worked out from the desktop's fonts each time, never
from the window's, so that a theme setting none gives the desktop's back.

**The theme is Tcl** because section 10.5 says so, rather than inventing a settings format for
one purpose. The script gets one command and nothing else: a theme is a
list of colours, and an interpreter that also offered file access would make "copy this theme
from the internet" a more exciting decision than it should be.

**A signal list is not even that.** It is split into commands by `Tcl_ParseCommand`, through
`splitTclCommands`, and never evaluated, so there is no interpreter for a list to reach. That is also what lets the window
write one back out: a program cannot be saved from what it produced, and a list can.

**Every row is made one way.** `WaveformModel::rowFor`, `sliceOf`, `elementOf` and `bitOf` are
how a row is made from a signal, a range of one, an element of an array or a bit, and
`unpackedRows` is what unpacking makes of a row. Adding from the Objects window, pasting and
loading a list all go through them and through `insertSignalRows`. The Structure window's
*Add to Wave* entries go further: they run the signal list command `add wave ?-r? <scope>`, so
the menu and the file cannot come to disagree about what a scope adds.

**Rendering is bounded by pixels, not by transitions** (section 10.3). Each trace is drawn by
asking the waveform database for one summary per pixel column, which it answers from its resident
time index by binary search, so a column spanning a million transitions costs what an empty one
costs. Values are read only for the columns quiet enough to print one, which bounds those reads
by the pixel count too. This is why the database was built the way it was before a single pixel
was drawn: it is very hard to retrofit, and getting it wrong makes the viewer unusable on exactly
the large designs Enzan is for.

**Row geometry comes from the tree beside the canvas.** The names and the traces are two widgets
in a splitter, and the canvas reads its row positions from the tree's `visualRect`. Scrolling,
collapsing a group and a change of font then keep the two aligned without the canvas knowing
about any of them.

**Qt's unprefixed keywords are off** (`QT_NO_KEYWORDS`). `signals`, `slots` and `emit` are macros,
and each of them collides with a name the rest of Enzan uses correctly — `Image::signals()`,
`ElaboratedDesign::slots()`, `DiagnosticEngine::emit`. The GUI writes `Q_SIGNALS`, `Q_SLOTS` and
`Q_EMIT` instead, which costs nothing and stops the simulator having to be renamed around its
front end.

**Nothing here blocks.** The session sits on Qt's event loop through a socket notifier: a busy
kernel never freezes the interface, which is the mirror image of section 2.2's requirement that a
slow interface never stall the kernel. Both come from the same arrangement — neither side waits
for the other.

## What this milestone does not include

The **Source** and **Processes** windows of section 10.2 are absent.
