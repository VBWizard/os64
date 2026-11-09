#!/usr/bin/env bash
set -eu
cd "$(git rev-parse --show-toplevel)"
work_test_dir=$(mktemp -d)
trap 'rm -rf "$work_test_dir"' EXIT
mkdir -p "$work_test_dir/os64"
cat > "$work_test_dir/os64/gui.h" <<'HEADER'
#include <stdint.h>
typedef struct { int unused; } os64_gui_window_state_t;
int64_t os64_gui_window_get_state(int64_t, os64_gui_window_state_t *);
int64_t os64_gui_event_ring(int64_t, uint32_t);
HEADER
cc -D_DEFAULT_SOURCE -std=c11 -pthread -g -O1 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -I "$work_test_dir" \
    -I userland/libos64/include -I abi/include tools/test_work_host.c \
    -o "$work_test_dir/test_work"
"$work_test_dir/test_work"
