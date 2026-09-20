# System and personal font discovery

Follow-up to `f9ca6ef`, authorized by Chris after his P5 font list lost
unselected system fonts on reboot. The configuration ladder still selects
settings; it no longer selects the sole font directory to scan.

Discovery visits configured files, `/home/fonts`, `/etc/fonts`, a custom
installation target and the active config's adjacent `fonts/` directory.
Repeated directory paths are skipped. A missing optional directory does not
hide another directory's results. Directory-entry and source-read limits are
shared across the operation: 256 entries, 128 catalog rows, 64 MiB read budget,
32 MiB maximum per file. Reaching those limits may truncate the collection.

Valid files with identical bytes share one row. Configured paths win over
scanned paths; personal copies win over identical system copies when neither
is configured. Duplicate paths selected by different roles map to the shared
row through catalog aliases without changing the role's configured path.
Matching family/style names do not establish equality: different contents stay
separate. The provider's font-instance identity and adoption APIs are unchanged.

Exact comparison retains validated source buffers until discovery returns,
within the 64 MiB read budget. This is additional temporary source memory, not
a new persistent glyph cache. The catalog exposes `os64_font_catalog_find` for
representative/configured-alias lookup. Updating Workshop therefore requires
the matching libos64; refresh compatible userland and reboot before use.

## Host and build receipts

- `ASAN_OPTIONS=detect_leaks=0 python3 tools/test_font_config_host.py --output /tmp/quinn-font-union-host`
- `ASAN_OPTIONS=detect_leaks=0 python3 tools/test_font_config_host.py --backend-objects /tmp/quinn-font-union-host --output /tmp/quinn-font-union-final`
- `ASAN_OPTIONS=detect_leaks=0 bash tools/test_appearance_host.sh`
- `make` (strict userland flags include `-Wall -Wextra -Werror`; image/ISO assembly included).

The final config suite reports 28,012,099 raw assertions, zero failures,
3,713 discovery allocation-denial positions and 1,141 preparation denial cases;
the allocation ledger returns to zero after context/catalog cleanup. Tests cover
personal vs system config precedence, renamed duplicate copies selected by two
roles, an unselected system font, different bytes with the same family metadata,
missing folders, the global entry limit, alias lookup after sorting, and partial
catalog cleanup. These are raw assertion counts, not independent scenario counts.
The appearance suite and build pass. Logs are retained in this directory.

## Guest fixture and observations

Use private copies of this branch's built `disk/os64.img` and `disk/os64_data.img`
with its built ISO. The QEMU GUI boot uses the FAT root. Add Source Sans 3 and
Source Code Pro from the pinned fixtures to that copy's `/etc/fonts`, alongside
the two shipped DejaVu fonts, to represent Chris's four-font download. Add
byte-identical DejaVu copies named `/fonts/sans-copy.ttf` and
`/fonts/mono-copy.ttf` to the private home filesystem. Install
`initial-fonts.conf` at `/fonts.conf` on that filesystem. These changes affect
private test images, not the user's home or backup images.

Run `docs/fonts/f5-evidence/run_guest.py . . /tmp/quinn-font-union-guest --resume --label union`
after preparing those images. `--resume` skips the harness's payload injection.
Its action stream opens Workshop, examines Interface and Document, saves, and
refreshes. The inspected screenshots show five rows (Builtin plus four fonts),
with Document correctly highlighting Sans while its configured path remains
`/home/fonts/sans-copy.ttf`. The guest shuts down successfully. An early screenshot
was taken before tab switching completed; it is not used as font-list evidence.

A second cold boot (`--resume --label saved`) after Save again shows the five
rows and the Document alias selected; `reboot.png` was inspected. Offline
extraction of `saved-fonts.conf` confirms Interface still names the system Sans
copy and Document still names its personal duplicate. Both guests exit 0.

Stored build/serial logs normalize line endings and trim trailing whitespace;
original logs remain under `/tmp/quinn-font-union-*` for this session.
