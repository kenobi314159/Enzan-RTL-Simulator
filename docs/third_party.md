# Third-party dependencies

DESIGN_GUIDE.md section 1.4 requires that code vendored into `third_party/`
retain its original license text and be recorded here with its license, upstream
URL and pinned revision. This file also records dependencies Enzan *acquires*
rather than vendors, because the license obligations are the same either way and
a single list is easier to audit than two.

Enzan is **GPLv3**, a consequence of linking NVC for the VHDL frontend. Every
dependency below is compatible with that.

## Vendored (`third_party/`)

**Nothing is vendored yet.**

Vendoring is reserved for cases where it is unavoidable. NVC is the expected
case, since it is consumed as a pinned fork of internal APIs rather than as a
released library (DESIGN_GUIDE.md section 5.2); it arrives at M6.

## Acquired at configure time

The build prefers a system package and falls back to building from source. See
`cmake/EnzanDependencies.cmake`.

| Dependency | Version / revision | License | Purpose | Acquisition |
|---|---|---|---|---|
| [slang](https://github.com/MikePopoloski/slang) | `v11.0` | MIT | SystemVerilog frontend (M1) | source build; not packaged by any distribution |
| [GoogleTest](https://github.com/google/googletest) | 1.12 or later, `v1.17.0` when fetched | BSD-3-Clause | unit tests | system package, source fallback |
| [zlib](https://zlib.net/) | 1.2.11 or later | zlib | FST waveform compression | system package |
| [Tcl](https://www.tcl-lang.org/) | 8.6 or later | BSD-style | control and configuration language (M3) | system package |
| [Qt](https://www.qt.io/) | 6.4 or later | LGPLv3 | graphical front end (M2) | system package, dynamically linked |
| [LLVM](https://llvm.org/) | 17 or later | Apache-2.0 with LLVM exception | JIT backend (M8, optional) | system package, off by default |
| [NVC](https://github.com/nickg/nvc) | pinned at M6 | GPLv3 | VHDL frontend (M6) | vendored submodule |

slang itself builds against fmt, Boost headers and mimalloc, which it acquires
on its own terms; all three are permissively licensed.

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

## Adding a dependency

Per DESIGN_GUIDE.md section 11.2, adding one requires all of:

1. An entry in section 11.3 of the design guide.
2. A `find_package` path with an error message naming the package to install.
3. A `FetchContent` fallback, where one is possible.
4. Installation in CI.
5. A license check against section 1.4, and a row in this file.
