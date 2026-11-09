# Web-family implementation evidence — 2026-09-27

Branch base: `3f56e3c1` (`origin/userland` when the worktree was created).
The packet implements the provider/configuration/font assets; Yonder's
consumer switch belongs to its separate worktree.

## Automated checks

- `make -j8`: strict kernel/userland build and both disk images pass.
- `python3 tools/test_font_family_host.py`: fake-backend cache tests under
  ASan/UBSan and leak detection; all twelve faces at eight sizes pass through
  the 32-entry LRU. Includes exact LRU identity checks, source snapshot
  independence, byte-cap eviction, configured fallbacks, allocation denial,
  retained runs surviving eviction/destruction, and 64 retained faces causing
  a clean LIMIT. Final allocator live bytes are zero.
- `python3 tools/test_font_family_host.py --real`: the same lifetime/count
  cases with the pinned DejaVu files and production FreeType; maximum tracked
  allocator usage is 37,843,815 bytes under the 128 MiB context cap.
- The fake suite launches an intentional expired-list read in a subprocess.
  [ASan's diagnostic](expired-list-asan.txt) is the expected success criterion;
  the main suite does not dereference invalid storage.
- `python3 tools/test_font_config_host.py`: 28,016,836 checks, zero failures
  and zero live allocations. Includes the twelve default faces, style
  inheritance, paths/line numbers, refusal of an unused unreadable style,
  round-trip encoding, and existing role/discovery allocation-denial sweeps.
- `python3 tools/test_font_config_host.py --settings`: native-file Save/Apply,
  installation and refusal tests pass. Family lines and their inline comments
  survive role Save verbatim; they are absent from the role session envelope.
- `python3 tools/test_font_provider_host.py`: 1,368,124 existing provider and
  adoption checks pass, including 672 allocation-denial positions.
- All twelve `/etc/fonts` files were extracted from **both** FAT and ext2
  build images and compared byte-for-byte with the pinned fixtures.
- `git diff --check` and `tools/stale_refs.sh` were reviewed before committing.

## QEMU

Eight cores, q35, qemu64 with RDRAND/RDSEED, 8 GiB RAM; isolated copies of the
built images. The text boot's `/tests/webfacestest` passes. The graphical
boot uses the same ext2 root plus `GUI BACKSTOP=10`; its [probe output](guest.txt)
passes. The GUI harness waits for the shell, rather than the text-boot-only
`boot complete` serial marker.

The [specimen](specimen.png) was read visually: Serif, Sans and Mono each
render regular, bold, italic/oblique and bold-italic/bold-oblique at 12, 18
and 26px. The footer uses nested style transitions. [Scribe](scribe.png)
opens `/etc/fonts.conf` with its existing builtin document role;
[gterm](gterm.png) launches husk with its existing builtin terminal role.
These are visual smoke checks, not pixel-difference tests against a baseline.

## Spawn timing

`/tests/textspawn` runs six batches of 50 spawn/exit/reap cycles of `/bin/true`.
The first is warmup. The baseline uses libos64 built from `3f56e3c1`, installed
into a private copy of the same image; the after run uses the new library.
Kernel, machine configuration and test executable are the same.

| Warm batches | Before, ticks | After, ticks |
|---|---:|---:|
| 1 | 57 | 53 |
| 2 | 166 | 151 |
| 3 | 48 | 46 |
| 4 | 70 | 167 |
| 5 | 221 | 36 |
| Total (250 spawns) | 562 | 453 |

Ticks are 100 Hz: 5.62 seconds before, 4.53 after. There is no observed
regression, but these shared-host QEMU batches are noisy and do not establish
a speedup. Family resolution and font-file reads require an explicit family
cache preparation; neither was added to process startup. Raw outputs:
[before](spawn-before.txt), [after](spawn-after.txt).

## Boundaries

The cache adds one snapshot copy of each distinct source to the context's
budget (about 5.1 MiB for defaults), in addition to the engine's per-open
copies. Thirty-two entries do not necessarily mean thirty-two engine faces:
configured outline fallbacks and retained runs consume faces too. LIMIT is
returned when eviction cannot free enough resources; the text engine's
64-face cap remains in force. No kernel or text-engine changes are included.

`os64_font_config_t` now includes family settings. Deploy the rebuilt shared
library together with rebuilt consumers; the full image build does this.
