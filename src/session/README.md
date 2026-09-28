# `src/session` — the simulation pipeline

One object holding the three phases of DESIGN_GUIDE.md section 2.1: analysis,
elaboration, and the kernel with its waveform files.

Allowed dependencies: `enzan::common`, `enzan::eir`, `enzan::elab`,
`enzan::codegen`, `enzan::runtime`, `enzan::wavedb`, and the frontends.

It owns a kernel, so the GUI must not link it: a front end that linked the
simulator would quietly undo the process separation of section 2.2. Only the
`enzan` executable and `ipc`'s server end link it.

## What belongs here and what does not

Holding the phases together, and nothing else. Simulation behaviour lives in
`runtime`, analysis in `frontend`, layout in `elab`. A method here that
implemented semantics of its own would be a defect: the same operation has to be
reachable from the GUI and from VPI, and neither of those comes through here.

**There is one session per kernel process.** A server that built a simulation of
its own would make two, and a front end would then be showing a different one
from whatever else was driving it — which is the failure mode that makes a
debugger untrustworthy rather than merely wrong.
