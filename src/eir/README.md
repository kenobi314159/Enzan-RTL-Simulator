# `src/eir` — the Enzan Internal Representation

EIR is Enzan's own language-neutral representation, and the contract between the
frontends and everything downstream (DESIGN_GUIDE.md section 4). A frontend's only
job is to turn source text into EIR; elaboration, code generation, the runtime, the
waveform database and the PLI all consume it.

## Responsibility

- **Value representation** (`BitVector.h`) — packed four-state bit vectors in two
  parallel bit planes, the views used to address them inside the state arena, and
  the owning vector used off the hot path. The encoding is deliberately the same as
  VPI's `s_vpi_vecval` (section 3.6).
- **Four-state operations** (`BitOps.h`) — the arithmetic, logical, bitwise,
  comparison, shift and selection semantics of IEEE 1800, including
  X-propagation. This is the highest-risk code in the project.
- **The type system** (`Type.h`) — what an EIR value is, and how much arena storage
  it needs.
- **The EIR-B instruction set** (`Instruction.h`, `Opcodes.def`) — three-address
  operations over typed values, each carrying the sketch of its lowering to LLVM IR
  that section 6.1 requires.
- **The structural layer, EIR-S** (`Process.h`, `Module.h`, `Design.h`) — design
  units, their objects, ports, instances and processes, and the container that
  analysis fills and a library stores.

## Allowed dependencies

`common` only, plus the C++ standard library.

EIR MUST NOT depend on `frontend`, `elab`, `codegen`, `runtime`, `wavedb` or
`pli` — the dependency runs the other way. In particular no slang or NVC type may
appear in a header here: keeping frontend-specific code confined to its own
directory is a standing invariant (section 17.4), and a slang type leaking into EIR
would make every downstream component depend on slang.

## Invariants

- **Four-state shape** (section 3.6). Never write a value path that assumes a
  single bit plane, even while only two-state is exercised. Two-state is an
  optimisation of the four-state representation, never a separate engine.
- **Canonical form.** Bits at or above a vector's width are zero in both planes.
  Every operation produces canonical output and may assume canonical input.
- **No allocation on the hot path** (section 13.4). Operations act on non-owning
  views into the state arena. `BitVector` allocates and therefore belongs to
  analysis, elaboration, the PLI boundary and tests — not to the VM's inner loop.
- **Widths are decided before the runtime sees them.** Binary operations require
  equal widths; the frontend inserts the extensions SystemVerilog's
  context-determined width rules call for.
