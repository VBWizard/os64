# Blob URLs and FileReader

**Status: APPROVED TO BUILD 2026-11-17** (Fable's review, its eight
findings taken below; the review is kept in the Opus kit as
`BLOB_URLS_REVIEW.md`); the store's bound ruled by Chris the same day
(§ 5). This is WORKERS.md's slice W0 (§ 9 and § "The blob URL store") made
concrete, with FileReader beside it. Chris asked for both on 2026-11-17,
after a browser Minecraft game he tried stalled at 50%: its loader
downloads one zip of its assets (the first half of the bar), unpacks each
file into a `Blob`, and calls `URL.createObjectURL` on it (the second
half), then uses those addresses as pictures and as CSS
`background-image: url(...)`. yonder has no `createObjectURL`, so the
first call throws and the bar stops.

The standards are the File API (§ 8 FileReader, § 9 "A URL for Blob and
MediaSource reference"), the URL Standard (a blob URL's origin and its
blob URL entry, captured when the URL is parsed) and Fetch (§ 4.2 "scheme
fetch", its `blob` case).

## 0. What is wanted

| | Hosts (P5 corpus pulls of 11/07 and 11/11, 182 hosts) |
|---|---|
| call `URL.createObjectURL` | 69 |
| call `URL.revokeObjectURL` | 46 |
| construct a `FileReader` | 33 (`readAsArrayBuffer` 20, `readAsText` 16, `readAsDataURL` 15) |

What the 69 do with the address, from the 250 bytes either side of each
call (a host counts once for each it does): give it to a `.src` (24 — a
picture, a sound, a script), a `Worker` (15, which waits for W1), a
download link (15), an `.href` (13), `fetch` or `XMLHttpRequest` (13),
media objects (12 — `MediaSource`, which yonder does not have), CSS
`url()` (5), `import()` (4).

**The game that started it**, measured: its archive unpacks to 56 MiB in
867 files — 36 MiB of Ogg audio, 18 MiB of MP3, under 2 MiB of pictures and
fonts. Past this slice it meets three walls of its own: its Ogg sounds
(libav is built without Vorbis), the IndexedDB module it ships, and WebGL
if it draws in 3D. This slice gets its loader past 50% and its pictures
and backgrounds drawn.

**Out of this design**, each a DEBTS row written with the code: workers
(W1, where 15 of the 69 go next); `createObjectURL(MediaSource)` (a
TypeError here, as in a browser without MSE); opening a blob URL as a page
(`<a href=blob:>` followed without `download`, `window.open(url)`,
`location = url`: refused by name, decision 8); a ranged blob fetch; a
second window of the same origin fetching the first one's blob URLs
(decision 4); a picture's, a style's and a font's take after the script's
task (decision 3); and `FileReaderSync` (a worker's).

## 1. Decisions

1. **One store per page, owned by libdom, handed out to yonder's doors.**
   `os64_dom_blob_store_t` is made by yonder with the Page (the same life
   as the page's binding) and given to the binding in its options. It is
   a map from the URL's text to an entry: the Blob's bytes, **copied** out
   of the engine, and its type. It has a lock and its entries are
   refcounted with atomics, because the readers that hold an entry run on
   pool threads, and W1's workers will write the store from theirs.
   *Alternative considered:* sharing the Blob's `DKept` bytes instead of
   copying. `DKept` is per-binding and its counts are not thread-safe;
   making them so reaches every kept body (fetch, media). The copy costs
   memory once per URL made (decision 6 bounds it); the doors make no
   further copy (decision 8).

2. **The URL is `blob:<origin>/<uuid>`.** The origin is the page's,
   serialized (`blob:https://example.com/…`), or `null` for an opaque one
   (a `file:` page, a sandboxed document): `blob:null/<uuid>`. The uuid is
   `crypto.c`'s `randomUUID` over the host's entropy. `createObjectURL`
   takes a `Blob` or `File`; anything else is a TypeError (a
   `MediaSource` included). A Blob of any size is accepted, within
   decision 6.

3. **A blob URL is taken where the standard parses it.** The URL Standard
   captures the entry when the URL is parsed, and a page may count on it:
   `fetch(url)`, `xhr.open(…, url)` or a link's click followed at once by
   `revokeObjectURL(url)` must still work (WPT's `FileAPI/url/` tests the
   first two; the third is the hand-rolled download,
   `a.href = url; a.click(); URL.revokeObjectURL(url)`). The take is
   `os64_dom_blob_url_take(store, url)`: a held entry (a reference taken)
   or NULL. Who takes, and when, is per door, in decision 8's "taken at"
   column; the rule written beside `request_job` and in libdom's fetch
   and navigation is **"a blob URL is taken where the standard parses
   it"**. Where libdom parses (the `Request` constructor and `fetch()`,
   `xhr.open()`, a link followed or `location` assigned), libdom takes and
   hands the held entry to the host with the request
   (`os64_dom_request_t.blob`) or the navigation ask (`dom->navigation`,
   which lasts past the task); the host takes its own reference from it.
   Where yonder is told inside the call (a script element connected), its
   notice takes. Where the request comes later than the standard parses
   (a picture at the page's next picture pass, a style at the cascade, a
   font at first use), the gap is booked by name: the pattern there is to
   revoke in `onload`, after the request. A request carries its held entry
   to the pool, the run answers from it, the release drops the reference.
   A request made from a blob URL with no entry is a network error.

4. **A blob URL resolves only in its own page's store.** An entry is
   found only in the store of the page that made it, so a hit is always
   same origin, and the origin comparison the standard makes is true by
   construction. That is exact while yonder has no frames and no workers.
   Two differences from other browsers follow, each booked: a same-origin
   frame, which must see its parent's entries (IFRAME.md's design decides
   whether a frame shares its top page's store); and a second yonder
   window of the same origin, which in other browsers can fetch the first
   window's blob URL while that document lives, and here misses.

5. **The answer is the standard's scheme fetch for `blob`.** A `GET` only
   (anything else is a network error), status 200 `OK`, `Content-Type` the
   entry's type (absent when the type is empty) and `Content-Length` its
   length, the body the bytes whole, the response basic. No cache entry is
   written or read. A redirect to a blob URL is already refused by libway
   (`way_main_fetch` serves only http(s) and `data:` past the first hop),
   and stays so. A `Range` header is ignored and the whole body served
   (booked).

6. **The store is bounded, and its bytes are the page's.** Its bytes count
   in the page's memory record (`memory:` line, a `blob urls` figure beside
   `pictures`), and the store refuses an entry that would take it past
   **256 MiB**: `createObjectURL` then throws a `QuotaExceededError`. The
   standard sets no limit; browsers have storage limits of their own and
   fail in their own ways, and other browsers do not throw here. A page
   with no `catch` therefore stops where it stops today, and the record's
   `FAILED` line (decision 11) is then the only place that says why; that
   is accepted. The game measured in § 0 holds 56 MiB in the store, under
   a quarter of the bound; with the archive, the unpacked buffers and the
   Blobs in the engine beside it, its whole load is about 225 MiB of the
   page's 1 GiB. The bound is `OS64_DOM_BLOB_STORE_MAX` (dom.h), a constant
so that it can move (Chris, 2026-11-17).

7. **Revocation removes the name, not the bytes.** `revokeObjectURL(url)`
   removes the map's entry when the URL is in the store (a URL of another
   origin, or none, is ignored without a word, as the standard says); a
   request already holding the entry still answers from it. When the page
   goes, the store is freed and every entry with it, except those held by
   requests still in flight, which free themselves on release.

