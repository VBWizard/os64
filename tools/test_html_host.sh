#!/bin/bash
set -eu
cd "$(git rev-parse --show-toplevel)"
export PYTHONDONTWRITEBYTECODE=1
work=$(mktemp -d)
cleanup() {
    status=$?
    if [ "$status" -eq 0 ]; then rm -rf "$work"; else echo "HTML test artifacts: $work" >&2; fi
}
trap cleanup EXIT
python3 tools/gen_html_tables.py --check
cc -std=c11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Iuserland/libhtml/include -Iuserland/libos64/include -Iabi/include \
  userland/libhtml/core.c userland/libhtml/encoding.c userland/libhtml/tokenizer.c userland/libhtml/tree.c \
  tools/test_html_driver.c -o "$work/html_driver"
python3 -u tools/test_html_host.py --driver "$work/html_driver"
timeout 20s "$work/html_driver" --stress
python3 tools/test_html_corpus.py --driver "$work/html_driver"
python3 tools/test_html_fuzz.py --driver "$work/html_driver" --seconds "${HTML_FUZZ_SECONDS:-30}"
