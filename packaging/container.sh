#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds and tests Enzan's packages inside a container of the distribution they are for.
# packaging/build-package.sh runs it once for each phase:
#
#   container.sh deps  <tarball>          installs what building the packages needs
#   container.sh build <tarball> <out>    builds them into <out>; run with no network
#   container.sh test  <out>              installs them from <out>, and runs Enzan from them
#
# The recipes are debian/, rpm/enzan.spec and arch/PKGBUILD beside this script, their @...@ fields
# filled in from the tarball. Its name gives the version, so the version is set in one place, the
# project() of CMakeLists.txt, from which scripts/make-dist.sh names the tarball.

set -euo pipefail
shopt -s nullglob

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
packager=${ENZAN_PACKAGER:-Jan Kubalek <kubalekj492@gmail.com>}
jobs=${ENZAN_JOBS:-$(nproc)}
work=/work

fail() {
    echo "container.sh: $1" >&2
    exit 1
}

. /etc/os-release
case " $ID ${ID_LIKE:-} " in
    *" debian "* | *" ubuntu "*) family=deb ;;
    *" fedora "* | *" rhel "*) family=rpm ;;
    *" arch "*) family=arch ;;
    *) fail "there is no recipe for $PRETTY_NAME" ;;
esac

# --- the recipe, filled in from the tarball ------------------------------------------------------

tarball_version() {
    local name
    name=$(basename "$1" .tar.xz)
    [[ $name == enzan-* ]] || fail "$1 is not a tarball make-dist.sh wrote"
    echo "${name#enzan-}"
}

# The commit's time, which every file in the tarball carries, for the changelog.
tarball_time() {
    local scratch
    scratch=$(mktemp -d)
    tar -xJf "$1" -C "$scratch" "enzan-$2/.tarball-revision"
    stat -c %Y "$scratch/enzan-$2/.tarball-revision"
    rm -rf "$scratch"
}

# `0.2.0-dev` sorts after `0.2.0` everywhere unless the suffix is spelled so that it does not:
# with a tilde for dpkg and RPM, and run on for pacman.
prepare() {
    local tarball=$1 upstream version time
    upstream=$(tarball_version "$tarball")
    time=$(tarball_time "$tarball" "$upstream")
    rm -rf "$work"
    mkdir -p "$work"
    case $family in
        deb)
            version=${upstream//-/\~}
            cp "$tarball" "$work/enzan_$version.orig.tar.xz"
            tar -xJf "$tarball" -C "$work"
            source_tree=$work/enzan-$upstream
            cp -r "$here/debian" "$source_tree/debian"
            sed -i "s|@PACKAGER@|$packager|" "$source_tree/debian/control"
            {
                echo "enzan ($version-1~$ID$VERSION_ID) ${VERSION_CODENAME:-unstable}; urgency=medium"
                echo
                echo "  * Upstream release $upstream."
                echo
                echo " -- $packager  $(date -R -u -d "@$time")"
            } >"$source_tree/debian/changelog"
            ;;
        rpm)
            version=${upstream//-/\~}
            mkdir -p "$work/SOURCES" "$work/SPECS"
            cp "$tarball" "$work/SOURCES/"
            sed -e "s|@UPSTREAM_VERSION@|$upstream|g" -e "s|@PACKAGE_VERSION@|$version|g" \
                -e "s|@PACKAGER@|$packager|g" \
                -e "s|@CHANGELOG_DATE@|$(date -u -d "@$time" '+%a %b %d %Y')|g" \
                "$here/rpm/enzan.spec" >"$work/SPECS/enzan.spec"
            ;;
        arch)
            version=${upstream//-/}
            cp "$tarball" "$work/"
            sed -e "s|@UPSTREAM_VERSION@|$upstream|g" -e "s|@PACKAGE_VERSION@|$version|g" \
                -e "s|@PACKAGER@|$packager|g" \
                -e "s|@SHA256@|$(sha256sum "$tarball" | cut -d' ' -f1)|g" \
                "$here/arch/PKGBUILD" >"$work/PKGBUILD"
            ;;
    esac
}

# --- what building needs -------------------------------------------------------------------------

