# `src/wavedb` — the waveform database

Records value changes, and exports them in the formats external viewers understand
(DESIGN_GUIDE.md section 7).

## Responsibility

- **The shared recording machinery** (`WaveformWriter.h`) — which signals to record, walking the
  hierarchy to declare them, coalescing the changes within a time slot, and attributing those
  changes to the right timestamp. All of that is the same problem for both formats, and it is where
  the subtle mistakes live, so there is one copy of the answer.
- **VCD export** (`VcdWriter.h`) — the universal interchange format. Human-readable, understood by
  every viewer, and required both for interoperability and for the differential testing of section
  14.3.
- **FST export** (`FstWriter.h`) — compressed and seekable, and what anyone recording a real design
  will actually use. Produced by GTKWave's own library, vendored in `third_party/fst`.
- The in-memory database follows; see below.

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

**Both formats, from one run.** The kernel delivers to every registered listener, so
`--vcd out.vcd --fst out.fst` records both from a single simulation — which is also how the GUI will
attach alongside a file writer at M2.

**FST is validated by reading it back with GTKWave's own converter.** `golden.counter.fst` and
`golden.fifo.fst` put Enzan's FST through `fst2vcd` and compare the result against the VCD written
from the same run. That checks three things at once: the file is readable by the reader GTKWave
actually uses; the hierarchy survives including the aliases a collapsed port needs; and the two
writers agree about every value at every timestamp. The comparison is order-insensitive within a
timestamp, because VCD does not define an order there and the two paths legitimately differ.

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
