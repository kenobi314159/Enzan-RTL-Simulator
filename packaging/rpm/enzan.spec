# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fedora and RHEL 9 packages of Enzan (DESIGN_GUIDE.md section 11.7). The @...@ fields are filled
# in from the tarball by packaging/container.sh.

%global upstream_version @UPSTREAM_VERSION@

Name:           enzan
Version:        @PACKAGE_VERSION@
Release:        1%{?dist}
Summary:        Event-driven mixed-language RTL simulator

# Enzan and NVC are GPL-3.0-or-later; VHDL's standard libraries and vhpi_user.h Apache-2.0, but
# for one of NVC's, LGPL-3.0-or-later; slang, fmt, FST and FastLZ MIT; NVC's cpustate ISC; LZ4
# BSD-2-Clause; slang's boost_unordered and expected BSL-1.0; NVC's sha1 public domain.
License:        GPL-3.0-or-later AND Apache-2.0 AND LGPL-3.0-or-later AND MIT AND ISC AND BSD-2-Clause AND BSL-1.0 AND LicenseRef-Fedora-Public-Domain
URL:            https://github.com/kenobi314159/Enzan-RTL-Simulator
Source0:        %{url}/releases/download/v%{upstream_version}/enzan-%{upstream_version}.tar.xz

# RHEL 9's GCC is 11, and Enzan needs 13 (DESIGN_GUIDE.md section 11.1). RHEL's build flags load
# the annobin plugin, which the toolset's compiler has only from a package of its own.
%if 0%{?rhel} == 9
BuildRequires:  gcc-toolset-13-gcc
BuildRequires:  gcc-toolset-13-gcc-c++
BuildRequires:  gcc-toolset-13-annobin-plugin-gcc
%else
BuildRequires:  gcc >= 13
BuildRequires:  gcc-c++ >= 13
%endif
BuildRequires:  cmake >= 3.24
BuildRequires:  ninja-build
BuildRequires:  flex
BuildRequires:  libzstd-devel
BuildRequires:  zlib-devel
BuildRequires:  qt6-qtbase-devel
BuildRequires:  tcl-devel
BuildRequires:  gtest-devel

# What the tarball carries, built into the binaries (DESIGN_GUIDE.md section 11.6).
Provides:       bundled(slang) = 11.0
Provides:       bundled(fmt) = 12.1.0
Provides:       bundled(nvc) = 1.23.0
Provides:       bundled(fstapi)
Provides:       bundled(lz4)
Provides:       bundled(fastlz)
# The front end is suggested, not recommended, since dnf installs what is recommended (section
# 11.7): a machine without a display installs no GUI toolkit.
Suggests:       %{name}-gui

%description
Enzan simulates SystemVerilog and VHDL, each alone or both in one design, writes
VCD and FST waveforms, and runs cocotb testbenches through VPI and VHPI.

This package is the simulator, VHDL's standard libraries, the VPI and VHPI
headers and cocotb's makefile for Enzan. The graphical debugging environment
`enzan --gui` opens is in enzan-gui, so that a machine without a display
installs no GUI toolkit.

%package gui
Summary:        Event-driven mixed-language RTL simulator - graphical front end
Requires:       %{name}%{?_isa} = %{version}-%{release}

%description gui
Enzan simulates SystemVerilog and VHDL, each alone or both in one design, writes
VCD and FST waveforms, and runs cocotb testbenches through VPI and VHPI.

This package is the window `enzan --gui` opens on a simulation: its waveform
view, design structure and transcript.

%prep
%autosetup -n enzan-%{upstream_version}

%build
%if 0%{?rhel} == 9
. /opt/rh/gcc-toolset-13/enable
%endif
# The tarball carries every source a build would download (section 11.6), and a download now is
# a dependency the tarball lacks, so it is an error rather than an attempt.
%cmake -G Ninja -DENZAN_ALLOW_FETCH=OFF -DFETCHCONTENT_FULLY_DISCONNECTED=ON
%cmake_build

%install
%cmake_install

%check
%ctest

%files
%license LICENSE
%doc README.md docs/third_party.md docs/mixed-language.md docs/libraries.md docs/signal-lists.md
%{_bindir}/enzan
%{_prefix}/lib/enzan/
%{_includedir}/enzan/
%{_datadir}/enzan/

%files gui
%{_libexecdir}/enzan/

%changelog
* @CHANGELOG_DATE@ @PACKAGER@ - @PACKAGE_VERSION@-1
- Upstream release @UPSTREAM_VERSION@.