deps() {
    case $family in
        deb)
            export DEBIAN_FRONTEND=noninteractive
            apt-get update
            apt-get install -y --no-install-recommends build-essential dpkg-dev xz-utils
            prepare "$1"
            apt-get build-dep -y "$source_tree"
            ;;
        rpm)
            # RHEL 9 has Qt 6 and GoogleTest in EPEL, ninja in CodeReady Builder, and builddep in
            # dnf's plugins, which Fedora's dnf has built in.
            if [[ $ID != fedora ]]; then
                dnf install -y epel-release dnf-plugins-core
                dnf config-manager --set-enabled crb
            fi
            dnf install -y rpm-build tar xz
            prepare "$1"
            dnf builddep -y "$work/SPECS/enzan.spec"
            ;;
        arch)
            pacman -Syu --noconfirm --needed base-devel
            prepare "$1"
            # shellcheck source=/dev/null
            (. "$work/PKGBUILD" && pacman -S --noconfirm --needed "${makedepends[@]}")
            # makepkg will not run as root.
            useradd --create-home builder
            ;;
    esac
}

# --- the packages --------------------------------------------------------------------------------

build() {
    local out=$2
    prepare "$1"
    case $family in
        deb)
            (cd "$source_tree" && DEB_BUILD_OPTIONS="parallel=$jobs" dpkg-buildpackage -us -uc)
            cp "$work"/*.deb "$work"/*.ddeb "$work"/*.dsc "$work"/*.debian.tar.* \
                "$work"/*.orig.tar.xz "$out/"
            ;;
        rpm)
            rpmbuild -ba --define "_topdir $work" --define "_smp_build_ncpus $jobs" \
                "$work/SPECS/enzan.spec"
            cp "$work"/RPMS/*/*.rpm "$work"/SRPMS/*.rpm "$out/"
            ;;
        arch)
            chown -R builder "$work"
            runuser -u builder -- env CMAKE_BUILD_PARALLEL_LEVEL="$jobs" \
                bash -c "cd '$work' && makepkg --noconfirm"
            cp "$work"/*.pkg.tar.zst "$work/PKGBUILD" "$out/"
            ;;
    esac
    if [[ -n ${HOST_UID:-} ]]; then chown -R "$HOST_UID:${HOST_GID:-$HOST_UID}" "$out"; fi
}

# --- the packages, installed ---------------------------------------------------------------------

# One package file of the build in <out>, by the name it starts with.
package() {
    local matches=("$1"/$2)
    [[ ${#matches[@]} -eq 1 ]] || fail "expected one package matching $2 in $1: ${matches[*]:-none}"
    echo "${matches[0]}"
}

install_packages() {
    case $family in
        deb) DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "$@" ;;
        rpm) dnf install -y "$@" ;;
        arch) pacman -U --noconfirm "$@" ;;
    esac
}

test_packages() {
    local out=$1 kernel gui
    case $family in
        deb)
            apt-get update
            kernel=$(package "$out" 'enzan_*.deb')
            gui=$(package "$out" 'enzan-gui_*.deb')
            ;;
        rpm)
            if [[ $ID != fedora ]]; then dnf install -y epel-release; fi
            kernel=$(package "$out" 'enzan-[0-9]*.x86_64.rpm')
            gui=$(package "$out" 'enzan-gui-[0-9]*.x86_64.rpm')
            ;;
        arch)
            # The whole system, as Arch supports no partial upgrade.
            pacman -Syu --noconfirm
            kernel=$(package "$out" 'enzan-[0-9]*-x86_64.pkg.tar.zst')
            gui=$(package "$out" 'enzan-gui-[0-9]*-x86_64.pkg.tar.zst')
            ;;
    esac

    # The simulator alone first: a machine without a display installs no GUI toolkit.
    install_packages "$kernel"
    if ldconfig -p | grep -q libQt6Core; then fail "installing $kernel installed Qt"; fi
    "$here/smoke-test.sh" kernel

    install_packages "$gui"
    "$here/smoke-test.sh" gui
}

case ${1:-} in
    deps) [[ $# -eq 2 ]] || fail "usage: $0 deps <tarball>"; deps "$2" ;;
    build) [[ $# -eq 3 ]] || fail "usage: $0 build <tarball> <out>"; build "$2" "$3" ;;
    test) [[ $# -eq 2 ]] || fail "usage: $0 test <out>"; test_packages "$2" ;;
    *) fail "usage: $0 deps|build|test ..." ;;
esac
