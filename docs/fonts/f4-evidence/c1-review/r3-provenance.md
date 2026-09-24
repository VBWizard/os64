# Evidence provenance after the third review

Reviewed production commit: `0cfb485`. Reviewer: Quinn, 2026-09-17.

- `r2-observations.txt` was restored byte-for-byte from the still-existing
  original capture at `/tmp/quinn-f4-c1-r2-durable/r2-observations.txt`.
  Its body also matched the reconstructed body checked in by Opus; only the
  reconstruction preamble differed. This is recovery of the original capture,
  not a fresh historical rerun and not a reconstruction from the review text.
- `baseline-host.txt` matches its tracked contents at `5357245`; Opus's git
  restoration succeeded. That is the first review's 85-check result.
- `r2-baseline-host.txt` and `r2-original-probes.txt`, linked in the previous
  review but absent from `0cfb485`, were restored from the surviving original
  `/tmp/quinn-f4-c1-r2` captures. The former is the 197-check result; the latter
  combines the six original logs in the same order/format as the previous handoff.
- `r3-baseline-host.txt`, `r3-prior-probes.txt`, and `r3-observations.txt` are
  fresh captures from this review, not replacements for old evidence.
- As submitted, `r2-run.py` ran the baseline and original six probes, then its
  follow-up C probe failed to compile: both the included host suite and the
  probe defined `static list_label`. The reviewer renamed the probe's local
  callback and its call site to `r2_list_label`, then rebuilt/reran it against
  the same production sources and freshly compiled backend objects. No
  production source or supplied host-suite implementation was changed.

SHA-256 of restored original `r2-observations.txt`:

```
00101efe7405f8209add83f45d3456198a27a11f8b7841f4bd8f4af46bc250b9
```
