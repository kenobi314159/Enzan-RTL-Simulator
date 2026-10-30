# `src/common`

Facilities that every other component is allowed to depend on, and that depend
on nothing inside Enzan themselves.

## Responsibility

- **Version information** (`Version.h`) — the version and source revision the
  binary was built from, as reported by `enzan --version`.
- **Installation layout** (`Installation.h`) — where the front end and VHDL's
  standard libraries are, found from the directory of the running executable.
- **Source management** (`SourceManager.h`) — loading source text into buffers
  and mapping byte offsets back to files, lines and columns.
- **Diagnostics** (`Diagnostics.h`) — the single diagnostic engine that serves
  the whole tool. Frontend diagnostics from slang and NVC are translated into it
  so that output is uniform regardless of source language.
- Small shared utilities that genuinely have no better home.

## Allowed dependencies

**None.** `common` is the bottom of the dependency graph: it may use the C++
standard library and nothing else from this repository. Every other component
may depend on it.

A change that makes `common` depend on `eir`, `runtime` or any other component
is a layering violation. If something in `common` needs to know about a
simulation concept, it belongs in the component that owns that concept, with
`common` providing only the generic mechanism.

"Small shared utilities" is not a licence to make this a dumping ground. Code
belongs here only when at least two components genuinely need it and it is
independent of simulation semantics.
