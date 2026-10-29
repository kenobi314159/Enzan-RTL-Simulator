#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the packages of a release's source tarball for one distribution, in a container of it,
# and tests them there:
#
#   packaging/build-package.sh <target> <tarball> [<directory>]
#
# <target> is ubuntu-24.04, debian-13, fedora, el9 or arch. The packages are written into
# <directory>, build/packages/<target> by default. ENZAN_CONTAINER names the container engine,
# docker by default, and ENZAN_JOBS how many jobs the build runs, all the cores by default.
#
# The build runs with no network, as a distribution's builder does, so that a tarball lacking a
# source is found here rather than by one: what the build needs is installed first, from the
# recipe, into an image of its own. The packages are then installed into a fresh container and run
# there, the simulator's alone first, since it must need no GUI toolkit.

set -euo pipefail

usage() {
    echo "usage: $0 <ubuntu-24.04|debian-13|fedora|el9|arch> <tarball> [<directory>]" >&2
    exit 2
}

[[ $# -eq 2 || $# -eq 3 ]] || usage
target=$1
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tarball=$(realpath "$2")
out=${3:-$here/../build/packages/$target}
engine=${ENZAN_CONTAINER:-docker}
jobs=${ENZAN_JOBS:-$(nproc)}

# The distributions Enzan is packaged for, and the image each is built in.
case $target in
    ubuntu-24.04) image=docker.io/library/ubuntu:24.04 ;;
    debian-13) image=docker.io/library/debian:13 ;;
    fedora) image=docker.io/library/fedora:latest ;;
    el9) image=docker.io/library/almalinux:9 ;;
    arch) image=docker.io/library/archlinux:latest ;;
    *) usage ;;
esac

[[ -f $tarball ]] || {
    echo "build-package.sh: no tarball $tarball" >&2
    exit 1
}
mkdir -p "$out"
out=$(realpath "$out")

name=$(basename "$tarball")
container=enzan-packaging-$target-$$
deps_image=localhost/enzan-packaging-deps:$target
environment=(-e ENZAN_JOBS="$jobs")
[[ -n ${ENZAN_PACKAGER:-} ]] && environment+=(-e ENZAN_PACKAGER)
recipes=(-v "$here:/packaging:ro" -v "$(dirname "$tarball"):/dist:ro")

remove_container() {
    "$engine" rm -f "$container" >/dev/null 2>&1 || true
}
# The image of what the build needed is a few gigabytes, and every run installs it again.
clean_up() {
    remove_container
    "$engine" rmi -f "$deps_image" >/dev/null 2>&1 || true
}
trap clean_up EXIT

echo "== $target: installing what the build needs"
"$engine" run --name "$container" "${recipes[@]}" "${environment[@]}" "$image" \
    /packaging/container.sh deps "/dist/$name"
"$engine" commit "$container" "$deps_image" >/dev/null
remove_container

echo "== $target: building, with no network"
"$engine" run --rm --network none "${recipes[@]}" -v "$out:/out" "${environment[@]}" \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" "$deps_image" \
    /packaging/container.sh build "/dist/$name" /out

echo "== $target: installing the packages and running Enzan from them"
"$engine" run --rm -v "$here:/packaging:ro" -v "$out:/out:ro" "$image" \
    /packaging/container.sh test /out

echo "== $target: packages in $out"
