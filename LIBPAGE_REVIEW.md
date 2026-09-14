# libpage independent repair and validation — 2026-09-14

Branch: `codex/libpage-review`, based on `d40373b` (`codex/html-form-owner`),
retaining the `opus/libpage` and `opus/wend` stack. The repair is retained locally; no push, PR-thread replies or new model
review has been requested. The numeric repair proceeded after Chris authorized
resolving the design boundary and finding defects independently.

## Contracts repaired

Normalized defaults, current values, option display labels and submission
values have distinct roles. Construction and edits share sanitization;
reset restores retained defaults without allocation. Radio groups use name
and owner, reserve edit records before publishing changes, and preserve
nameless controls independently. Select construction and required validation
share HTML display-size parsing. Failed edits preserve visible state;
reserved records alone do not make a value dirty. Incomplete models refuse
form submissions and mutation while permitting inspection.

Textareas retain LF in current values; hard wrapping derives submission
text using fixed Unicode-scalar columns, followed by CRLF normalization.
Constraint lengths count UTF-16 units for the applicable input types and
textareas. Option labels do not replace fallback submission text. A file
input holds no selected filename through markup or the text setter.
Checkbox/radio defaults supply `on` only when the value attribute is absent;
submission reads the current value, including later edits.

URL preprocessing trims surrounding C0/space and removes ASCII tab/newline
bytes before resolution. Empty and fragment-only links resolve against the
base URL; a literally empty form action uses the document URL. GET entry
lists use URL encoding across HTTP, data and mail paths. Mail POST text/plain
uses UTF-8 and path-set percent escaping. Request allocation failures refuse
instead of dropping fragments, directionality or entry bytes. Image-button
suffixes preserve long names, and a body may exactly equal its configured
payload limit.

Wend marks refresh handling on the loaded view before following or declining
it. Same-document fragment refreshes scroll; immediate self-reloads are
suppressed. Failed, declined, unsupported and over-limit hops do not restart
when the event loop receives another key.

## Numeric design

Numeric conversion remains private to libpage. The established musl decimal
scanner replaces the restricted integer mantissa helper; Ryū supplies shortest
binary64 formatting. The adapters require IEEE binary64 and the x86-64
extended-precision ABI, checked at compilation. They introduce no kernel or
public libc API. Pinned originals, licenses, hashes and adaptation notes live
in [upstream/README.md](userland/libpage/upstream/README.md). Numeric license
notices are included in both image-install recipes.

Values use strict HTML floating-point syntax; numeric attributes use the
HTML prefix-parsing rules. Overflow/nonfinite input is rejected; finite
underflow may yield zero. Long mantissas and exponent strings do not require
proportional scratch allocation. Number controls retain valid lexical input.
Changed range results use shortest round-trip spelling and HTML's fixed vs
exponential thresholds.

Range normalization converts finite inputs to their shortest decimal
representatives, then computes bounds, midpoint, step-base differences and
remainders exactly on that decimal grid. Eighty base-1e9 limbs cover the
finite binary64 exponent span and intermediate arithmetic. The default step
is one; ties choose the higher in-range candidate. Reversed bounds use the
minimum, and `step=any` skips the grid. Construction and edits call this same
algorithm. ISO week-year calculation and datetime-local normalization are
also corrected; date years are checked modulo 400 without integer overflow.

### Independent numeric evidence and compatibility limits

`tools/libpage_range_reference.json` retains 186 observations from Firefox
155.0.1, collected through its WebDriver BiDi interface, with the source
attributes and generator seed. `tools/gen_libpage_range.py` derives expected
numeric values independently using Python Decimal at 800-digit precision.
It does not call libpage or reproduce its limb implementation. The generated
C fixture is checked for freshness before the host suite runs.

Eight extreme cases differ numerically from Firefox. They are retained,
not filtered from the data. For example, `min=1e-300 max=0.9 step=3 value=0.3`
produces the minimum on the exact decimal grid; Firefox retained `0.3` in
this observation. With `min=-10` and a default step, `value=1e-300` rounds to
zero here while Firefox retained the tiny value. The implementation follows
the explicit decimal-grid contract above; these checks do not establish
complete browser equivalence. Representation differences with the same
binary64 value are not counted as numeric divergences.

