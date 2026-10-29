# Installing Enzan

Enzan runs on Linux only. Each release is published with packages for the distributions below,
built and tested on each of them, and with a source tarball that builds anywhere the toolchain
does.

## From a release's packages

Download the packages for your distribution from the
[release page](https://github.com/kenobi314159/Enzan-RTL-Simulator/releases). There are two:
`enzan`, the simulator, and `enzan-gui`, the window `enzan --gui` opens. The simulator needs no GUI
toolkit, so a compute server or a CI runner installs `enzan` alone and gets no Qt with it.

| Distribution | Install with |
|---|---|
| Ubuntu 24.04 | `sudo apt install ./enzan_*ubuntu24.04*.deb ./enzan-gui_*ubuntu24.04*.deb` |
| Debian 13 | `sudo apt install ./enzan_*debian13*.deb ./enzan-gui_*debian13*.deb` |
| Fedora | `sudo dnf install ./enzan-*.fc*.rpm ./enzan-gui-*.fc*.rpm` |
| RHEL 9, AlmaLinux 9, Rocky Linux 9 | `sudo dnf install epel-release`, then `sudo dnf install ./enzan-*.el9.*.rpm ./enzan-gui-*.el9.*.rpm` |
| Arch Linux | `sudo pacman -U enzan-[0-9]*.pkg.tar.zst enzan-gui-[0-9]*.pkg.tar.zst` |

RHEL 9 needs EPEL for the front end only: Qt 6 is there. Ubuntu 22.04 and Debian 12 are not
packaged for, because their compilers are older than Enzan needs.

The packages are not served from a repository yet, so a new release is installed the same way
again; `SHA256SUMS` on the release page checks what was downloaded.

What a package installs:

| Path | What it is |
|---|---|
| `/usr/bin/enzan` | the simulator, and the one command there is |
| `/usr/libexec/enzan/enzan-gui` | the front end, which only `enzan --gui` starts (`enzan-gui`) |
| `/usr/lib/enzan/vhdl/` | VHDL's standard libraries, found without being named |
| `/usr/include/enzan/` | `vpi_user.h`, `sv_vpi_user.h` and `vhpi_user.h`, to compile a VPI or VHPI application against |
| `/usr/share/enzan/cocotb/` | cocotb's makefile for Enzan; see the README's cocotb section |

## From the source tarball

The release's `enzan-X.Y.Z.tar.xz`, not the archive GitHub offers beside it: the tarball carries
NVC and the sources the build would otherwise download, slang's among them, and builds with no
network. With the prerequisites the README lists:

```sh
tar -xf enzan-X.Y.Z.tar.xz
cmake -S enzan-X.Y.Z -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
cmake --install build --prefix ~/.local
```

`--prefix` may be anywhere, and the installation may be moved afterwards: `enzan` finds the rest of
Enzan from where it is itself. Two versions installed side by side, in `/opt/enzan/0.1.0` and
`/opt/enzan/0.2.0` say, each run their own front end and their own standard libraries.

## From a checkout

As [CONTRIBUTING.md](../CONTRIBUTING.md) says: configure, build, and `cmake --install` the build
directory the same way. A checkout downloads slang and its dependencies the first time it is
configured.
