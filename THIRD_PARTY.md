# THIRD_PARTY.md — the code os64 ships that somebody else wrote, and the monthly scan

*Started 2026-10-23 by Fable at Chris's ask, the day a server-side hole in
most of the web's pages made him ask how a one-person browser stays on
top of such things. The answer: browser vulnerability lists are about
engines we do not run, so they are noise for us; what applies is the code
we EMBED, because a browser's attack surface is its parsers of hostile
bytes, and several of ours are someone else's. This file is the list and
the log. It lives in the root on purpose: it is read once a month by
whoever does the scan, and it must not be lost in a folder of designs.*

## The rule

**Once a month, every row below is checked against its upstream, and the
log at the foot gets an entry either way.** "Nothing new" is an entry too:
the log is how the next reader knows the scan happened. An upgrade is a
PR like any other (the component's `UPSTREAM_REVIEW.md` and `manifest.json`
move with it, its patch series rebases, its host harness runs), and the
log entry names the PR. A security fix upstream that touches a file we
ship is taken within the month; one that touches a file we exclude is
noted as such and left.

Each row says what the component is FOR and WHAT A BUG THERE REACHES, so
the scan knows which advisories matter. Every component runs in the one
process that holds the user's files: there is no process sandbox between
a decoder and the browser that called it (the work pool is threads), and
that is a known cost of one person on one machine, recorded here so it
is not mistaken for an oversight.

## The components

| Component | Pinned at | Where in the tree | What it is for | What a bug there reaches | Upstream, and where fixes are announced |
|---|---|---|---|---|---|
| **QuickJS** | snapshot 2026-06-04, patches 0001–0011 (`libjs/patches/`) | `userland/libjs/upstream/`, `manifest.json` | every page's JavaScript | a page's script escaping the engine into yonder's process | https://bellard.org/quickjs/ (the Changelog in each tarball; no advisory list, so the snapshot's diff is the scan) |
| **FreeType** | 2.14.3 (tag VER-2-14-3, 2026-03-22), patches in `libfreetype/patches/` | `userland/libfreetype/upstream/`, `manifest.json` | every glyph on the screen, and web fonts a page sends | a font file running code; web fonts make this a page's input, not only the system's | https://freetype.org/ (release notes), https://gitlab.freedesktop.org/freetype/freetype; CVE search "freetype" |
| **libwebp** | 1.6.0 (v1.6.0) | `userland/libwebp/upstream/`, `manifest.json` | WebP pictures | a picture running code (the 2023 WebP zero-day's class, CVE-2023-4863) | https://chromium.googlesource.com/webm/libwebp (NEWS), https://groups.google.com/a/webmproject.org/g/webp-discuss; CVE search "libwebp" |
| **libjpeg-turbo** | 3.2.0 | `userland/libjpeg/upstream/`, `manifest.json` | JPEG pictures | a picture running code | https://github.com/libjpeg-turbo/libjpeg-turbo/releases, its SECURITY.md; CVE search "libjpeg-turbo" |
| **BearSSL** | commit 7bea48e5 (2026-04-06), 39 commits after v0.6 | `userland/libtls/upstream/`, `UPSTREAM_REVIEW.md` | TLS for every https page and for sshd | a bad certificate accepted; a hostile server reaching into the handshake or the record layer | https://bearssl.org/ (the changelog and the git log; releases are rare, the git log is the scan) |
| **musl** (maths) | 1.2.5 | `userland/libmath/upstream/`, `manifest.json` | libm | wrong numbers, not code execution; scanned for correctness fixes | https://musl.libc.org/releases.html |
| **musl** (decimal conversion) | commit 5e9972ea | `userland/libpage/upstream/musl/` | HTML's number parsing (`floatscan`) | a malformed number read wrong; hostile input, so a bounds fix upstream matters | same as above |
| **ryu** | commit 4c0618b0 | `userland/libpage/upstream/ryu/` | shortest float printing | wrong digits; not hostile input | https://github.com/ulfjack/ryu |
| **Limine** | `v8.x-binary` branch, depth 1 — a FLOATING branch, at v8.7.0-binary on this machine (2025-01-10) | `limine/` (cloned by the root GNUmakefile), `kernel/src/limine.h` pinned by hash to commit c171e847 | the bootloader | boot-time only; no page reaches it; scanned so the floating branch does not surprise a fresh clone | https://github.com/limine-bootloader/limine/releases |
| **freestnd-c-hdrs-0bsd** | commit 0353851f | `kernel/freestnd-c-hdrs-0bsd/` (`kernel/get-deps`) | freestanding C headers | none at runtime; headers only | https://github.com/osdev0/freestnd-c-hdrs-0bsd |
| **cc-runtime** | commit 13fe6383 | `kernel/cc-runtime/` (`kernel/get-deps`) | the compiler's runtime helpers | none reachable from a page | https://github.com/osdev0/cc-runtime |

**Ours, and therefore not on anyone's list**: libhtml, libgarb, libflow,
libfetch, libway, libdom, libgzip (inflate and deflate), the PNG and GIF
decoders in libpng and libimage, the WOFF reader, the TLS client engine's
glue, the HTTP parser. Each parses bytes a page sent. Their defense is the
host harness under the sanitizers and the mutant run, and the rule that
web content stays hostile; the next step for each, when there is time, is
a fuzzer over the standalone build the harness already makes. **One
exception to "browser lists are noise"**: when a Chrome or Firefox
advisory names a file-format bug (WOFF2, WebP, JPEG, PNG, GIF, a font
table), that is a hostile input shape worth a case in our decoder's
harness whether or not our code shares the bug.

## The scan, step by step

1. For each row, open the upstream's releases or changelog and compare
   against "Pinned at". Read what changed since the pin; look for the
   words fix, overflow, bounds, crash, CVE, security.
2. A fix in a file we ship (`manifest.json`'s file list, or the
   `UPSTREAM_REVIEW.md` inventory): take it, as a bump or a cherry-pick
   into the patch series, with the component's harness run and its
   review document updated. A fix in a file we exclude: note it and
   leave it.
3. Write the log entry below, newest first: the date, who scanned, and
   one line per component that moved or had a fix, or "nothing new".
4. Check Limine's floating branch: if `git -C limine describe --tags`
   says a different tag than the row, update the row and say whether the
   boot was re-tested.

## Log

| Date | By | Result |
|---|---|---|
| 2026-10-23 | Fable | The list written from each `manifest.json` and `UPSTREAM_REVIEW.md`; no scan of upstreams this day. First scan due by 2026-11-23. Limine noted as a floating branch rather than a pin. |