The [HTML Range-state algorithm](https://html.spec.whatwg.org/multipage/input.html#range-state-(type=range))
is the behavioral basis. Full URL Standard conformance and constraint
validation beyond the booked required/length subset remain outside this
repair. No POST wire support, file picker or mutable DOM is added.

## PR #100 disposition

The 19 actionable findings have implementation and regression coverage.
This table records local disposition; it does not claim GitHub threads were
resolved or that a subsequent independent review has passed.

| Thread | Correction / evidence |
|---|---|
| [2192](https://github.com/VBWizard/os64/pull/100#discussion_r4007822192) | Per-view refresh handling; guest failure and cycle checks |
| [2197](https://github.com/VBWizard/os64/pull/100#discussion_r4007822197) | Shared construction/edit sanitization; state and numeric tests |
| [2201](https://github.com/VBWizard/os64/pull/100#discussion_r4007822201) | Initial single-select normalization, disabled groups and size parsing |
| [2208](https://github.com/VBWizard/os64/pull/100#discussion_r4007822208) | UTF-16 constraint lengths, including astral text |
| [2214](https://github.com/VBWizard/os64/pull/100#discussion_r4007822214) | Finite numeric conversion without the artificial digit limit or signed overflow |
| [2225](https://github.com/VBWizard/os64/pull/100#discussion_r4007822225) | Same-document fragment refresh; guest scroll target inspected |
| [2230](https://github.com/VBWizard/os64/pull/100#discussion_r4007822230) | Separate option label and fallback value |
| [2236](https://github.com/VBWizard/os64/pull/100#discussion_r4007822236) | File markup and text edits cannot supply a filename |
| [2245](https://github.com/VBWizard/os64/pull/100#discussion_r4007822245) | Range bounds, midpoint and step grid; independent decimal oracle |
| [2259](https://github.com/VBWizard/os64/pull/100#discussion_r4007822259) | GET URL encoding selected before serialization, including mail/data |
| [2268](https://github.com/VBWizard/os64/pull/100#discussion_r4007822268) | Exact payload ceiling excludes the private terminator |
| [2279](https://github.com/VBWizard/os64/pull/100#discussion_r4007822279) | Initial radio group settlement shared with retained reset defaults |
| [2288](https://github.com/VBWizard/os64/pull/100#discussion_r4007822288) | Correct ISO week-year weekday calculation |
| [2294](https://github.com/VBWizard/os64/pull/100#discussion_r4007822294) | Shared URL-input preprocessing |
| [2300](https://github.com/VBWizard/os64/pull/100#discussion_r4007822300) | Option allocation failure marks incomplete; isolated-failure checks |
| [2305](https://github.com/VBWizard/os64/pull/100#discussion_r4007822305) | Allocation-free reset, including a 6000-byte default |
| [2312](https://github.com/VBWizard/os64/pull/100#discussion_r4007822312) | Fragment-copy failure refuses and preserves request ownership |
| [2320](https://github.com/VBWizard/os64/pull/100#discussion_r4007822320) | Submission-only hard wrapping without splitting UTF-8 |
| [2328](https://github.com/VBWizard/os64/pull/100#discussion_r4007822328) | Dynamic image-button names with complete `.x`/`.y` suffixes |

[Thread 2217](https://github.com/VBWizard/os64/pull/100#discussion_r4007822217)
remains rejected: the [WHATWG Windows-1252 index](https://encoding.spec.whatwg.org/index-windows-1252.txt)
explicitly maps the disputed C1 values. Removing those mappings would break
encoding. Additional independent repairs include checkbox/radio edited
values, base-relative fragments, mail text/plain escaping, datetime-local
normalization, long year handling, allocator arithmetic guards and dirty
state preservation after failed edits. The previous range midpoint and
2026-W53 tests asserted incorrect expectations; both are corrected.

## Validation

- `ASAN_OPTIONS=detect_leaks=0 bash tools/test_libpage_host.sh`: **178,130
  assertions, zero failures**; persistent failure sweep over 315 allocation
  positions, plus isolated build/edit/activation failures with semantic
  state checks. The allocator reports no retained blocks. LeakSanitizer
  cannot operate under this environment's tracing; ASan and UBSan remain on.
- Numeric coverage includes 30,000 generated binary64 patterns, libc parsing
  comparisons, 25,000-digit inputs and the 186-case decimal range oracle.
- `ASAN_OPTIONS=detect_leaks=0 bash tools/test_wend_host.sh`: **269,921 checks,
  zero failures**, zero live blocks, plus retained real-page render fixtures.
- `make -C userland`: complete strict cross-build passed with `-Werror`.
- `git diff --check` and `tools/stale_refs.sh`: passed. Pinned upstream
  numeric sources retain their original bytes through `.gitattributes`.
- Numeric-license installation and byte-for-byte readback passed on both
  ext2 and FAT. The post-shutdown guest ext2 filesystem check passed.
- QEMU, two CPUs under TCG: new `/tests/testrun pagetest` and existing
  `/tests/testrun htmltest` passed. Pagetest checks numeric conversion across
  yields, range results, radio/select state, request bytes, reset and heap
  integrity. This run uses the unchanged parent kernel with newly built
  libpage, libhtml, wend, pagetest and testrun installed in an isolated disk.
- Guest refresh observations: `/fragment` scrolled to the target at the
  bottom of the page; a connection closed before the HTTP status line left
  `/fail-refresh` displayed, with one attempted hop; `/loop-a` and `/loop-b`
  stopped after five hops. Up/Down/Home/End did not add server requests after
  the failed hop or capped cycle. Declined TLS-downgrade prompts were not
  separately exercised in this guest run.

Local execution artifacts: `/tmp/libpage-final-host.log`,
`/tmp/libpage-wend-host.log`, `/tmp/libpage-full-userland-build.log`, and
`/tmp/libpage-guest/` (serial log, HTTP log and inspected screenshots).
The printed host step ratios are an inventory, not a conformance percentage.

### Repeat the refresh probes

Run `python3 tools/test_libpage_refresh_server.py --log /tmp/libpage-refresh.log`
on the QEMU host. In wend on a guest with user networking, open
`http://10.0.2.2:8769/fragment`, `/fail-refresh`, then `/loop-a`. Inspect the
fragment target, the retained source page after the connection failure, and
the capped-cycle status respectively. After each settles, compare the access
log before and after Up/Down/Home/End. No extra requests should appear. The
cycle permits the initial request plus five refresh hops. Explicit reload
or a new navigation is a fresh loaded document and can handle refresh again.
