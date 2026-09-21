# `src/ipc` — the kernel/GUI protocol

The wire format and the transport that carry a simulation between the headless kernel and the
graphical front end (DESIGN_GUIDE.md section 2.2).

## Why there is a protocol at all

The kernel and the GUI are **separate processes**, and section 2.2 forbids collapsing them into
one for convenience. The reasons are concrete: the kernel must be testable and scriptable with
no display attached, a stalled or slow GUI must never stall the kernel, and attaching a local
GUI to a kernel on a remote compute server is something users of commercial tools do daily.
Everything in this directory exists to serve those three, and a change that makes any of them
harder is a change in the wrong direction.

## Responsibility

- **The wire format** (`Codec.h`) — framing, the little-endian primitives, and the bounds
  checking that turns a corrupt or mismatched stream into a protocol error rather than an
  allocation the machine cannot satisfy.
- **The messages** (`Protocol.h`) — every request and event, with one `serialize` function each.
- **The transport** (`Transport.h`) — the listening Unix domain socket, the connection, and the
  buffering in both directions.

## Allowed dependencies

`common` and `eir`.

Deliberately not `runtime`, `elab` or `wavedb`. Both ends of the connection link this
component, and the GUI has no business linking the simulation kernel: the point of the split is
that the two are separable. Values cross the socket as `eir::BitVector`, in the same two-plane
layout the state arena uses (section 3.6), so a value is copied rather than converted.

## Design notes

**One `serialize` per message, shared by both directions.** The writer and the reader present
the same member functions, so encoding and decoding a message are the same function read twice.
An encoder and a decoder written separately drift, and a field added to one and forgotten in the
other is a defect that only shows up when the two ends are different builds — which is precisely
the case a remote attach creates.

**Version mismatch is refused, never negotiated.** `ProtocolVersion` is compared in the
handshake and a difference ends the connection with a message saying so. A GUI and a kernel that
half-understand each other would display wrong values, and section 1.2 puts correctness above
convenience.

**Message numbers are permanent**, for the same reason diagnostic codes are (section 10.6): a
peer built against an older header decodes by number. Requests are numbered from 1 and events
from 128, so a message travelling in the wrong direction is visible in a trace instead of being
silently misread.

**Everything read off the socket is untrusted**, even from a friendly peer, because a half-written
frame or a version skew produces plausible-looking nonsense. Lengths are capped, counts are
checked against the bytes actually remaining before anything is resized, and a decode failure is
sticky so that a caller never receives a half-filled message that looks usable. A payload with
trailing bytes is a failure too: it means the two ends disagree about the message.

**Value changes are batched per settled time slot**, never per change. Section 14.3 warns that
two correct simulators may legitimately differ at delta level; a GUI shown delta-level
intermediate values would draw transitions that no waveform file contains, and users would
report them as bugs.

**Nothing blocks.** A blocking write is precisely how a slow GUI stalls a kernel: a client that
stops reading fills the socket buffer and the kernel sits in `write` until it comes back. Output
is queued in the connection and drained whenever the socket will take it, so a client that stops
reading costs the kernel memory and nothing else. Writes use `MSG_NOSIGNAL` for the same reason
— a peer that vanishes must make the write fail, not raise `SIGPIPE` and take the simulation
with it.

**A stale socket path is reused; a live one is not.** A path left behind by a kernel that was
killed has to be reusable or the user is stranded, and a path a running kernel is serving must
not be unlinked or the next kernel silently steals the session. The two look identical on disk,
so the listener tells them apart by trying to connect.

**Subscription is per signal.** A design has far more signals than a wave window shows, and
streaming the ones nobody is looking at would spend the socket on data the client discards. A
signal added to the wave window mid-run asks for its history separately, which is what makes its
past appear rather than only its future.
