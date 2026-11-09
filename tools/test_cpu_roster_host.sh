#!/bin/bash
# Drive the CPU roster on the host, under ASan and UBSan.
#
# The roster is where an APIC ID becomes a CPU number (cpu_roster.h); see
# tools/test_cpu_roster_host.c for the machines it builds, the 3900X's sparse
# IDs among them.
#
#   tools/test_cpu_roster_host.sh [random-machine rounds, default 20000]

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined \
   -I kernel/include \
   kernel/src/cpu_roster.c tools/test_cpu_roster_host.c -o "$work/test_cpu_roster"

"$work/test_cpu_roster" "${1:-20000}"
