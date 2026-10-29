# `src/codegen` — EIR-B to bytecode

Lowers the behavioural layer of EIR to the register-based bytecode the virtual machine
executes.

## Responsibility

- Assign every virtual register a Word offset in a per-process frame.
- Flatten the constant pool into one array in the same two-plane form the state arena
  uses, so reading a constant is a view rather than a copy.
- Lay blocks out end to end and resolve branch targets to instruction indices.
- Write every width the operation needs into the instruction, so that the interpreter's
  inner loop needs nothing but the instruction and the data.
- Resolve system task arguments to frame offsets and constant indices.
- Compile every subprogram's body as a process's is. A call keeps the caller's half — where its
  arguments are and where the result goes — and the callee's body keeps where its parameters and
  result live; which binding of the callee a call reaches is elaboration's to say, so nothing here
  resolves one to the other.

## What this component deliberately does not do

**No optimisation at all** — no constant folding, no dead code elimination, no register
coalescing. Optimisation belongs in elaboration or in the JIT. While the interpreter is
still the reference the JIT will be differentially tested against, an optimising
bytecode compiler would be a third place for a semantic bug to hide, and correctness
comes before performance.

**No knowledge of instances.** A compiled body refers to state by EIR object index, and
the runtime resolves those per instance. That is what keeps compiled code proportional
to the source rather than to the elaborated hierarchy, and it is why this component is
handed a `Design` and never an `ElaboratedDesign`.

**No validation it can avoid.** The EIR verifier is expected to have run; where this
component checks anything it reports that the verifier was skipped, because a compiler
that quietly tolerates bad input is how a frontend bug becomes a runtime crash.

## Allowed dependencies

`common` and `eir`.

Not `elab` — see above — and not `runtime`, which consumes the bytecode. The bytecode
format is the interface between the two, and it lives here.
