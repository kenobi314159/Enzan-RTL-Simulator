#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fetches a repository at one revision, for the suites and designs that test Enzan against code
# it does not hold.
#
#   fetch-revision.sh <url> <revision> <directory>
#
# A directory already holding that revision is used as it is, since a commit never moves; any
# other is replaced. Only the one commit is fetched, because the history is never needed.

set -uo pipefail

if [[ $# -ne 3 ]]; then
    printf 'usage: %s <url> <revision> <directory>\n' "$0" >&2
    exit 2
fi

readonly url="$1"
readonly revision="$2"
readonly directory="$3"

if [[ "$(git -C "$directory" rev-parse HEAD 2>/dev/null)" == "$revision" ]]; then
    exit 0
fi

rm -rf "$directory"
mkdir -p "$directory"
if ! { git -C "$directory" init --quiet \
    && git -C "$directory" fetch --quiet --depth 1 "$url" "$revision" \
    && git -C "$directory" checkout --quiet FETCH_HEAD; }; then
    printf 'could not fetch %s at %s\n' "$url" "$revision" >&2
    exit 1
fi
