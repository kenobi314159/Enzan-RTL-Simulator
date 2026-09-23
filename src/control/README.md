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
