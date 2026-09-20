#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Verifies that every source file carries an SPDX license identifier, as
# DESIGN_GUIDE.md section 1.4 requires. Run from anywhere in the working tree;
# exits non-zero and lists the offenders when any file is missing one.
#
# Enzan is GPLv3 as a consequence of linking NVC, so the identifier is not
# decoration: it is what makes the license of each file unambiguous to
# downstream packagers and to automated license scanners.

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

readonly expected='SPDX-License-Identifier: GPL-3.0-or-later'

# JSON admits no comments, so CMakePresets.json cannot carry a header. Markdown
# and plain text are documentation rather than source. Anything under
# third_party/ keeps its own upstream license and is recorded in
# docs/third_party.md instead.
mapfile -t files < <(
    git ls-files \
        '*.h' '*.cpp' '*.def' '*.h.in' '*.cmake' '*.sh' '*.py' \
        'CMakeLists.txt' '*/CMakeLists.txt' \
    | grep -v '^third_party/'
)

# An empty list means the glob or the exclusion has gone wrong, not that every
# file passed. Reporting success on nothing would quietly disable this check.
if [[ ${#files[@]} -eq 0 ]]; then
    printf 'No source files were found to check; the file list is wrong.\n' >&2
    exit 1
fi

missing=()
for file in "${files[@]}"; do
    # The identifier must appear in the first few lines: a header buried in the
    # middle of a file is not a header.
    if ! head -n 5 "$file" | grep -qF "$expected"; then
        missing+=("$file")
    fi
done

if [[ ${#missing[@]} -gt 0 ]]; then
    printf 'Missing "%s" in the first 5 lines of:\n' "$expected" >&2
    printf '  %s\n' "${missing[@]}" >&2
    printf '\nSee DESIGN_GUIDE.md section 1.4.\n' >&2
    exit 1
fi

printf 'All %d source files carry an SPDX license identifier.\n' "${#files[@]}"
