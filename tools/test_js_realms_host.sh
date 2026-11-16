#!/bin/bash
# Focused target-engine ownership matrix for the routing mutation campaign.
set -eu
export JS_RUNTIME_REALMS_ONLY=1
exec bash "$(dirname "$0")/test_js_runtime_host.sh"
