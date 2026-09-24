# Quinn C3 correction review receipts

Fresh independent runs against the working tree identified by
[source-sha256.txt](source-sha256.txt), over `8f09ae8`, 2026-09-19.
The verdict is in [F4-QUINN-REVIEW.md](../../F4-QUINN-REVIEW.md).

- `original-probes.txt`: unchanged first-round consumer probe; both original
  defects corrected, including refusal of every post-barrier allocation.
- `repro.c`, `run.py`, `observations.txt`: combined reduced-window / Help /
  font-change probe. Also checks first view paint, refused adoption, active
  run identities, unchanged document bytes and visible caret. 30 checks pass.
- `widget-host.txt`, `scribe-host.txt`: fresh O2 real-backend suites under
  ASan+UBSan, with LSan disabled. 1,376 and 2,470 checks pass.
- `c2-recovery.txt`: prior transient-binding/paint-choice regression probe.
- `cross-build.txt`: forced strict compilation of `ui_text.c` and `scribe.c`,
  relinking libos64, Scribe and the real-Scribe fixture.

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real --output /tmp/f4-current
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_scribe_host.py --output /tmp/f4-scribe
python3 docs/fonts/f4-evidence/c3-review/run.py --baseline /tmp/f4-current --output /tmp/f4-original
python3 docs/fonts/f4-evidence/c3-review-r2/run.py --baseline /tmp/f4-current --output /tmp/f4-combined
python3 docs/fonts/f4-evidence/c2-review-r3/run.py --baseline /tmp/f4-current --output /tmp/f4-recovery
```

The runner compiles review probes at O1 against fresh O2 backend objects.
Original first-round evidence and Opus's correction captures are untouched.
No new independent QEMU, O0 or hardware run is claimed here; the guest images
in `../c3-fixes/` remain Opus's evidence.
