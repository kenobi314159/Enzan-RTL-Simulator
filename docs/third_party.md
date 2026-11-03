# Third-party dependencies

Code vendored into `third_party/` keeps its original license text and is
recorded here with its license, upstream URL and pinned revision. This file also
records dependencies Enzan *acquires* rather than vendors, because the license
obligations are the same either way and a single list is easier to audit than
two.
Enzan is **GPLv3**, a consequence of linking NVC for the VHDL frontend. Every
dependency below is compatible with that.

## Vendored (`third_party/`)

| Dependency | Version / revision | License | Purpose | Notes |
|---|---|---|---|---|
| [FST](https://github.com/gtkwave/gtkwave) (`fstapi`) | tag `v3.3.116`, from `gtkwave3-gtk3/src/helpers/fst/` | MIT | FST waveform output | see `third_party/fst/README.md` |
| [FastLZ](https://github.com/ariya/FastLZ) | as shipped with the above | MIT | FST block compression | vendored by GTKWave, kept with it |
| [LZ4](https://github.com/lz4/lz4) | as shipped with the above | BSD-2-Clause | FST block compression | vendored by GTKWave, kept with it |
| [NVC](https://github.com/nickg/nvc) | tag `r1.23.0` (`cba10a2`), a git submodule | GPL-3.0-or-later | VHDL frontend | see `docs/development/nvc_integration.md` |

Vendoring is reserved for cases where it is unavoidable. FST qualifies because it is not released
as a library: no distribution packages a `libfst` with headers, and GTKWave ships it as source
inside its own tree. Verilator vendors the same files for the same reason.

`third_party/fst/fst_config.h` is **Enzan's own** file, standing in for GTKWave's autoconf
`config.h`, and carries Enzan's GPLv3 licence. Everything else in that directory is byte-identical
to upstream, which is what makes the next update a replacement rather than a merge.

NVC is the other case, since it is consumed at a pinned revision of its internal APIs rather than
as a released library. It is a git submodule rather than a copy, so
`third_party/nvc` is NVC's own tree at the pinned commit, licence included, and
`git submodule update --init third_party/nvc` fetches it; `docs/development/nvc_integration.md` records which
of its files Enzan builds and why.

## Acquired at configure time

The build prefers a system package and falls back to building from source. See
`cmake/EnzanDependencies.cmake`.

| Dependency | Version / revision | License | Purpose | Acquisition |
|---|---|---|---|---|
| [slang](https://github.com/MikePopoloski/slang) | `v11.0` | MIT | SystemVerilog frontend | source build; not packaged by any distribution |
| [GoogleTest](https://github.com/google/googletest) | 1.12 or later, `v1.17.0` when fetched | BSD-3-Clause | unit tests | system package, source fallback |
| [zlib](https://zlib.net/) | 1.2.11 or later | zlib | FST waveform compression | system package |
| [LZ4](https://github.com/lz4/lz4) | as vendored with FST | BSD-2-Clause | FST waveform compression | vendored, see above |
| [Tcl](https://www.tcl-lang.org/) | 8.6 or later | BSD-style | GUI theme language and signal list syntax | system package |
| [Qt](https://www.qt.io/) | 6.4 or later | LGPLv3 | graphical front end | system package, dynamically linked |
| [LLVM](https://llvm.org/) | 17 or later | Apache-2.0 with LLVM exception | JIT backend (M8, optional) | system package, off by default |
| [flex](https://github.com/westes/flex) | 2.6 or later | BSD-style; the scanners it generates carry no restriction | generating NVC's lexer when the VHDL frontend is built; nothing of it is linked | system package |
| [zstd](https://github.com/facebook/zstd) | 1.4 or later | BSD-3-Clause, or GPL-2.0 at the user's choice | the analysed units NVC writes into a library, which it compresses | system package |
| [cocotb](https://github.com/cocotb/cocotb) | `2.1.0`, pinned in `tests/conformance/cocotb/requirements.txt` | BSD-3-Clause | the cocotb golden test and regression suite; loaded with `--vpi`, never linked | `pip`; its test suite is fetched at the matching tag and never vendored |

slang itself builds against fmt, Boost headers and mimalloc, which it acquires
on its own terms; all three are permissively licensed. Enzan has slang use
mimalloc only where the system has one, and never build it: the static copy
slang builds crashed every program linking it, in glibc's own start-up (see
`cmake/EnzanDependencies.cmake`).

## Bundled in a release's source tarball

A release's tarball carries, in `third_party/bundled/`, the sources FetchContent downloads to build
slang, and GoogleTest, because a distribution's builder has no network and RHEL 9 packages
GoogleTest 1.11. `scripts/make-dist.sh` copies whatever CMake fetched rather than restating it, so
this list is a record of the current revisions rather than a second place they are set:

| Dependency | Version | License | Brought in by |
|---|---|---|---|
| slang | `v11.0` | MIT | Enzan, `ENZAN_SLANG_GIT_TAG` |
| [fmt](https://github.com/fmtlib/fmt) | `12.1.0` | MIT | slang, which builds its own (see `cmake/EnzanDependencies.cmake`) |
| [GoogleTest](https://github.com/google/googletest) | `v1.17.0` | BSD-3-Clause | Enzan's unit tests, where no GoogleTest 1.12 or later is installed |

None of them is in the repository, and each keeps its own licence text in the tarball.

## Translated from other projects

Two testbenches of the design regression are another project's testbench rewritten in the other
language, so that the design can be checked with the boundary between the languages moved. A
translation is that project's work as well as Enzan's, so each keeps the original's copyright
notice and licence, which cover the translation too, rather than Enzan's GPLv3:

| File | Translated from | License |
|---|---|---|
| `tests/designs/neorv32/tb/neorv32_tb.v` | [NEORV32](https://github.com/stnolting/neorv32)'s `sim/neorv32_tb.vhd` | BSD-3-Clause |
| `tests/designs/aes/tb/tb_aes_core.vhd` | [secworks/aes](https://github.com/secworks/aes)'s `src/tb/tb_aes_core.v` | BSD-2-Clause |

Each was translated from the revision its design's `revision.txt` names.
