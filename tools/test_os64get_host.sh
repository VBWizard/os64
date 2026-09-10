#!/bin/bash
# Compile the production app with host I/O adapters and exercise batch failures.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
   -fsanitize=address,undefined -fno-pie -no-pie \
   -I userland/libtls/include -I userland/libos64/include -I abi/include -I userland/libgzip/include \
   tools/test_os64get_host.c userland/apps/os64get/install.c userland/apps/os64get/http.c \
   userland/libos64/{str,fmt,args,date,crc32,url}.c userland/libgzip/{gzip,inflate,deflate}.c \
   -Wl,--gc-sections,--wrap=os64_time -o "$work/os64get-test"
scenarios=(success absent unchanged force-identical no-archive single url url-https url-archive-blocked url-short url-cancel url-tls-good url-tls-close url-tls-framed-cut url-tls-alert url-tls-cut url-tls-roots url-tls-cert url-tls-ip url-tls-name url-tls-downgrade url-tls-redirect url-tls-bypass url-upgrade url-tls-other url-tls-head-alert short crc \
                backup-read backup-write backup-corrupt sync close publish aliases appeared unsafe-name archive-overlap \
                cancel-list cancel-download cancel-backup cancel-verify cancel-commit cancel-cleanup cancel-transition
                review-empty-backup review-partial-backup review-ro review-full review-all-full
                review-empty-ro review-legacy-list review-force-ro review-single-full review-duplicate-unchanged review-duplicate-new
                integrity-existing integrity-absent integrity-append integrity-read integrity-url integrity-gzip integrity-gzip-ok
                integrity-no-archive integrity-cancel
                enclose-single enclose-batch enclose-fat-alias enclose-sibling enclose-fat-scratch)
if (( $# )); then scenarios=("$@"); fi
for scenario in "${scenarios[@]}"; do
    mkdir "$work/$scenario"
    if [[ "$scenario" == cancel-transition ]]; then
        # Stop at the commit-state assignment and perform the signal handler's
        # flag store there. This deterministically exercises the transition gap
        # without adding a scheduling hook to production code. Requires GDB.
        transition_line=$(python3 - <<'PYLINE'
from pathlib import Path
lines = Path("userland/apps/os64get/install.c").read_text().splitlines()
spots = [i + 1 for i, line in enumerate(lines) if line.strip() == "committing = true;"]
assert len(spots) == 1, "locate the commit transition before injecting cancellation"
print(spots[0])
PYLINE
        )
        if ! ASAN_OPTIONS=detect_leaks=0 gdb -nx -q -batch \
            -ex "break userland/apps/os64get/install.c:$transition_line" \
            -ex run -ex 'set variable cancelled = 1' -ex continue \
            --args "$work/os64get-test" "$scenario" "$work/$scenario" > "$work/transition.log" 2>&1; then
            cat "$work/transition.log" >&2
            exit 1
        fi
        cat "$work/transition.log"
        rg -q '^PASS cancel-transition$' "$work/transition.log"
    else
        if ! ASAN_OPTIONS=detect_leaks=0 "$work/os64get-test" "$scenario" "$work/$scenario" 2> "$work/stderr"; then
            cat "$work/stderr" >&2
            exit 1
        fi
        cat "$work/stderr" >&2
        if [[ "$scenario" == url-tls-ip || "$scenario" == url-tls-name ]]; then
            target=10.0.2.2
            [[ "$scenario" == url-tls-ip ]] || target=bad_name
            rg -F "check HTTPS target '$target': TLS requires a supported DNS name; IP literals are not supported" "$work/stderr"
        fi
    fi
done
