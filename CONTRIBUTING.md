# Contributing to Enzan

Building Enzan from source, checking a change the way CI does, and where the developer
documentation is.

**Read [`DESIGN_GUIDE.md`](DESIGN_GUIDE.md) before changing anything.** It is the normative
specification: it fixes the simulation semantics, the coding standards, the testing requirements
and the definition of done, and a commit that makes it wrong is a defect whatever the code does.

## Prerequisites

Enzan builds on Linux only.

| Requirement | Minimum | Notes |
|---|---|---|
| C++ compiler | GCC 13 or Clang 17 | C++20 is required by slang |
| CMake | 3.24 | presets are used throughout |
| Ninja | any recent | the presets all use the Ninja generator |
| zlib | 1.2.11 | FST waveform compression |
| Python 3 | 3.8 | optional; only for the waveform structure checks in the test suite |
| GoogleTest | 1.12 | built from source automatically if absent |
| Qt 6 | 6.4 | the GUI; not needed with `-DENZAN_BUILD_GUI=OFF` |
| Tcl | 8.6 | the GUI's themes and signal lists; not needed with `-DENZAN_BUILD_GUI=OFF` |
| flex | 2.6 | generates NVC's lexer; not needed with `-DENZAN_ENABLE_VHDL=OFF` |
| zstd | 1.4 | NVC's library files; not needed with `-DENZAN_ENABLE_VHDL=OFF` |
| ccache | any | optional; used automatically when present |

```sh
# Arch and derivatives
sudo pacman -S --needed base-devel cmake ninja zlib gtest tcl ccache qt6-base flex zstd
# Debian and Ubuntu
sudo apt install build-essential cmake ninja-build zlib1g-dev libgtest-dev tcl-dev ccache \
                 qt6-base-dev flex libzstd-dev
# Fedora
sudo dnf install gcc-c++ cmake ninja-build zlib-devel gtest-devel tcl-devel ccache \
                 qt6-qtbase-devel flex libzstd-devel
```

If a dependency is missing, CMake stops with a message naming the package to install. The VHDL
frontend is built on [NVC](https://github.com/nickg/nvc), a git submodule: clone with
`--recursive`, or run `git submodule update --init third_party/nvc` before configuring.

## Configure, build and test

```sh
git clone --recursive https://github.com/kenobi314159/Enzan-RTL-Simulator.git
cd Enzan-RTL-Simulator
cmake --preset gcc-debug
cmake --build --preset gcc-debug
ctest --preset gcc-debug
./build/gcc-debug/bin/enzan --version
```

`cmake --list-presets` shows every preset: `gcc-` and `clang-` variants of `debug`, `release` and
`relwithdebinfo`, `clang-sanitize` for the AddressSanitizer and UndefinedBehaviorSanitizer
configuration CI runs, and `gcc-profile`, `relwithdebinfo` with frame pointers kept everywhere so
that `perf record --call-graph fp` and `samply` get whole stacks. Each preset builds into its own
directory under `build/`. Time a simulation in an optimised build only: a debug build is an order
of magnitude slower, and what dominates its profile is what the optimiser would remove.

**The first build takes a while.** No distribution packages slang, the SystemVerilog frontend, so
the build downloads and compiles it. Work on the kernel, the waveform database or the GUI can leave
it out, and the VHDL frontend and the GUI too:

```sh
cmake --preset gcc-debug -DENZAN_ENABLE_SYSTEMVERILOG=OFF
cmake --preset gcc-debug -DENZAN_ENABLE_SYSTEMVERILOG=OFF -DENZAN_ENABLE_VHDL=OFF -DENZAN_BUILD_GUI=OFF
```

| Option | Default | Effect |
|---|---|---|
| `ENZAN_BUILD_TESTS` | `ON` | build the unit test suites |
| `ENZAN_ENABLE_SYSTEMVERILOG` | `ON` | build the slang-based frontend |
| `ENZAN_ENABLE_VHDL` | `ON` | build the NVC-based frontend; needs flex, zstd and the `third_party/nvc` submodule |
| `ENZAN_BUILD_GUI` | `ON` | build the Qt 6 front end; needs Qt 6 and Tcl |
| `ENZAN_ENABLE_JIT` | `OFF` | build the LLVM JIT backend, which is not written yet |
| `ENZAN_WERROR` | `OFF` | treat warnings as errors, as CI does |
| `ENZAN_ALLOW_FETCH` | `ON` | allow missing dependencies to be built from source |
| `ENZAN_SANITIZERS` | empty | semicolon-separated sanitizer list |
| `ENZAN_SLANG_GIT_TAG`, `ENZAN_SLANG_VERSION` | pinned | try another slang revision without editing the build |

Changing a pinned revision is a deliberate act: upgrading slang or NVC is expected to break the
adapters in `src/frontend/`, which is what the golden tests are there to catch.

## Before you push

The checks CI runs are all runnable locally:

```sh
clang-format --dry-run --Werror $(git ls-files '*.h' '*.cpp')
./scripts/check-license-headers.sh
cmake --preset clang-debug -DENZAN_WERROR=ON
cmake --build --preset clang-debug && ctest --preset clang-debug
clang-tidy -p build/clang-debug --quiet $(git ls-files 'src/*.cpp' 'tools/*.cpp' 'tests/*.cpp')
```

Configure with every optional component on, as the defaults are, before running `clang-tidy`: a
translation unit the build does not compile is linted with default flags, cannot find the slang,
NVC or Qt headers, and reports whatever its half-parsed view of the file suggests.

The GUI's tests need no display; they select Qt's offscreen platform themselves. The suite compares
designs against Icarus Verilog when `iverilog` is installed, and against GHDL when `ghdl` is.
Benchmarks are a target of their own, since a measurement taken alongside the rest of the suite
compares with nothing: `cmake --build --preset gcc-release --target enzan-bench`. The conformance
suites run from their scripts under `tests/conformance/`; cocotb's, for example, is
`tests/conformance/cocotb/run-suite.sh <enzan> <work>`, with `vhdl` after it for VHDL toplevels.

Every source file begins with `// SPDX-License-Identifier: GPL-3.0-or-later`. Commit messages carry
a category prefix, followed by the path of the area when the change is confined to one:
`runtime: scheduler: Drain the NBA region before Observed`. Section 17 of the design guide has the
rest of the working agreements.

## Developer documentation

- [`DESIGN_GUIDE.md`](DESIGN_GUIDE.md) — the design and the rules every change follows.
- Each component under `src/` has a `README.md` with its responsibility and allowed dependencies.
- [`docs/development/nvc_integration.md`](docs/development/nvc_integration.md) — how the VHDL
  frontend uses NVC, and how to upgrade it.
- [`docs/development/releasing.md`](docs/development/releasing.md) — making a release.
- [`packaging/README.md`](packaging/README.md) — each distribution's recipe and how packages are
  built.
- [`bench/README.md`](bench/README.md) — the benchmarks.
- [`docs/third_party.md`](docs/third_party.md) — dependencies, their licenses and their pinned
  revisions.
