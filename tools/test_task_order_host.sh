#!/bin/bash
# Run yonder's task order (userland/apps/yonder/task_order.c: which of a
# page's ready tasks runs next, priority among the young and age among
# the old) on the host under ASan and UBSan, against the cases, the
# bound's guarantee and the random walk in tools/test_task_order_host.c.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c17 -g -O1 -Wall -Wextra -Werror \
   -fsanitize=address,undefined -fno-sanitize-recover=undefined \
   -I userland/apps/yonder \
   tools/test_task_order_host.c userland/apps/yonder/task_order.c -o "$work/test_task_order"

"$work/test_task_order"
