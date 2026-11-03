#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Verifies the subject line of every commit a branch adds: that it begins with a category, one
# lowercase word and a colon, and that it is 72 characters at most. Run from anywhere in the
# working tree. With no argument it checks what HEAD adds to origin/main; a revision range checks
# that range instead. Exits non-zero and lists the offenders when any subject fails.
#
# Only the category's shape is checked, not the word. Which word fits a change is its author's
# call, and a closed list would refuse a sensible one nobody had thought of. A subject with no
# category at all is what the check exists for, because the log is read and searched by its
# prefixes.
#
# Merge commits are skipped: GitHub writes them, from the pull request's title.

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

readonly range="${1:-origin/main..HEAD}"
readonly limit=72
readonly category='^[a-z][a-z0-9-]*: [^ ]'

# A base this clone does not have would make `git log` fail. A base it has at an old revision
# would make it check commits that are not the branch's. CI fetches the whole history for this.
readonly base="${range%%..*}"
if ! git rev-parse --verify --quiet "${base}^{commit}" >/dev/null; then
    printf "'%s' is not a commit in this clone; fetch it, or give a revision range.\n" "$base" >&2
    exit 1
fi

checked=0
failures=()
while IFS= read -r line; do
    commit="${line%% *}"
    subject="${line#* }"
    checked=$((checked + 1))
    if [[ ! "$subject" =~ $category ]]; then
        failures+=("$commit has no lowercase category: $subject")
    fi
    if [[ ${#subject} -gt $limit ]]; then
        failures+=("$commit is ${#subject} characters long, over $limit: $subject")
    fi
done < <(git log --no-merges --format='%h %s' "$range")

if [[ ${#failures[@]} -gt 0 ]]; then
    printf 'Commit subjects that break the convention:\n' >&2
    printf '  %s\n' "${failures[@]}" >&2
    exit 1
fi

# Nothing to check is a pass here, unlike in the licence check: a push to main adds no commits
# that main lacks.
printf 'All %d commit subjects in %s carry a category and fit in %d characters.\n' \
    "$checked" "$range" "$limit"
