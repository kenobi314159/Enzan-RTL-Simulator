# `src/jit` — native code preparation

This optional component owns LLVM ORC and its generated code. `Engine` verifies modules,
installs them for the host target, and resolves the resulting entry points. It explicitly
registers the runtime helper addresses; it does not make the executable's other symbols
available to generated code. Preparation errors go through Enzan's diagnostic engine.

An engine and its generated entry points belong to one prepared simulation. They must be
destroyed after the last native activation and before the diagnostic engine they report to.
LLVM's C++ types stay inside this component and its tests. The simulation runtime has no
dependency on LLVM or on this component.

This is the ORC foundation, not yet a bytecode compiler or a selectable execution backend.
It has been built and tested with LLVM 18; the remaining LLVM versions admitted by the build
need their own coverage.

## Allowed dependencies

`common`, `runtime`, and LLVM. The runtime must not depend on this component.
