# `src/gui` — the Qt 6 front end

The interactive debugging environment: the design hierarchy, the objects in a scope, the
waveform view and the transcript (DESIGN_GUIDE.md section 10).

## Responsibility

- **The theme** (`Theme.h`) — the colours everything here paints with, loaded from a Tcl script.
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

**The theme is Tcl** because section 8.1 commits to Tcl as *the* configuration language for the
whole tool, with no second format. The script gets one command and nothing else: a theme is a
list of colours, and an interpreter that also offered file access would make "copy this theme
from the internet" a more exciting decision than it should be.

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

The **Source** and **Processes** windows of section 10.2 are absent. Source browsing is only
useful with the breakpoints and stepping that make it a debugger rather than a file viewer, and
those arrive with M3; a window that showed source and could do nothing with it would be a worse
answer than the editor the user already has.

The Transcript's **command entry is present and disabled**, with the reason in its placeholder.
Section 8.1 commits to Tcl with a Questa-style vocabulary as *the* control language and section 18
puts it in M3. A handful of commands invented here to fill the gap would be a second control
surface that M3 would then have to stay compatible with.
