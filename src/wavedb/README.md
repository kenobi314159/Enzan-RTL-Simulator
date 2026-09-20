# `src/wavedb` — the waveform database

Records value changes, and exports them in the formats external viewers understand
(DESIGN_GUIDE.md section 7).

## Responsibility

- **VCD export** (`VcdWriter.h`) — the universal interchange format. Human-readable,
  understood by every viewer, and required both for interoperability and for the
  differential testing of section 14.3.
- FST export and the in-memory database follow; see below.

## Allowed dependencies

`common`, `eir` and `runtime`.

The dependency runs this way and not the other: the kernel publishes value changes through
`runtime::ValueChangeListener` and knows nothing about waveforms, because it must be usable
with no recording at all and because the GUI attaches and detaches its own listeners.

## What M1 delivers, and what it does not

The VCD writer **streams straight to a file**. It does not fill an in-memory database
first.

That is a deliberate ordering rather than a shortcut. The database of section 7.1 — random
access by time, a configurable memory budget, spill to disk — exists to serve a GUI that
redraws an arbitrary zoom window without replaying the run, and the GUI is M2. Building the
database first and the exporter on top of it would make every batch run pay the database's
memory cost, which is the opposite of what a batch run wants. The two will coexist: the
database becomes a second listener alongside the writer.

**FST export is not yet implemented.** Section 7.3 lists it alongside VCD for M1 and it
remains M1 work. It needs GTKWave's `fstapi` vendored into `third_party/` with its license
recorded in `docs/third_party.md`, which is a separate piece of work from the writer itself.
Until then, VCD plus `gzip` covers the interchange need, and GTKWave converts with
`vcd2fst`.

## Design notes

**Values are read from the state arena at capture time** (section 7.2), not copied when a
change is reported. That is what makes coalescing free: several changes to one signal within
a time slot collapse into the value the slot settles on, which is what every other
simulator records and what stops a diff of two VCD files from drowning in delta-level
noise. Section 14.3 warns that comparisons must be made at settled time boundaries, and
this is where that starts.

**Selective capture is a requirement, not a convenience** (section 7.1). On a 100k-LUT
design, recording everything exhausts memory within seconds of simulated time. The scope
filter is a path prefix for now; the full `log -r /*` vocabulary arrives with the control
layer at M3.

**Every name a signal has is declared.** A collapsed port gives one slot two hierarchical
names and both are real, so both are emitted as `$var` declarations sharing one identifier.
A viewer that showed a DUT's port only under its parent's name would be confusing, and VCD
supports this exactly for the case.
