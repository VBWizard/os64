#!/usr/bin/env bash
# test262_fetch.sh — ECMA-262/402's conformance suite, Intl's oracle
# (INTL.md § 8), checked out at ONE pinned commit into .vendor/test262,
# which .gitignore covers: the tree carries no copy of the suite, only the
# expectations written against it (tools/test262/).
#
#   tools/test262_fetch.sh                  the checkout, made or moved to the pin
#   TEST262_DIR=<dir> tools/test262_fetch.sh   somewhere else
#
# The checkout is shallow (the pinned commit alone), partial (no file's
# bytes are fetched until it is checked out) and sparse (only the
# directories below). Run again with the checkout present, it moves to the
# pin and fetches only what the pin or a new directory adds.
#
# THE PIN is for everyone: a slice that needs a newer suite moves it and
# says so in its PR, since every expectation file was written against the
# commit named here. A slice adds the directories it ships to the list;
# `harness` is what the tests themselves include.

set -euo pipefail

TEST262_COMMIT=d11543772591a5c24b3229e6195a74c73e56032e
TEST262_REMOTE=https://github.com/tc39/test262.git
TEST262_DIRS=(
    harness
    test/intl402/Intl
    test/intl402/Locale
    test/intl402/DateTimeFormat
    test/intl402/NumberFormat
    test/intl402/PluralRules
    test/intl402/RelativeTimeFormat
    test/intl402/ListFormat
)

top=$(git rev-parse --show-toplevel)
dest=${TEST262_DIR:-$top/.vendor/test262}
mkdir -p "$dest"
cd "$dest"
if [ ! -d .git ]; then
    git init -q
    git remote add origin "$TEST262_REMOTE"
fi
# Cone mode: each name is a directory, taken whole with everything under it.
git sparse-checkout set --cone "${TEST262_DIRS[@]}"
if [ "$(git rev-parse -q --verify 'HEAD^{commit}' 2>/dev/null || true)" != "$TEST262_COMMIT" ]; then
    if ! git cat-file -e "$TEST262_COMMIT^{commit}" 2>/dev/null; then
        git fetch -q --depth 1 --filter=blob:none origin "$TEST262_COMMIT"
    fi
    git -c advice.detachedHead=false checkout -q --detach "$TEST262_COMMIT"
fi
echo "test262 at $TEST262_COMMIT in $dest: ${TEST262_DIRS[*]}"
