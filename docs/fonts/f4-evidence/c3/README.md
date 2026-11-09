# F4 checkpoint 3 evidence — long lines through a window

Every screenshot is a QEMU screendump (`/QEMU GUI Boot`, ext2 root, booted
from scratch copies of the images, headless, monitor on 55556), cropped to
the Scribe window. The file is written in the guest with
`scribefonttest --long-demo /home/long.txt`, four lines:

1. `head`
2. `café words ` repeated to 2.5 MB (2,621,436 bytes) — a line of whole words
   with a two-byte letter in each
3. `tail`
4. `abc e` + 5,000 combining acutes + ` xyz` — one cluster of 10,001 bytes,
   longer than the default 8 KiB window, between ordinary letters

`scribefonttest /home/long.txt`, then **Alt+2** (DejaVu Sans 16), unless the
row says production Scribe.

| File | Keys | Shows |
|---|---|---|
| `default-window-open.png` | — | The default window. Line 2 is cut at the paper's edge (its window is 8 KiB, far wider than the view). Line 4 is `abc`, the placeholder for the long cluster, and a "more" arrow: the line goes on past the cluster. |
| `default-window-end.png` | Down, End | The caret at the end of the 2.5 MB line. The row lays out the last 8 KiB, and the view has scrolled to the caret. |
| `default-window-selection.png` | Shift+Home | Selected from the end back to the start. The window moved back to the line's start with the caret; the selection is lit from there. |
| `window64-both-ends.png` | Alt+w, Down, End, Left ×70 | A 64-byte window in the middle of the line: "more" at both ends, the caret inside. |
| `window64-selection-lights-trail.png` | then Shift+Down | The selection now runs from inside line 2 into `tail`. Line 2 is no longer the caret's, so it shows its start. Every selected byte of it lies beyond that window, so only the trailing decoration is lit. |
| `cluster-before.png` | Alt+w, Down ×3, Home, Right ×4 | The caret before the long cluster: `abc`, then the placeholder, then "more". |
| `cluster-after.png` | then Right | One step crosses the whole cluster. The window starts after it: "more", the placeholder, then ` xyz`. |
| `cluster-selected.png` | Left, Shift+Right | The cluster selected: the leading decoration, which stands for it, is lit. |
| `cluster-deleted.png` | Delete | All 10,001 bytes deleted in one key; the title shows the unsaved star. |
| `production-scribe-open.png` | `scribe /home/long.txt` | /bin/scribe with no fixture, on the builtin face: the same rows, the same decorations. |
| `production-scribe-end.png` | Down, End | The end of the 2.5 MB line in production Scribe. |
| `production-scribe-cluster.png` | Down ×2, Home, Right ×4 | The caret before the long cluster in production Scribe. |
| `selftest-pass-46.png` | `scribefonttest --selftest` | `PASS, 46 checks, 0 failed`: C2's 41, plus the 2.5 MB round trip through the real disk (write it, open it with Scribe's Open, type deep inside, split there, add at the end, Save As, and compare every byte). |
| `log-timing-qemu-before.png` | `scribefonttest --log-demo /home/big.log`, then `time scribefonttest --time-open /home/big.log` | Before the lazy extent. 100,000 log lines (8.9 MB): Open under the builtin face 34.0 s; changing to DejaVu Sans 16 31.9 s; Open under it 37.5 s. |
| `log-timing-qemu.png` | the same | After it, on the final build: Open 2.3 s; the change 0.35 s; Open under DejaVu Sans 5.9 s, which also frees the first copy's 100,000 lines. The self-test above it still passes. |
| `production-scribe-log.png` | `scribe /home/big.log` | /bin/scribe with the same log open. |

`log-bench/` is the same measurement on the host, at `-O2` with no
sanitizer: `bash docs/fonts/f4-evidence/c3/log-bench/run.sh` from the
repository root. `native-o2.txt` is its output on this machine:

```
8912542 bytes, 100000 lines: reading them 30.9 ms
Open, builtin face: 26.0 ms
change to DejaVu Sans 16 with it open: 2.1 ms (adopted)
Open under DejaVu Sans 16: 30.9 ms
```

`before-lazy-extent.txt` is the earlier version of the bench, which timed
the whole-document measurement Scribe then made at every Open and font
change. Reading the bytes was 31 ms; measuring every line was 781 ms under
the builtin face and 1,274 ms under DejaVu Sans. The builtin face is an F2
face too, so each line was a full layout either way:

```
8912542 bytes, 100000 lines: load 31.3 ms; extent under the builtin face 781.4 ms (960 px)
adopting DejaVu Sans 16 1279.6 ms; extent under it 1273.5 ms (1012 px)
```
