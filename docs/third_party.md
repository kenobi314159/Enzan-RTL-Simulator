# Third-party dependencies

DESIGN_GUIDE.md section 1.4 requires that code vendored into `third_party/`
retain its original license text and be recorded here with its license, upstream
URL and pinned revision. This file also records dependencies Enzan *acquires*
rather than vendors, because the license obligations are the same either way and
a single list is easier to audit than two.

Enzan is **GPLv3**, a consequence of linking NVC for the VHDL frontend. Every
dependency below is compatible with that.

## Vendored (`third_party/`)

| Dependency | Version / revision | License | Purpose | Notes |
|---|---|---|---|---|
| [FST](https://github.com/gtkwave/gtkwave) (`fstapi`) | tag `v3.3.116`, from `gtkwave3-gtk3/src/helpers/fst/` | MIT | FST waveform output | see `third_party/fst/README.md` |
| [FastLZ](https://github.com/ariya/FastLZ) | as shipped with the above | MIT | FST block compression | vendored by GTKWave, kept with it |
| [LZ4](https://github.com/lz4/lz4) | as shipped with the above | BSD-2-Clause | FST block compression | vendored by GTKWave, kept with it |
| [NVC](https://github.com/nickg/nvc) | tag `r1.23.0` (`cba10a2`), a git submodule | GPL-3.0-or-later | VHDL frontend (M6) | see `docs/nvc_integration.md` |

Vendoring is reserved for cases where it is unavoidable. FST qualifies because it is not released
as a library: no distribution packages a `libfst` with headers, and GTKWave ships it as source
inside its own tree. Verilator vendors the same files for the same reason.

`third_party/fst/fst_config.h` is **Enzan's own** file, standing in for GTKWave's autoconf
`config.h`, and carries Enzan's GPLv3 licence. Everything else in that directory is byte-identical
to upstream, which is what makes the next update a replacement rather than a merge.

NVC is the other case, since it is consumed at a pinned revision of its internal APIs rather than
as a released library (DESIGN_GUIDE.md section 5.2). It is a git submodule rather than a copy, so
`third_party/nvc` is NVC's own tree at the pinned commit, licence included, and
`git submodule update --init third_party/nvc` fetches it; `docs/nvc_integration.md` records which
of its files Enzan builds and why.

## Acquired at configure time

The build prefers a system package and falls back to building from source. See
`cmake/EnzanDependencies.cmake`.

| Dependency | Version / revision | License | Purpose | Acquisition |
|---|---|---|---|---|
| [slang](https://github.com/MikePopoloski/slang) | `v11.0` | MIT | SystemVerilog frontend (M1) | source build; not packaged by any distribution |
| [GoogleTest](https://github.com/google/googletest) | 1.12 or later, `v1.17.0` when fetched | BSD-3-Clause | unit tests | system package, source fallback |
| [zlib](https://zlib.net/) | 1.2.11 or later | zlib | FST waveform compression | system package |
| [LZ4](https://github.com/lz4/lz4) | as vendored with FST | BSD-2-Clause | FST waveform compression | vendored, see above |
| [Tcl](https://www.tcl-lang.org/) | 8.6 or later | BSD-style | GUI theme language (M2) | system package |
| [Qt](https://www.qt.io/) | 6.4 or later | LGPLv3 | graphical front end (M2) | system package, dynamically linked |
| [LLVM](https://llvm.org/) | 17 or later | Apache-2.0 with LLVM exception | JIT backend (M8, optional) | system package, off by default |
| [flex](https://github.com/westes/flex) | 2.6 or later | BSD-style; the scanners it generates carry no restriction | generating NVC's lexer when the VHDL frontend is built (M6); nothing of it is linked | system package |
| [zstd](https://github.com/facebook/zstd) | 1.4 or later | BSD-3-Clause, or GPL-2.0 at the user's choice | the analysed units NVC writes into a library, which it compresses (M6) | system package |
| [cocotb](https://github.com/cocotb/cocotb) | `2.1.0`, pinned in `tests/conformance/cocotb/requirements.txt` | BSD-3-Clause | the cocotb golden test and regression suite (M4); loaded with `--vpi`, never linked | `pip`; its test suite is fetched at the matching tag and never vendored |

slang itself builds against fmt, Boost headers and mimalloc, which it acquires
on its own terms; all three are permissively licensed. Enzan has slang use
mimalloc only where the system has one, and never build it: the static copy
slang builds crashed every program linking it, in glibc's own start-up (see
`cmake/EnzanDependencies.cmake`).

## Pinned revisions

The slang revision is a CMake cache variable rather than a hard-coded string, so
that a contributor can test against a different revision without editing the
build:

```sh
cmake --preset gcc-debug -DENZAN_SLANG_GIT_TAG=v10.0 -DENZAN_SLANG_VERSION=10.0
```

Changing the default pinned revision is a deliberate act. Upgrading slang or NVC
is expected to break the adapters in `src/frontend/`, which is what the golden
tests exist to catch (DESIGN_GUIDE.md section 5.2).

## Bundled in a release's source tarball

A release's tarball carries, in `third_party/bundled/`, the sources FetchContent downloads to
build slang, and GoogleTest, because a distribution's builder has no network (DESIGN_GUIDE.md
section 11.6) and RHEL 9 packages GoogleTest 1.11. `scripts/make-dist.sh` copies whatever CMake
fetched rather than restating it, so this list is a record of the current revisions rather than a
second place they are set:

| Dependency | Version | License | Brought in by |
|---|---|---|---|
| slang | `v11.0` | MIT | Enzan, `ENZAN_SLANG_GIT_TAG` |
| [fmt](https://github.com/fmtlib/fmt) | `12.1.0` | MIT | slang, which builds its own (see `cmake/EnzanDependencies.cmake`) |
| [GoogleTest](https://github.com/google/googletest) | `v1.17.0` | BSD-3-Clause | Enzan's unit tests, where no GoogleTest 1.12 or later is installed |

None of them is in the repository, and each keeps its own licence text in the tarball.

## Adding a dependency

Per DESIGN_GUIDE.md section 11.2, adding one requires all of:

1. An entry in section 11.3 of the design guide.
2. A `find_package` path with an error message naming the package to install.
3. A `FetchContent` fallback, where one is possible.
4. Installation in CI.
5. A license check against section 1.4, and a row in this file.
