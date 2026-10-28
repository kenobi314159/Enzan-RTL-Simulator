#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Writes the source tarball of a release (DESIGN_GUIDE.md section 11.6):
#
#   scripts/make-dist.sh [<revision> [<directory>]]
#
# enzan-<version>.tar.xz and its .sha256 are written into <directory>, build/dist by default, from
# <revision>, HEAD by default. The tarball holds the revision's tree; each submodule at the commit
# the revision pins, NVC's included; in third_party/bundled/, the sources FetchContent downloads
# to build slang, slang's own and fmt; and .tarball-revision, the revision it was made from. A
# distribution's builder has no network, and this is everything a build needs that a distribution
# does not package.
#
# What is bundled is what CMake downloads, so nothing here restates which revisions slang pins: the
# revision is configured once, with every download forced, and the sources it fetched are copied.
# The tarball is made from what is committed, whatever the working tree changes, and the same
# revision gives the same bytes.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
revision=${1:-HEAD}
out=${2:-$root/build/dist}

commit=$(git -C "$root" rev-parse --verify --quiet "$revision^{commit}") || {
    echo "make-dist.sh: '$revision' is not a revision" >&2
    exit 1
}

staging=$(mktemp -d)
trap 'rm -rf "$staging"' EXIT
tree=$staging/tree
mkdir "$tree"

git -C "$root" archive "$commit" | tar -x -C "$tree"

# A submodule is a commit of another repository, which `git archive` leaves out.
while read -r _ type submodule path; do
    [[ $type == commit ]] || continue
    if ! git -C "$root/$path" cat-file -e "$submodule^{commit}" 2>/dev/null; then
        echo "make-dist.sh: $path does not have $submodule; fetch it with" >&2
        echo "  git submodule update --init $path" >&2
        exit 1
    fi
    git -C "$root/$path" archive "$submodule" | tar -x -C "$tree/$path"
done < <(git -C "$root" ls-tree -r "$commit")

# Every download forced, a system package found or not, and only the frontend that needs them.
cmake -S "$tree" -B "$staging/configure" \
    -DENZAN_ENABLE_SYSTEMVERILOG=ON -DENZAN_ENABLE_VHDL=OFF -DENZAN_BUILD_GUI=OFF \
    -DENZAN_BUILD_TESTS=OFF -DENZAN_ALLOW_FETCH=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_slang=ON -DFETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER \
    >"$staging/configure.log" 2>&1 || {
    cat "$staging/configure.log" >&2
    echo "make-dist.sh: configuring $commit to fetch its dependencies failed" >&2
    exit 1
}

mkdir "$tree/third_party/bundled"
for source in "$staging/configure/_deps"/*-src; do
    name=$(basename "$source" -src)
    tar -C "$source" --exclude=.git -cf - . | tar -x -C "$tree/third_party/bundled" --one-top-level="$name"
done

version=$(sed -n 's/^#define ENZAN_VERSION_STRING "\(.*\)"$/\1/p' \
    "$staging/configure/generated/common/VersionInfo.h")
[[ -n $version ]] || {
    echo "make-dist.sh: the configured version cannot be read" >&2
    exit 1
}

git -C "$root" describe --always --abbrev=8 "$commit" >"$tree/.tarball-revision"

name=enzan-$version
mv "$tree" "$staging/$name"
mkdir -p "$out"
# The commit's time and no one's ownership, in name order: the same revision, the same bytes. One
# xz thread, since a different number of them compresses differently.
tar -C "$staging" --sort=name --owner=0 --group=0 --numeric-owner --format=gnu \
    --mtime="@$(git -C "$root" log -1 --format=%ct "$commit")" -cf - "$name" \
    | xz -T1 -9 >"$out/$name.tar.xz"
(cd "$out" && sha256sum "$name.tar.xz" >"$name.tar.xz.sha256")

echo "$out/$name.tar.xz"
