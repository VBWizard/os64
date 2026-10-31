#!/usr/bin/env bash
# wpt_fetch.sh — web-platform-tests, the Modern Web arc's oracle
# (MODERN_WEB.md § The oracle), checked out at ONE pinned commit into
# .vendor/wpt, which .gitignore covers: the tree carries no copy of the
# suite, only the expectations written against it (tools/wpt/).
#
#   tools/wpt_fetch.sh            the checkout, made or moved to the pin
#   WPT_DIR=<dir> tools/wpt_fetch.sh   somewhere else
#
# The suite is gigabytes, so the checkout is shallow (the pinned commit
# alone), partial (no file's bytes are fetched until it is checked out) and
# sparse (only the directories below are checked out). Run again with the
# checkout present, it moves to the pin and fetches only what the pin or a
# new directory adds.
#
# THE PIN is for everyone: a slice that needs a newer directory moves it
# and says so in its PR, since every expectation file was written against
# the commit named here. A slice adds the directories its brief names to
# the list; `resources` (testharness.js), `common` and `css/support` (the
# CSS tests' parsing and computed-value helpers) are what the tests
# themselves load, and `tools` is the suite's own, which makes
# MANIFEST.json: the list of every test, its type and the pages a script
# test makes, which the runner reads (tools/wpt/pages.py). It is built over
# what is checked out, again whenever the pin or the list moves; without
# Python it is not, and the runner finds tests by their file names.

set -euo pipefail

WPT_COMMIT=a699fb16767be39ba3e61a8e8b6bef60383f08ee
WPT_REMOTE=https://github.com/web-platform-tests/wpt.git
WPT_DIRS=(
    resources
    common
    tools
    dom/nodes
    webstorage
    url
    encoding
    hr-time
    user-timing
    css/cssom-view
    html/browsers/history
    css/cssom
    css/mediaqueries
    fetch/api
    xhr
    html/semantics/scripting-1/the-script-element
    css/css-logical
    css/css-sizing
    css/css-images
    css/css-text
    css/css-ui
    css/css-overflow
    css/support
    html/browsers/the-window-object
    html/dom
    html/canvas/element
    html/canvas/resources
    html/webappapis/animation-frames
    html/webappapis/microtask-queuing
    html/webappapis/scripting
    secure-contexts
    WebCryptoAPI
    page-visibility
    html/interaction/focus
    dom/events
    html/webappapis/structured-clone
    requestidlecallback
    webmessaging/without-ports
    html/semantics/document-metadata/the-link-element
    html/semantics/document-metadata/the-style-element
    preload
    html/semantics/forms/the-select-element
    html/semantics/forms/the-option-element
    html/semantics/forms/the-optgroup-element
    import-maps
    html/semantics/forms/textfieldselection
    html/semantics/forms/the-input-element
    html/semantics/forms/form-control-infrastructure
    html/semantics/forms/the-textarea-element
    html/semantics/forms/the-button-element
    html/semantics/forms/the-label-element
    html/semantics/forms/the-form-element
    html/semantics/forms/the-fieldset-element
    html/semantics/forms/the-legend-element
    html/semantics/forms/the-output-element
    html/semantics/forms/constraints
    html/infrastructure/common-dom-interfaces/collections
    html/semantics/tabular-data
    interfaces
    uievents
    custom-elements
)

top=$(git rev-parse --show-toplevel)
dest=${WPT_DIR:-$top/.vendor/wpt}
mkdir -p "$dest"
cd "$dest"
if [ ! -d .git ]; then
    git init -q
    git remote add origin "$WPT_REMOTE"
fi
# Cone mode: each name is a directory, taken whole with everything under it.
git sparse-checkout set --cone "${WPT_DIRS[@]}"
if [ "$(git rev-parse -q --verify 'HEAD^{commit}' 2>/dev/null || true)" != "$WPT_COMMIT" ]; then
    if ! git cat-file -e "$WPT_COMMIT^{commit}" 2>/dev/null; then
        git fetch -q --depth 1 --filter=blob:none origin "$WPT_COMMIT"
    fi
    git -c advice.detachedHead=false checkout -q --detach "$WPT_COMMIT"
fi
stamp="$WPT_COMMIT ${WPT_DIRS[*]}"
manifest="no MANIFEST.json (the runner finds tests by file name)"
if [ -f MANIFEST.json ] && [ "$(cat MANIFEST.stamp 2>/dev/null)" = "$stamp" ]; then
    manifest="MANIFEST.json"
elif command -v python3 > /dev/null 2>&1 &&
    python3 -I -c '
import sys
sys.path.insert(0, ".")
import tools.localpaths
from tools.manifest import update
sys.argv = ["manifest", "--no-download", "--rebuild", "--no-parallel", "-p", "MANIFEST.json",
            "--cache-root", ".wptcache"]
update.main()
' > manifest.log 2>&1; then
    printf '%s\n' "$stamp" > MANIFEST.stamp
    manifest="MANIFEST.json, built"
else
    rm -f MANIFEST.json MANIFEST.stamp
    echo "wpt: MANIFEST.json could not be built ($dest/manifest.log); file names it is" >&2
fi
echo "wpt: $(git rev-parse --short HEAD) in $dest, ${#WPT_DIRS[@]} directories, $manifest"
