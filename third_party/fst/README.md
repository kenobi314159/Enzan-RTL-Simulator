# `third_party/fst` — GTKWave's FST library

Vendored, unmodified, from [GTKWave](https://github.com/gtkwave/gtkwave) at tag **v3.3.116**,
from `gtkwave3-gtk3/src/helpers/fst/`.

| File | Origin | Licence |
|---|---|---|
| `fstapi.c`, `fstapi.h` | GTKWave, Tony Bybell | MIT |
| `fastlz.c`, `fastlz.h` | FastLZ, Ariya Hidayat | MIT |
| `lz4.c`, `lz4.h` | LZ4, Yann Collet | BSD-2-Clause |
| `fst_win_unistd.h` | GTKWave | MIT |
| `block_format.txt` | GTKWave — the on-disk format, kept because it is the only documentation of it | MIT |
| `fst_config.h` | **Enzan's own**, stands in for GTKWave's autoconf `config.h` | GPL-3.0-or-later |

All are compatible with Enzan's GPLv3 (DESIGN_GUIDE.md section 1.4), and each is recorded in
`docs/third_party.md` with its upstream URL and pinned revision.

## Why this is vendored at all

Section 11.2 reserves vendoring for cases where it is unavoidable, and this is one. FST is not
released as a library: no distribution packages a `libfst` with headers, and GTKWave ships it as
source inside its own tree. Verilator vendors the same files for the same reason. The alternative
— writing an FST writer from scratch — means reimplementing a compressed block container with a
hierarchy blob and variable-length value encoding, which is a large amount of work to arrive at a
format a reader might still reject.

## Rules for this directory

- **Nothing here is modified.** `fst_config.h` is added rather than patching `fstapi.c`, which is
  what `FST_CONFIG_INCLUDE` exists for. Keeping the vendored files byte-identical to upstream is
  what makes the next update a straight replacement instead of a merge.
- **It is compiled with its own warning flags**, not Enzan's. Third-party C code held to Enzan's
  warning set with `-Werror` would fail the build over style Enzan does not own, and patching it
  to satisfy us would break the rule above.
- **Its headers are system includes** to callers, for the same reason.
- Judy arrays and the parallel writer are both left off; `fst_config.h` says why.

## Updating

1. Replace the vendored files from the new tag, leaving `fst_config.h` and this README alone.
2. Check whether `fstapi.c` consults any new `HAVE_*` macro: `grep -oE 'HAVE_[A-Z0-9_]+' fstapi.c`.
3. Update the tag here and in `docs/third_party.md`.
4. Run the golden suite. `golden.*.fst` converts Enzan's output back with GTKWave's own `fst2vcd`
   and validates it, so a format regression fails there rather than in a user's viewer.
