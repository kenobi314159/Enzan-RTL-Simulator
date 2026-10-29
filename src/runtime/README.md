# `src/runtime` — the simulation kernel

The virtual machine, the scheduler, the state arena and the value engine that runs an
elaborated, compiled design.

## Responsibility

- **The arena** (`Arena.h`) — the single contiguous allocation holding all mutable
  simulation state, addressed by Word offset and never by pointer.
- **The image** (`Image.h`) — the last assembly step, where the analysed design's types,
  elaboration's hierarchy and codegen's bodies meet. Nothing downstream needs any of the
  three.
- **The scheduler** (`Scheduler.h`) — IEEE 1800's stratified event queue, the time
  wheel, and the non-blocking update queue. Holds ordering, not policy.
- **The interpreter** (`Interpreter.h`) — executes compiled bodies.
- **The kernel** (`Kernel.h`) — drives the regions, applies updates, detects value changes,
  resolves drivers, enforces the delta limit and performs system tasks. It also keeps each
  VHDL driver's projected waveform, which is what a VHDL signal assignment acts on: the
  transactions an assignment deletes are taken out of the time wheel, so they leave no time
  step behind. And it keeps the heap of access values, where std.textio's lines live by
  handle, and runs textio's subprograms on them natively.
- **Formatting** (`Format.h`) — the `$display` conversions.

## Invariants this component is responsible for

**One kernel, one event queue.** VHDL and SystemVerilog are not simulated by separate
engines that synchronise. Nothing here is specific to a source language; the kernel sees
signals, slots and compiled bodies and has no idea which frontend produced them.

**Determinism.** Process execution order within a region is strictly FIFO. No container
whose iteration order is unspecified may influence which process runs first, which value is
written, or what appears in the transcript.

**Activity-based scheduling.** A process is woken when a signal it watches actually changes
value, never merely when it is assigned. The comparison happens before the write, in
`Kernel::writeSignal`.

**A delta loop must be diagnosable.** The limit exists, defaults to 10,000 and names the
processes still queued when it trips. A bare `always` loop with no time control counts each
iteration as a delta so that the same limit catches it, rather than spinning in the Active
region forever.

**No allocation on the hot path.** Register frames are one allocation for the whole design,
the non-blocking update pool keeps its capacity between time slots, and the region queues
reuse their storage.

**One call stack.** A call's registers go on a stack that follows the processes' frames in that
one allocation, since a subprogram never suspends and every call a process makes has returned
before it does. The image measures the deepest chain of calls the design can make and the stack
is laid out for exactly that; where a call can reach itself again, it is laid out for
`KernelOptions::callDepthLimit` calls, 10,000 by default, and a run that would go deeper stops
with ENZ5011 naming the subprogram.

**Single-threaded.** That is the plan and not a fallback: a cache-friendly single-threaded
loop outperforms a naively parallel one, and determinism comes free.

**Pausing must not change the answer.** The GUI must be able to stop a running kernel, and a
stalled GUI must never stall it. Both come from polling rather than signalling:
`RunInterrupt::shouldInterrupt` is asked between time slots, and the kernel returns
`RunStatus::Interrupted` with everything it holds still in the scheduler and the arena, so
calling `run` again produces exactly what running straight through would have. The boundary
is chosen for that reason and not for convenience — an interruption part-way through a
region would make a waveform depend on when the user clicked, which is the least debuggable
form determinism could be violated in.

## Allowed dependencies

`common`, `eir`, `elab` and `codegen`.

**Not `wavedb`.** The kernel must be usable with no waveform recording at all, and the
GUI attaches and detaches its own listeners, so value changes are published through the
`ValueChangeListener` interface and the waveform database implements it.

**Not `pli`**, for the same reason. The points in the region ordering where a VPI
application's callbacks run are the `PliHooks` interface, and `pli` implements it. A kernel
with nothing loaded pays one null test per hook.

## Deliberate deferrals

- **Dispatch is a `switch`, not computed goto.** Threaded dispatch is a SHOULD, and nothing
  is optimised without a measurement. It arrives with M8's profiling. Until then the
  interpreter is the reference the JIT will be differentially tested against, and readable
  semantics are worth more than a dispatch win nobody has measured.
- **The time wheel is an ordered map.** A hierarchical or calendar queue is for when there
  is a benchmark to justify one.
- **Preponed and the PLI-specific regions are not separate queues.** They may be callback
  hook points instead, which is what `PliHooks` is, and nothing in the subset samples in
  Preponed.
- **Resolution has no strength model.** Every driver is strong, which the frontend
  guarantees by refusing any other strength, so the tables of IEEE 1800 tables 6-2 to 6-4
  are the whole of it.
