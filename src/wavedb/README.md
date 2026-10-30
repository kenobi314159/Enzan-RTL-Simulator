# `src/wavedb` — the waveform database

Records value changes, and exports them in the formats external viewers understand.

## Responsibility

- **The shared recording machinery** (`WaveformWriter.h`) — which signals to record, walking the
  hierarchy to declare them, coalescing the changes within a time slot, and attributing those
  changes to the right timestamp. All of that is the same problem for VCD, for FST and for the live
  database, and it is where the subtle mistakes live, so there is one copy of the answer. It is also
  where a VHDL signal's codes become the bits they stand for, so that every format and the database
  show one value, and where a real is told apart, which VCD and FST each hold as the number in a
  variable of their own real type.
- **VCD export** (`VcdWriter.h`) — the universal interchange format. Human-readable, understood by
  every viewer, and required both for interoperability and for differential testing.
- **FST export** (`FstWriter.h`) — compressed and seekable, and what anyone recording a real design
  will actually use. Produced by GTKWave's own library, vendored in `third_party/fst`.
- **The live database** (`WaveformDatabase.h`) — the in-memory store the GUI draws from, with
  random access by time, per-column summaries for the waveform view, and a memory budget that
  spills to a scratch file rather than growing without bound.
- **The recorder** (`WaveformRecorder.h`) — the listener that fills the database from a running
  kernel. It is a `WaveformWriter` whose "format" is the database, so the two exporters and the
  database share one answer to which signals are in scope and which value a time slot settled on.

## Allowed dependencies

`common`, `eir` and `runtime`.

The dependency runs this way and not the other: the kernel publishes value changes through
`runtime::ValueChangeListener` and knows nothing about waveforms, because it must be usable
with no recording at all and because the GUI attaches and detaches its own listeners.

## The exporters and the database are peers

The VCD and FST writers **stream straight to a file** and never fill the database first, and
the database is never written through a file. Both are listeners on the kernel, registered
independently, and a run may have any combination of them.

That is a deliberate ordering rather than an accident of how the two arrived. Making the
exporters read from the database would force every batch run to pay the database's memory
cost, which is the opposite of what a batch run wants; making the database read from a file
would force the GUI to wait for a flush. What they do share is the part that has to agree —
`WaveformWriter`, which decides what is in scope and which value a slot settled on.

## How the database is laid out, and why

The structure is per-signal append-only chunks of value changes, plus a time index for random
access. Where the split falls is what the rest of the requirements turn on.

**The time index is resident; the values spill.** Per signal the database keeps the change
times and a prefix count of changes to an unknown value in memory always, and the value payload
in fixed-size chunks that are written to a scratch file and freed once the memory budget is
exceeded. The index costs twelve bytes per change against sixteen or more for the payload, so
the budget governs the part that actually grows.

The more important consequence is for rendering. The cost of drawing a window must be a function of
visible pixels rather than of the value changes in it, and every question the view asks per pixel
column — how many transitions fall inside it, was anything unknown, which change is in effect at its
left edge — is answered from the resident index by a binary search and a subtraction. A column
spanning a million transitions costs exactly what an empty one costs, and no value is read at all.
Values are fetched only for the columns quiet enough to print one, which bounds chunk reloads by the
pixel count too.

**A scratch file that cannot be created is not fatal.** The chunk stays in memory and leaves the
eviction order. Losing a waveform silently is worse than exceeding a budget.

**Both formats, from one run.** The kernel delivers to every registered listener, so
`--vcd out.vcd --fst out.fst` records both from a single simulation — and it is how the GUI's
recorder attaches alongside a file writer while a run is in progress.

**FST is validated by reading it back with GTKWave's own converter.** `golden.counter.fst` and
`golden.fifo.fst` put Enzan's FST through `fst2vcd` and compare the result against the VCD written
from the same run. That checks three things at once: the file is readable by the reader GTKWave
actually uses; the hierarchy survives including the aliases a collapsed port needs; and the two
writers agree about every value at every timestamp. The comparison is order-insensitive within a
timestamp, because VCD does not define an order there and the two paths legitimately differ.

## Design notes

**Values are read from the state arena at capture time**, not copied when a change is reported. That
is what makes coalescing free: several changes to one signal within a time slot collapse into the
value the slot settles on, which is what every other simulator records and what stops a diff of two
VCD files from drowning in delta-level noise. A differential comparison has to be made at settled
time boundaries, and this is where that starts.

**Selective capture is a requirement, not a convenience.** On a 100k-LUT design, recording
everything exhausts memory within seconds of simulated time. The scope filter is a list of path
prefixes, so a reader can select a subtree and a handful of single signals at once.

**Every name a signal has is declared.** A collapsed port gives one slot two hierarchical
names and both are real, so both are emitted as `$var` declarations sharing one identifier.
A viewer that showed a DUT's port only under its parent's name would be confusing, and VCD
supports this exactly for the case.
