# WebP navigation and shutdown stress

2026-10-10, `codex/webp-decoder`. **The P5 completed twelve navigation rounds
with active image jobs, drained image storage after each round, and exited
normally. A separate cache-off P5 close-under-load run exited normally with
zero live heap allocations in its final stable sample.** The user observed
delayed closing; close-click latency is not instrumented. QEMU exposed a
responsiveness problem documented below. Production decoder, Yonder and
kernel code were not changed during this investigation.

## Reproducer

[`webp_stress_pages.py`](../../tools/webp_stress_pages.py) generates a timed
sequence of pages. Each load page asks for six distinct image URLs; after
one second it navigates to a page without images. A fifteen-second pause
allows workers to drain before the next round. The default is twelve rounds.
The WebP fixture is 61,358 bytes and 4096 × 4096 pixels: 64 MiB of decoded
output per image. Its dimensions reach the default pixel cap without
exceeding the encoded-input cap.

[`webpwatch`](../../userland/tests/webpwatch/webpwatch.c) launches the real
Yonder, samples `/proc/<pid>/heap` and CPU accounting every 200 ms, and records
the child's exit status. Its ten-minute watchdog reports failure if it has
to kill the child. No watchdog kill was needed in the recorded runs.
QEMU close requests used Alt+F4 without the second-press force-close
escalation; Chris closed the P5 windows using their close button.

Example, from the repository root:

```sh
make
python3 tools/webp_stress_pages.py --output /tmp/webp-stress
python3 -m http.server 8093 --bind 127.0.0.1 --directory /tmp/webp-stress
```

Boot a disposable GUI QEMU guest with user networking and `virtio-net-pci`.
Set its `/home/yonder.conf` to:

```ini
scripts = on
diagnostics = /home/webp-stress-diag
cache = off
```

Create that diagnostic directory, then run:

```sh
/tests/webpwatch http://10.0.2.2:8093/start.html > /home/webp-nav.txt
```

`close.html` is a separate six-image page without automatic navigation for
close-during-decode checks. On hardware, use a reachable fixture server
address. `file:` works for opening the fixture, but Yonder refuses the
script-requested file navigation, so it cannot exercise this sequence.

Controls can be generated with `--format png` for the same-size PNG, or
`--fixture tools/pages/webp-alpha.png --rounds 4 --idle-ms 1500` for the tiny
PNG navigation/transport control. Generated artifacts belong outside the
shipped `/tests/pages` demo. The recorded PNG controls used PNG bytes under
the original `.webp` fixture names; libimage detects their signatures. The
generator uses matching suffixes for new controls.

## Observations

The VM used q35, eight emulated CPUs, 8 GiB RAM, FAT root, and ext2 `/home`.
These are QEMU measurements, not P5 performance results. Port 55557 isolated
this run after unrelated keyboard input appeared on the shared test monitor.

| Run | Observation |
| --- | --- |
| Large WebP, script audit enabled | The first navigation away arrived in 115 ms; the next took 68,679 ms. Close eventually returned exit 0. Total process lifetime was 319.1 s. |
| Large WebP, script audit disabled | The slowdown and prolonged shutdown recurred. Both load-page records show six pictures still pending when left. Exit 0 after 175.5 s total process lifetime. Stable samples reached 169.31 MiB live heap; a final stable sample reported zero live heap bytes. |
| Tiny PNG control | Four cycles completed, all 24 pictures shown, with no picture failures or retention refusals. Page arrivals were 57–261 ms. Normal close returned exit 0. |
| Large PNG control | Large-image pressure also delayed this run. The second page's trivial navigation timer exceeded the five-second script execution limit. Four images were retained and two exceeded the page's picture-retention budget; neither was a PNG decoding failure. Normal close returned exit 0 after 124.8 s total process lifetime. |

Process lifetime includes baseline pauses, navigation, time left open for
inspection, and shutdown; it is **not** a close-latency measurement. Stable
heap samples peaked at 297.25 MiB in the audit-enabled WebP run and 457.19 MiB
in the large-PNG control. These include the whole browser, unlike the
decoder allocation budget. Torn heap snapshots were excluded from those
figures; they were frequent under load and cannot support allocation-leak
conclusions. All retained stable snapshots had a balanced allocator audit.

The twelve-round WebP run was interrupted for diagnosis rather than called
a pass. The same-size PNG control establishes that this symptom is not
exclusive to the WebP decoder; it does not identify the root cause. The
tiny-image control rules out a generally broken page sequence or fixture
server. A further two-round WebP run without `webpwatch` also reached the
slow second navigation, ruling out the observer as a necessary trigger.
That run also closed normally with exit 0, recorded in
[`webp-direct-exit.txt`](stress/webp-direct-exit.txt).

The [memory-path investigation](memory-profile.md) reproduces the delay
without codecs or Yonder and locates a major cost in populated-region
release and cross-core invalidation. It does not measure WebP's gate wait
or decoder phases separately. The P5 run below supplies twelve-round
completion and repeated image-storage recovery on hardware. Bounded close
latency while decoding remains acceptance work; the QEMU slowdown remains
evidence for the allocator debt.

## P5 navigation result

The P5 watcher exited with code 0 and no timeout after 242.93 seconds.
The page sequence completed `start.html`, twelve load/idle pairs, and
`done.html`: all 26 records have clean verdicts and HTTP 200. Each load page
requested six pictures and was left with four to six still pending; there
were no reported picture failures or retention refusals. This exercises
navigation while picture jobs are outstanding, without establishing that
every pending job had entered the decoder.

