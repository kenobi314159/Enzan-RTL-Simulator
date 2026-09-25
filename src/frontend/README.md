# `src/frontend` — language frontends

A frontend's only job is to turn source text into EIR (DESIGN_GUIDE.md section 5).

Each language is exactly one directory here, exposing a single entry point that takes source
files plus options and returns EIR design units or diagnostics (section 5.3). Nothing else
in Enzan may reach into a frontend, and a frontend may depend on nothing but `common` and
`eir`.

| Directory | Language | Built on | Milestone |
|---|---|---|---|
| `sv/` | SystemVerilog | [slang](https://sv-lang.com/) (MIT) | M1 |
| `vhdl/` | VHDL | [NVC](https://github.com/nickg/nvc) (GPLv3) | M6 |

The invariants that apply to every frontend:

- **No flattening.** Hierarchy is preserved; flattening is an elaboration-time optimisation
  (section 17.4).
- **No scheduling decisions and no runtime state.** Both belong to `runtime`.
- **Thin adapters.** Frontend-specific types stay inside the frontend's own directory. A
  slang or NVC type in an EIR header would make every downstream component depend on that
  frontend.
- **Verified output.** A frontend's EIR is checked by `eir::verify` before it is accepted,
  because the core invariant of section 4.2 is exactly what a translator gets quietly wrong.
