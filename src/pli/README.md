# `src/pli` — the Verilog Procedural Interface

What a VPI application loaded with `--vpi` sees of a simulation, and how it drives one
(DESIGN_GUIDE.md section 9). cocotb is the application this exists for.

## Responsibility

- **Loading** (`VpiContext::load`) — opening a library and running its startup routines.
- **The object model** — modules, generate blocks, nets, variables and parameters of the
  elaborated design, named and typed as section 9.2 says.
- **Values** (`VpiValue.h`) — converting between Enzan's two-plane vectors and the formats of
  `s_vpi_value`. The planes are the standard's own encoding (section 3.6), so most of this is
  copying.
- **Callbacks** — the registry, and the `runtime::PliHooks` that put each callback at its point
  in the region ordering (section 9.3).
- **The routines** (`VpiRoutines.cpp`) — every routine of `include/enzan/vpi_user.h`, with C
  linkage, forwarding to the context.

## Invariants this component is responsible for

**Writes are the kernel's writes.** `vpi_put_value` deposits, forces, releases and schedules
non-blocking updates through the same kernel operations the front end uses (sections 9.4 and
10.4). A second implementation of writing a signal from outside would be a second set of
semantics.

**A bad handle is an error, not a crash.** Every handle is looked up before it is used, and one
the context did not issue or has freed fails the call and is reported through `vpi_chk_error`.

**Callbacks fire in registration order** (section 3.8), and a callback registered for the phase
that is running waits for its next occurrence (section 9.3).

**Nothing a callback may still hold is freed under it.** A callback that fires for the last
time or is removed is retired and freed by a sweep once no dispatch is running, because a
callback may remove another, or itself, while a list it is in is being walked.

## Allowed dependencies

`common`, `eir`, `elab` and `runtime`, and the C library's dynamic loader.

**The kernel does not depend on this.** It calls `runtime::PliHooks`, which the context
implements, so that a kernel with nothing loaded pays one null test per hook.

## Deliberate deferrals

- **Single bits only.** `vpi_handle_by_index` selects one bit of a vector; there is no handle
  to a part-select, and unpacked arrays do not exist yet (M5).
- **User system tasks.** `vpi_register_systf` fails: a call to one has to be compiled by the
  frontend, and it cannot be.
- **The three delay modes behave alike** (section 9.4).
