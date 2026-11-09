#!/usr/bin/env bash
# D6: lifetime cases and an hour-equivalent fragment replacement workload.
set -euo pipefail
cd "$(dirname "$0")/.."
exec bash tools/test_html_dom_host.sh --reclaim
