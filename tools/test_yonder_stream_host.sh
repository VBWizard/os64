#!/bin/bash
# The navigation mailbox's STREAM on the host, under the sanitizers, with
# real pipes and two threads (docs/design/pending/DOM_D4.md): a worker
# posting a page's head, body and verdict through the bounded ring, and a
# window taking them at its own pace. tools/test_yonder_stream_host.c says
# what each case holds it to. The window's side of the stream — the parser
# fed a slice per turn — is in tools/test_yonder_scripts_host.sh, which
# hosts yonder.c itself.
set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
cleanup() {
    status=$?
    if [ "$status" -eq 0 ]; then rm -rf "$work"; else echo "yonder stream test artifacts: $work" >&2; fi
}
trap cleanup EXIT

# -fno-builtin and -fno-tree-loop-distribute-patterns for test_garb_host.sh's
# reason: libos64's str.c defines memset, and an optimizer may turn
# os64_memset's loop into a call to it that recurses forever.
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=all -fno-builtin -fno-tree-loop-distribute-patterns -pthread \
   -I userland/apps/yonder -I userland/libway/include -I userland/libfetch/include \
   -I userland/libhtml/include -I userland/libpage/include -I userland/libgzip/include \
   -I userland/libtls/include -I userland/libos64/include -I abi/include -I userland \
   tools/test_yonder_stream_host.c userland/apps/yonder/mail.c userland/libos64/str.c \
   -o "$work/stream_driver"

"$work/stream_driver" "$@"
