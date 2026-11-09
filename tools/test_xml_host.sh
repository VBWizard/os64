#!/bin/bash
set -eu
cd "$(git rev-parse --show-toplevel)"
if [ "$#" -gt 1 ] || { [ "$#" -eq 1 ] && [ "$1" != --local-only ]; }; then
    echo 'usage: tools/test_xml_host.sh [--local-only]' >&2
    exit 2
fi
work=${XML_TEST_WORK:-$(mktemp -d)}
mkdir -p "$work"
trap 'status=$?; if [ "$status" -eq 0 ] && [ -z "${XML_TEST_WORK:-}" ]; then rm -rf "$work"; else echo "XML test artifacts: $work" >&2; fi' EXIT
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1}"
export CCACHE_DISABLE=1
. tools/host_build.sh
host_flags=(-std=c11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -Iuserland/libxml/include)
host_sources=(userland/libxml/xml.c tools/test_xml_driver.c)
host_objects "$work"
cc "${host_flags[@]}" "${host_objs[@]}" -o "$work/xml_driver"
timeout 60s "$work/xml_driver" --checks
python3 tools/test_xml_differential.py --driver "$work/xml_driver"
if [ "${1:-}" != --local-only ]; then
    python3 tools/xmlconf/run.py --driver "$work/xml_driver" --report "$work/xmlconf.json"
else
    echo 'Explicit local-only run: xmlconf gate omitted' >&2
fi
