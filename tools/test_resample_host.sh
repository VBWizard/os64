#!/bin/bash
# Run libsound's resampler on the host, under ASan and UBSan: speexdsp's
# resample.c as vendored, os64's wrapper, and the checks /tests/resampletest
# runs in ring 3 (userland/tests/resampletest/checks.h says what they are).
# The host's allocator and libm stand in for libos64's and libmath's.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# os64/mem.h, for the wrapper: the two names it uses, as the host's.
mkdir -p "$work/os64"
printf '#include <stdlib.h>\n#define os64_malloc malloc\n#define os64_free free\n' > "$work/os64/mem.h"

S=userland/libsound
cc -std=c11 -g -O1 -Wall -Wextra -Werror \
   -fsanitize=address,undefined -fno-sanitize-recover=undefined \
   -DOUTSIDE_SPEEX -DRANDOM_PREFIX=os64_speex -DFLOATING_POINT -DEXPORT= \
   -I "$work" -I $S/upstream/include/speex -I $S/include \
   -c $S/port/resample.c -o "$work/port.o"
cc -std=c11 -g -O1 -Wall -Wextra -Werror \
   -fsanitize=address,undefined -fno-sanitize-recover=undefined \
   -I $S/include -I userland/tests/resampletest \
   -c tools/test_resample_host.c -o "$work/test.o"
# The upstream file alone is spared one warning, as in libsound/shared.mk.
cc -std=c11 -g -O1 -Wall -Wextra -Werror -Wno-sign-compare \
   -fsanitize=address,undefined -fno-sanitize-recover=undefined \
   -DOUTSIDE_SPEEX -DRANDOM_PREFIX=os64_speex -DFLOATING_POINT -DEXPORT= \
   -I $S/upstream/include/speex -I $S/upstream/libspeexdsp \
   -c $S/upstream/libspeexdsp/resample.c -o "$work/upstream.o"
cc -fsanitize=address,undefined "$work/port.o" "$work/test.o" "$work/upstream.o" -lm -o "$work/test_resample"

"$work/test_resample"
