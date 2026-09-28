# `src/control` — the control layer

Tcl is Enzan's control and configuration language (DESIGN_GUIDE.md section 8.1).
There is no second configuration format anywhere in the tool: the command
vocabulary, the file lists, the GUI theme and the window layout are all Tcl.

The directory builds **two** components, and the split is a design constraint
rather than a build detail.

## `enzan::control`

The interpreter and the things that only need text: the embedding of Tcl itself,
command registration, and the file-list and compile-order machinery that decides
*what* to analyse and in *what order*.

Allowed dependencies: `enzan::common`, and Tcl.

It deliberately does not depend on the simulation kernel. The GUI links it for
the theme script (section 10.5), and a front end that linked the kernel to read
a list of colours would quietly undo the process separation of section 2.2.

## `enzan::control_session`

The Questa-style command vocabulary and the session state it drives: analysis,
elaboration, the kernel, the waveform database.

Allowed dependencies: `enzan::control`, `enzan::common`, `enzan::eir`,
`enzan::elab`, `enzan::codegen`, `enzan::runtime`, `enzan::wavedb`, and the
frontends.

Only the `enzan` executable links it.

## What belongs here and what does not

A command's job is to parse its arguments, call into the component that does the
work, and turn the result into a Tcl result or a diagnostic. Simulation
behaviour lives in `runtime`, analysis in `frontend`, layout in `elab`. A
command that implements semantics of its own is a defect: the same operation has
to be reachable from the GUI and from VPI, neither of which goes through Tcl.

One command cannot follow that rule on its own. `listen` hands the session to a
graphical front end, and the server that does it lives in `ipc`, which depends on
`control` for the session it serves — so the dependency cannot run back the other
way. `Session` therefore holds a `SessionServer`, an interface `tools/enzan`
implements and installs before anything is evaluated. A session with none
answers the command with that reason, which is what a test process and a build
without the frontend both look like.

The refusals belong to `Session::serveTo` rather than to the command, because
`--listen` is the same request spelled as an option and the two must refuse a
design identically. One of them resetting a simulation that the other declined to
touch is exactly the divergence that having two control surfaces causes.
