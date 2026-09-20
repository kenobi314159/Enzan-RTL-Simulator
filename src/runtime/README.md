# `src/runtime` — the simulation kernel

The virtual machine, the scheduler, the state arena and the value engine that runs an
elaborated, compiled design.

## Responsibility

- **The arena** (`Arena.h`) — the single contiguous allocation holding all mutable
  simulation state, addressed by Word offset and never by pointer (DESIGN_GUIDE.md
  section 4.3).
- **The image** (`Image.h`) — the last assembly step, where the analysed design's types,
  elaboration's hierarchy and codegen's bodies meet. Nothing downstream needs any of the
  three.
- **The scheduler** (`Scheduler.h`) — the stratified event queue of section 3.3, the time
  wheel, and the non-blocking update queue. Holds ordering, not policy.
- **The interpreter** (`Interpreter.h`) — executes compiled bodies.
- **The kernel** (`Kernel.h`) — drives the regions, applies updates, detects value
  changes, enforces the delta limit and performs system tasks.
- **Formatting** (`Format.h`) — the `$display` conversions.

## Invariants this component is responsible for

**One kernel, one event queue.** Section 3.5 requires this even once VHDL arrives: VHDL
and SystemVerilog are not simulated by separate engines that synchronise. Nothing here is
specific to a source language; the kernel sees signals, slots and compiled bodies and has
no idea which frontend produced them.

**Determinism** (section 3.8). Process execution order within a region is strictly FIFO.
No container whose iteration order is unspecified may influence which process runs first,
which value is written, or what appears in the transcript.

**Activity-based scheduling** (section 6.2). A process is woken when a signal it watches
actually changes value, never merely when it is assigned. The comparison happens before
the write, in `Kernel::writeSignal`.

**A delta loop must be diagnosable** (section 3.2). The limit exists, defaults to 10,000
and names the processes still queued when it trips. A bare `always` loop with no time
control counts each iteration as a delta so that the same limit catches it, rather than
spinning in the Active region forever.

**No allocation on the hot path** (section 13.4). Register frames are one allocation for
the whole design, the non-blocking update pool keeps its capacity between time slots, and
the region queues reuse their storage.

**Single-threaded.** That is the plan and not a fallback (section 6.4): a cache-friendly
single-threaded loop outperforms a naively parallel one, and determinism comes free.

## Allowed dependencies

`common`, `eir`, `elab` and `codegen`.

**Not `wavedb`.** The kernel must be usable with no waveform recording at all, and the
GUI attaches and detaches its own listeners, so value changes are published through the
`ValueChangeListener` interface and the waveform database implements it.

## Deliberate deferrals

- **Dispatch is a `switch`, not computed goto.** Section 6.1 makes threaded dispatch a
  SHOULD, and section 13.4 forbids optimising without a measurement. It arrives with M8's
  profiling. Until then the interpreter is the reference the JIT will be differentially
  tested against, and readable semantics are worth more than a dispatch win nobody has
  measured.
- **The time wheel is an ordered map.** Section 6.2 calls for a hierarchical or calendar
  queue once there is a benchmark to justify one.
- **Preponed and the PLI-specific regions are not separate queues.** Section 3.3 permits
  representing them as callback hook points, and nothing in the M1 subset samples in
  Preponed.
- **One driver per signal.** Driver resolution is M5; elaboration rejects a design that
  would need it rather than letting the last write win silently.
