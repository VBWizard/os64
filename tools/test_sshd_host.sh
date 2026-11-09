#!/bin/sh
set -eu
exec python3 "$(dirname "$0")/test_sshd_host.py" "$@"