All twelve sampled image-memory bursts drained. Live heap peaked at
244,402,320 bytes (233.08 MiB), returned to 7,777,184–7,786,096 bytes
(about 7.42 MiB) at the first low sample after each burst, and ended at
7,486,912 bytes (7.14 MiB) in the final pre-exit sample. The first-low samples
span 8,912 bytes, so this is evidence of repeated large-buffer recovery,
not a claim of zero small leaks. All 1,141 stable heap samples had balanced
allocator audits; 58 torn samples were excluded. Recorded page arrivals
ranged from 23 to 695 ms. Whole-process lifetime includes pauses and the
user's final close; it does not measure close latency.

The fixtures were served by Python on Windows after WSL reachability
problems. The retrieved Yonder configuration had scripts on, cache on
(`cache_mb = 2048`), and the usual diagnostics directory, rather than the
cache-off configuration in the reproducer. This result is recorded with
caching enabled; the twelve memory bursts and pending-job page records
still establish repeated image work. Earlier connection/404 setup attempts
were excluded from the successful sequence.

Evidence: [watcher report](stress/p5-navigation/watch.txt.gz),
[validated summary](stress/p5-navigation/summary.json), and
`stress/p5-navigation/page-00.txt` through `page-25.txt`.
The fixture server address in saved page records and the summary is replaced
with `fixture.test:8093`; the observer report is unchanged. Reproduce analysis:

```sh
python3 tools/analyze_webp_stress.py \
    docs/webp-evidence/stress/p5-navigation/watch.txt.gz \
    docs/webp-evidence/stress/p5-navigation --rounds 12 \
    --address-prefix http://fixture.test:8093/load-
```

Closing after `done.html` establishes normal idle shutdown. The separate
`close.html` run below exercises closing with image jobs outstanding.

## P5 close-under-load result

Chris disabled caching, opened `close.html` through `webpwatch`, and clicked
the close button when images began loading. He reported that the window
took longer than usual to close but finished. The retrieved configuration
confirmed `cache = off` and scripts enabled. The latest close-page record
(`0049`) has a clean verdict, two pictures shown, four still pending, and
no picture failures or retention refusals. An earlier close-page record
(`0048`) had all six jobs settled and is not used as evidence for this run.

The watcher recorded normal exit code 0 with no timeout at 7,274,072 µs
after process launch. Its final stable heap sample at 7,071,945 µs had zero
live bytes and zero live blocks, with one empty 1 MiB pool still mapped.
Peak sampled live heap was 310,680,416 bytes (296.29 MiB). All 30 stable
samples had balanced allocator audits; five torn samples were excluded.
These results establish normal shutdown under outstanding image work and
release of tracked heap allocations in this run. The global physical-used
counter increased by 37,142,064 bytes across the run; it includes other
system activity and is not a per-process leak measurement.

The observer does not timestamp the close click, and the page record has
no process ID. Attribution uses the latest close-page record and Chris's
reported sequence. The 7.27 seconds is whole-process lifetime, not measured
close latency. Yonder joins its workers before destroying its window, and
the synchronous WebP call (including waiting at its serialization gate)
does not poll cancellation. The observed delay is consistent with draining
those calls and releasing their buffers; this report does not isolate the
time spent decoding, waiting, or cleaning up. Prompt cancellation and a
measured close-latency bound remain unproven.

Evidence: [watcher report](stress/p5-close/watch.txt.gz),
[observer summary](stress/p5-close/summary.json), and
[close-page record](stress/p5-close/page.txt). The saved page record replaces
the fixture server address with `fixture.test:8093`; the watcher is unchanged.
The generic analyzer's `--rounds 0` mode validates the observer's exit and
heap samples; the close-page pending count and final zero-live sample were
checked separately:

```sh
python3 tools/analyze_webp_stress.py \
    docs/webp-evidence/stress/p5-close/watch.txt.gz \
    docs/webp-evidence/stress/p5-close --rounds 0
```

## Evidence

Raw observer logs are gzip-compressed in [`stress/`](stress/):
`webp-audit.txt.gz`, `webp.txt.gz`, `small-png.txt.gz`, and `large-png.txt.gz`.
[`samples.json`](stress/samples.json) summarizes stable heap samples and
process exits; its allocation bursts are not a count of navigation rounds.
Fixture manifests preserve dimensions, compressed sizes and SHA-256 values.
The directory also retains page diagnostics and screenshots.

[`analyze_webp_stress.py`](../../tools/analyze_webp_stress.py) reads plain or
gzipped observer logs and page records. It rejects a nonzero/forced exit,
unbalanced stable heap reports, missing rounds, picture failures, and a
run that did not leave while pictures were pending. For example:

```sh
python3 tools/analyze_webp_stress.py /tmp/webp-nav.txt /tmp/page-records \
    --rounds 12 --address-prefix http://10.0.2.2:8093/load-
```

The recorded QEMU WebP runs fail the twelve-round check; the P5 navigation
run passes it. The
full build passed; generator references and the analyzer's handling of
the captured samples were checked. No new sanitizer claim is made for
these guest stress runs; the wrapper's earlier ASan/UBSan/LeakSanitizer
results are recorded in [integration.md](integration.md).
