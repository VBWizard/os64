#!/bin/bash
set -eu
cd "$(git rev-parse --show-toplevel)"
make -C userland -j8 js-core
python3 tools/test_js_port_target.py