8. **Every door that fetches a URL a page names takes a blob URL.** Each
   door's change is the same three steps: take the entry (or receive
   libdom's) at the "taken at" moment, carry it, answer from it in the
   run. The entry's bytes never change, so a door that reads its input
   whole reads them **in place**: a picture's decode input, a classic
   script's source and a download's write are views of the held bytes,
   and a font's face is made from them as a fetched face is from its
   body. A door whose consumer takes a fetched BODY (fetch,
   XMLHttpRequest, modules, sheets, media's loader) is answered by the
   fetch engine (`blob_answer`, fetch_job.c) with a body copied from the
   entry, because that consumer owns and frees what it is handed; one
   that would have streamed the network's body takes the copy under no
   cap, as the network's would have had none.

   | Door | Taken at | Where | What a blob URL serves |
   |---|---|---|---|
   | `fetch` (`Request` constructor, `fetch()`) | inside the call (libdom) | `request_job` (`yonder_fetch_run`) | decision 5's response |
   | `XMLHttpRequest`, async and sync | inside `open()` (libdom) | `request_job`, `script_run` | decision 5's response |
   | Classic `<script src>` connected by a verb | inside the call (yonder's `script_connected` notice) | the script host's request | the script, compiled as a fetched one is, its address the blob URL; no bytecode kept (no cache entry) |
   | Classic `<script src>` from the parser | when the parser reaches it (the standard's moment too) | the script host's request | as above |
   | Modules: a graph's fetches, `import()` | when the graph asks for the module (the standard's "fetch a module script"), on the window thread | the script host's request | the module |
   | Download: `<a download href=blob:>` clicked or `.click()`ed | inside the activation (libdom, with the navigation ask) | `downloads_feed` → `download_job.c` | the file, named by `download`, else from the type |
   | Pictures: `<img>`, `<picture>`'s `<source srcset>`, `<input type=image>`, CSS `background-image` and the rest of the picture catalog, a Canvas image source | the page's next picture pass (later than the standard: **booked**) | `picture_submit` → `picture.c` | the picture, decoded as a fetched one is; origin-clean |
   | Style sheets: `<link rel=stylesheet>`, `@import` | the sheet's fetch (later: **booked**) | `sheet_fetch` → `sheet.c` | the sheet; a `rel=preload` of one is not sent, its sheet takes its own entry |
   | Web fonts: `@font-face src: url(blob:)` | first use of the face (later: **booked**) | `webfonts_feed` → `webfont.c` | the face's bytes |
   | Media: `<audio src>`, its `<source>`s, a `<video>`'s sound | the resource selection's turn, after a stable state (the standard's moment too) | `media.c` (`yonder_fetch_media_run`) | the sound, its byte window the whole body |
   | A link followed without `download`, `window.open(url)`, `location = url` | — | — | **refused by name** (`opening a blob: URL as a page is not supported`), booked |
   | `<iframe src>` | — | frames do not load (IFRAME.md) | not applicable |
   | `<track src>`, `<link rel=icon>`, `cursor: url()`, `<embed>`, `<object>`, SVG `<image href>`, an external SVG `<use href>` | — | yonder fetches none of these today | not applicable; each gains the take the day it fetches |

9. **Nothing central lets a blob URL reach the network.** Below every door
   there is one floor already: libfetch refuses any scheme but `http` and
   `https` when it parses an address (`http.c`, `HTTP_URL_SCHEME`, which
   `os64_fetch_open` answers as `OS64_FETCH_BAD_URL`), and every network
   fetch in yonder goes through it — the fetch job's hops, `way_fetch_whole`
   (web fonts), a download's `os64_fetch_open`. So a `blob:` address with
   no held entry that reaches any door, listed or not, fails closed there.
   The proof suite holds that per door (§ 3), and the door names the
   failure in the record (decision 11) when it can.

10. **FileReader is the File API's § 8, whole but for the sync form.** The
    interface: `readAsArrayBuffer`, `readAsBinaryString`, `readAsText(blob,
    encoding)`, `readAsDataURL`, `abort`; `readyState` (EMPTY, LOADING,
    DONE), `result`, `error`; the six event handlers and their events
    (`loadstart`, `progress`, `load`, `abort`, `error`, `loadend`) as
    `ProgressEvent`s with `loaded` and `total`. It reads only Blobs the
    binding already holds whole, so a read never waits on anything: each
    read is **one queued task**, folded into the posted-message queue
    (`os64_dom_message_next/_fire`, its source named "file reading" in
    the record), that fires `loadstart` then one `progress` then `load`,
    and then `loadend` **only if the reader is not LOADING again**: a
    `load` handler that starts the next read (the chunked-reading loop)
    suppresses the first read's `loadend`, as the standard says. Starting
    a read while LOADING is an `InvalidStateError`. `abort()` during
    LOADING cancels the queued delivery and fires `abort` then `loadend`;
    `abort()` in EMPTY or DONE fires nothing and only sets `result` to
    null. `readAsText` decodes with the label given (the Encoding
    Standard's labels libdom's `TextDecoder` knows), falling back to the
    Blob's charset parameter and then UTF-8, BOM sniffed. `readAsDataURL`
    is `data:<type>;base64,<…>`, `application/octet-stream` for an empty
    type. A result past the engine's string or buffer limit is an `error`
    event with a `NotReadableError`, not a crash. WPT's
    `FileAPI/reading-data-section/` holds all of it.

11. **Both are Modern Web surface, and the record says what happened.**
    `createObjectURL`, `revokeObjectURL` and `FileReader` join the rows
    table beside `URL` and `Blob` (modern.c), so a page read with the
    Modern Web off has none of them, as it has no `Blob` today. A request
    answered from a blob URL is recorded as a `data:` one is (`request GET
    blob:…: 200 blob, N bytes`); a miss is `FAILED` with why (`revoked or
    never made`, `not this page's`, `not GET`, `opening a blob: URL as a
    page is not supported`); the `memory:` line gains the store's bytes
    and entries; a refused `createObjectURL` is a `FAILED` line naming the
    bound. The census counts `createObjectURL` and `FileReader` as read
    the day they are, so the tally stops listing them.

## 2. The entry's lifecycle

| State | How it is entered | What holds it | Leaves to |
|---|---|---|---|
| **named** | `createObjectURL`: bytes copied, the map takes the first reference | the store's map | **held** (a take), **unnamed** (revoked, or the page goes) |
| **held** | a take, on the window thread: a reference for each request or navigation ask | the map and each holder | **named** (each holder released), **orphaned** (revoked or page gone while held) |
| **orphaned** | revocation or the store's free while holders remain | holders alone | **freed** at the last release, on whichever thread releases |
| **unnamed → freed** | revoked or the page gone with no holder | nothing | — |

Invariants: the bytes never change after creation (no reader locks them,
and a door reads them in place); a take happens only on the window
thread under the store's lock, so it cannot race a revocation of the same
URL; the last release frees, on any thread, which is why the count is
atomic and the store's lock is not needed to drop a reference.

## 3. Proof

- **libdom's host suite** (`tools/test_dom_host.sh`, a `--blob` probe so a
  run takes minutes, not the full suite's 90): the URL's form and origin
  (opaque included), the TypeErrors, revoke then fetch, revoke right after
  `fetch()`, `xhr.open()` and a link's `.click()` (each still answers),
  the store's free with a holder (ASan), the bound, two threads releasing
  (a stress case), and FileReader's every read kind, event order, the
  suppressed `loadend` of a read restarted in `load`, `abort` in each
  state, the re-entrancy `InvalidStateError`, the encodings, and an
  oversized result.
- **WPT**: `FileAPI/url/` and `FileAPI/reading-data-section/` added to
  `tools/wpt_fetch.sh`, run in the scripted-page host; their expectations
  pinned.
- **yonder's host suite**: a scripted page that makes a picture, a
  background, a sheet, a script, a module, a font and a sound from blob
  URLs and fetches one, each door asserted; a revoked or unknown blob URL
  failing at each door of decision 8's table, including the refused and
  not-applicable rows (decision 9's floor); a download saved whole after
  `a.click(); URL.revokeObjectURL(url)`.
- **QEMU**: `tools/pages/blob-urls.html` showing each door (a picture made
  from bytes, a background, a script, a font, a sound, a download,
  FileReader's four reads printed), then the game's loader on the P5 past
  50% (Chris), with § 0's honest note.
- A cold Opus read, then Codex: the store's two threads are the part
  worth a reviewer.

## 4. Slices

Two PRs, stacked: **B1** the store, `createObjectURL`/`revokeObjectURL`,
the takes and the doors; **B2** FileReader (no yonder change but its
record line). B1 is the one with threads in it.

## 5. Questions

1. **Ruled (Chris, 2026-11-17): the store's bound is 256 MiB**, as a
   constant that can be changed (`OS64_DOM_BLOB_STORE_MAX`), counted in
   the page's memory record.

Fable's answers to the draft's others, taken: FileReader folds into the
posted-message queue with its source named (decision 10); the bytes are
copied now, read in place at the doors (decisions 1 and 8); and the rule
is written as "a blob URL is taken where the standard parses it"
(decision 3).
