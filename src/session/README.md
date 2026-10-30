# `src/session` — the simulation pipeline

One object holding the three phases: analysis, elaboration, and the kernel with its waveform files.

Allowed dependencies: `enzan::common`, `enzan::eir`, `enzan::elab`,
`enzan::codegen`, `enzan::runtime`, `enzan::wavedb`, `enzan::pli`, and the frontends.

A loaded VPI application is part of the simulation rather than something that watches it, so
the session holds it beside the kernel: loaded before analysis, shown each elaboration, and told
when the simulation ends however that happens.

It owns a kernel, so the GUI must not link it: a front end that linked the simulator would quietly
undo the separation of the two processes. Only the `enzan` executable and `ipc`'s server end link
it.

## What belongs here and what does not

Holding the phases together, and nothing else. Simulation behaviour lives in
`runtime`, analysis in `frontend`, layout in `elab`. A method here that
implemented semantics of its own would be a defect: the same operation has to be
reachable from the GUI and from VPI, and neither of those comes through here.

**There is one session per kernel process.** A server that built a simulation of
its own would make two, and a front end would then be showing a different one
from whatever else was driving it — which is the failure mode that makes a
debugger untrustworthy rather than merely wrong.
