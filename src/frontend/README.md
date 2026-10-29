# `src/frontend` — language frontends

A frontend's only job is to turn source text into EIR.

Each language is exactly one directory here, exposing a single entry point that takes source
files plus options and returns EIR design units or diagnostics. Nothing else
in Enzan may reach into a frontend, and a frontend may depend on nothing but `common` and
`eir`.

| Directory | Language | Built on |
|---|---|---|
| `sv/` | SystemVerilog | [slang](https://sv-lang.com/) (MIT) |
| `vhdl/` | VHDL | [NVC](https://github.com/nickg/nvc) (GPLv3) |

The invariants that apply to every frontend:

- **No flattening.** Hierarchy is preserved; flattening is an elaboration-time optimisation.
- **No scheduling decisions and no runtime state.** Both belong to `runtime`.
- **Thin adapters.** Frontend-specific types stay inside the frontend's own directory. A
  slang or NVC type in an EIR header would make every downstream component depend on that
  frontend.
- **Verified output.** A frontend's EIR is checked by `eir::verify` before it is accepted,
  because a body's declared read and write sets are exactly what a translator gets quietly
  wrong.
