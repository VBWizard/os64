#!/bin/bash
# Drive the console's PSF2 loader on the host, under ASan and UBSan.
#
# The loader reads an image a ring-3 program wrote, so its tests are mostly
# images no honest tool produces: every prefix of a good font, damaged
# headers, malformed UTF-8 in the Unicode table. Each lives in a heap block
# of exactly its own size, which turns "read one byte too far" into a report.
#
# Then, if the host has real console fonts installed, every PSF2 among them
# must load and map all of printable ASCII — the fixtures prove the rules,
# the shelf proves the rules match what font authors actually ship.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined \
   -I kernel/include -I abi/include \
   kernel/src/psf2.c tools/test_psf2_host.c -o "$work/test_psf2"

"$work/test_psf2"

shelf=/usr/share/consolefonts
if [ -d "$shelf" ]; then
    loaded=0 psf1=0
    for font in "$shelf"/*.psf.gz "$shelf"/*.psfu.gz; do
        [ -e "$font" ] || continue
        gunzip -c "$font" > "$work/font.psf"
        rc=0
        "$work/test_psf2" "$work/font.psf" > "$work/font.out" || rc=$?
        case $rc in
            0) loaded=$((loaded + 1)) ;;
            3) psf1=$((psf1 + 1)) ;;
            *) echo "FAIL $(basename "$font"): $(cat "$work/font.out")"; exit 1 ;;
        esac
    done
    echo "test_psf2_host: $loaded PSF2 fonts from $shelf loaded ($psf1 PSF1 skipped)"
else
    echo "test_psf2_host: no $shelf on this host; shelf check skipped"
fi

echo "test_psf2_host: all checks passed"
