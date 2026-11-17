# Blob URLs and FileReader

**Status: DRAFT 2026-11-17, for Fable's read before any code.** This is
WORKERS.md's slice W0 (§ 9 and § "The blob URL store") made concrete, with
FileReader beside it. Chris asked for both on 2026-11-17, after a browser
Minecraft game he tried stalled at 50%: its loader downloads one zip of
its assets (the first half of the bar), unpacks each file into a `Blob`,
and calls `URL.createObjectURL` on it (the second half), then uses those
addresses as pictures and as CSS `background-image: url(...)`. yonder has
no `createObjectURL`, so the first call throws and the bar stops.

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

Out of this design, each with its DEBTS row: workers (W1, which is where
15 of the 69 go next), `createObjectURL(MediaSource)` (a TypeError here, as
in a browser without MSE), opening a blob URL as a page (§ 6), Range
requests of a blob URL, and `FileReaderSync` (a worker's).

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
   memory once per URL made (decision 6 bounds it), and is what WORKERS.md
   § "The blob URL store" chose.

2. **The URL is `blob:<origin>/<uuid>`.** The origin is the page's,
   serialized (`blob:https://example.com/…`), or `null` for an opaque one
   (a `file:` page, a sandboxed document): `blob:null/<uuid>`. The uuid is
   `crypto.c`'s `randomUUID` over the host's entropy. `createObjectURL`
   takes a `Blob` or `File`; anything else is a TypeError (a
   `MediaSource` included). A Blob of any size is accepted, within
   decision 6.

3. **A blob URL is resolved when a request is made, not when it runs.**
   The URL Standard captures the entry when the URL is parsed, so a page
   may set `img.src = url` and revoke the URL on the next line and the
   picture still loads, and a page may count on it. So each door, on the
   window thread, at the moment it creates its request, asks
   `os64_dom_blob_url_take(store, url)`: a held entry (a reference taken)
   or NULL. The request carries the held entry to the pool; the run
   answers from it; the release drops the reference. A request made from
   a blob URL with no entry (never made, revoked before the request, or
   another page's) is a network error.

4. **A blob URL resolves only in its own page's store.** An entry is
   found only in the store of the page that made it, so a hit is always
   same origin, and the origin comparison the standard makes is true by
   construction. That is exact while yonder has no frames and no
   workers: it is the one place a cross-origin blob URL could come from.
   The day frames load (IFRAME.md) a frame's page has a store of its own
   and its parent's URLs miss there, which the standard also gives
   (different origins) except for a same-origin frame, which must see its
   parent's entries — IFRAME.md's design decides whether a frame shares
   its top page's store; noted there, not solved here.

5. **The answer is the standard's scheme fetch for `blob`.** A `GET` only
   (anything else is a network error), status 200 `OK`, `Content-Type` the
   entry's type (absent when the type is empty) and `Content-Length` its
   length, the body the bytes whole, the response basic. No cache entry is
   written or read. A redirect to a blob URL is already refused by libway
   (`way_main_fetch` serves only http(s) and `data:` past the first hop),
   and stays so. A `Range` header is ignored and the whole body served
   (DEBTS: the standard's ranged blob fetch).

6. **The store is bounded, and its bytes are the page's.** Its bytes count
   in the page's memory record (`memory:` line, a `blobs` figure beside
   `pictures`), and the store refuses an entry that would take it past
   **256 MiB**: `createObjectURL` then throws a `QuotaExceededError`. The
   standard sets no limit and says nothing of one; browsers have storage
   limits of their own and fail in their own ways, and a throw is the
   answer a page's `catch` can see. *Question 1 below.*

7. **Revocation removes the name, not the bytes.** `revokeObjectURL(url)`
   removes the map's entry when the URL is in the store (a URL of another
   origin, or none, is ignored without a word, as the standard says); a
   request already holding the entry still answers from it. When the page
   goes, the store is freed and every entry with it, except those held by
   requests still in flight, which free themselves on release.

8. **Every door that fetches a URL a page names takes a blob URL**, by
   decision 3. Each door's change is small, the same three steps: take
   the entry where the request is created, carry it, answer from it in
   the run (or, where the door answers a `data:` URL on the window
   thread, answer there). The doors:

   | Door | Where the request is made | What a blob URL serves |
   |---|---|---|
   | Pictures: `<img>`, CSS `background-image` and the rest of the picture catalog, a Canvas image source | `picture_submit` (yonder.c), run by `picture.c` | the picture, decoded as a fetched one is; origin-clean |
   | Style sheets: `<link rel=stylesheet>`, `@import` | `sheet_fetch` → `sheet.c` (`yonder_fetch_style_run`) | the sheet |
   | Scripts: classic `<script src>`, module graphs and `import()` | the script host's requests (`scripts_for`, `request_job`) | the script, compiled as a fetched one is, its address the blob URL (bytecode keyed by it is not kept: there is no cache entry) |
   | `fetch` and `XMLHttpRequest`, async and sync | `request_job`, `script_run` (`yonder_fetch_run`) | the response of decision 5 |
   | Web fonts: `@font-face src: url(blob:)` | `webfonts_feed` → `webfont.c` (`way_fetch_whole` today) | the face's bytes |
   | Media: `<audio src>`, a `<video>`'s sound | `media.c` (`yonder_fetch_media_run`) | the sound; its byte window (`media_bytes.c`) is the whole body at once |
   | Downloads: `<a download href=blob:>` and a blob URL the page asks to save | `downloads_feed` → `download_job.c` | the file, named by the `download` attribute or the Blob's type |

   A door that cannot carry an entry yet answers a blob URL with a
   network error that names it (`a blob: URL this door does not take`),
   never by fetching the text as an address.

9. **FileReader is the File API's § 8, whole but for the sync form.** The
   interface: `readAsArrayBuffer`, `readAsBinaryString`, `readAsText(blob,
   encoding)`, `readAsDataURL`, `abort`; `readyState` (EMPTY, LOADING,
   DONE), `result`, `error`; the six event handlers and their events
   (`loadstart`, `progress`, `load`, `abort`, `error`, `loadend`) as
   `ProgressEvent`s with `loaded` and `total`. It reads only Blobs the
   binding already holds whole in memory, so a read never waits on
   anything: each read is **one queued task** that fires `loadstart`, then
   — the bytes being there — one `progress`, then `load` and `loadend`,
   in the standard's order. The task is queued on the binding's host
   queue the way a posted message is (`os64_dom_message_next/_fire`), so
   it runs after the current task and in order with other queued work.
   *Question 2.* Starting a read while one is LOADING is the standard's
   `InvalidStateError`; `abort` during LOADING cancels the queued task's
   delivery and fires `abort` then `loadend`. `readAsText` decodes with
   the label given (the Encoding Standard's labels that libdom's
   `TextDecoder` knows), falling back to the Blob's charset parameter and
   then UTF-8, BOM sniffed. `readAsDataURL` is `data:<type>;base64,<…>`,
   `application/octet-stream` for an empty type. A result past the
   engine's string or buffer limit is an `error` event with a
   `NotReadableError`, not a crash.

10. **Both are Modern Web surface.** `createObjectURL`, `revokeObjectURL`
    and `FileReader` join the rows table beside `URL` and `Blob`
    (modern.c), so a page read with the Modern Web off has none of them,
    as it has no `Blob` today.

11. **The page record says what happened.** A request answered from a
    blob URL is recorded like a `data:` one (`request GET blob:…: 200
    blob, N bytes`), a miss as `FAILED` with why (`revoked or never made`,
    `not this page's`, `not GET`, `a door that does not take blob: URLs`);
    the record's `memory:` line gains the store's bytes and entries, and a
    refused `createObjectURL` is a `FAILED` line with the quota. The
    census counts `createObjectURL` and `FileReader` as read the day they
    are, so the tally stops listing them.

## 2. The entry's lifecycle

| State | How it is entered | What holds it | Leaves to |
|---|---|---|---|
| **named** | `createObjectURL`: bytes copied, the map takes the first reference | the store's map | **held** (a door takes it), **unnamed** (revoked, or the page goes) |
| **held** | a door's take, on the window thread: a reference per request | the map and each request | **named** (each request released), **orphaned** (revoked or page gone while held) |
| **orphaned** | revocation or the store's free while requests hold it | requests alone | **freed** at the last release, on whichever thread releases |
| **unnamed → freed** | revoked or the page gone with no request holding it | nothing | — |

Invariants: the bytes never change after creation (no reader locks them);
a take happens only on the window thread under the store's lock, so it
cannot race a revocation of the same URL; the last release frees, on any
thread, which is why the count is atomic and the store's lock is not
needed to drop a reference.

## 3. Proof

- **libdom's host suite** (`tools/test_dom_host.sh`, a `--blob` probe so a
  run takes minutes, not the full suite's 90): the URL's form and origin
  (opaque included), the TypeErrors, revoke-then-fetch, revoke after a
  request was made (the take survives), the store's free with a request
  holding an entry (ASan), the quota, two threads releasing (a stress
  case), and FileReader's every read kind, event order, `abort`, the
  re-entrancy `InvalidStateError`, the encodings, and an oversized
  result.
- **WPT**: `FileAPI/url/` and `FileAPI/reading-data-section/` added to
  `tools/wpt_fetch.sh`, run in the scripted-page host; their expectations
  pinned.
- **yonder's host suite**: a scripted page that makes a picture, a sheet,
  a script and a font from blob URLs and fetches one, each door asserted;
  a revoked URL failing at each door; a download of a blob saved whole.
- **QEMU**: `tools/pages/blob-urls.html` showing each door (a picture
  made from bytes, a background, a script, a font, a sound, a download,
  FileReader's four reads printed), then the Minecraft game's loader on
  the P5 past 50% (Chris), with the honest note that what it needs after
  that (IndexedDB, and WebGL if it draws in 3D) is not this slice.
- A cold Opus read, then Codex: the store's two threads are the part
  worth a reviewer.

## 4. Slices

One PR or two stacked, Opus's call at the code: **B1** the store,
`createObjectURL`/`revokeObjectURL` and the doors; **B2** FileReader (no
yonder change but its record line). B1 is the one with threads in it.

## 5. Questions

1. **For Chris: the store's bound.** 256 MiB of copied bytes a page, past
   which `createObjectURL` throws. Pages that unpack an archive into blob
   URLs (the Minecraft loader, a texture pack) are the ones that reach for
   it; `page_memory_mb` (1024 on the P5) is the page's whole budget.
   Lean: 256 MiB, counted in the page's memory record.
2. **For Fable: FileReader's queue.** The File API queues its tasks on the
   "file reading task source"; folding them into the posted-message queue
   gives the right order relative to the page's other queued work and adds
   no host turn. A separate source would need its own `_next`/`_fire` and a
   place in yonder's turn order. Lean: fold, with the task's source named
   in the record.
3. **For Fable: copy or share.** Decision 1 copies the bytes per URL. A
   page that makes many URLs over one large Blob (slices of a sprite
   sheet each given a URL) pays for each. Lean: copy now; share when a
   page shows the cost, which would mean thread-safe kept bodies.
4. **For Fable: the take on the window thread.** Decision 3 puts every
   take where its request is made. A door that builds its request on a
   pool thread from a window-side description would have to take earlier;
   none does today (the table above). Is a rule ("a blob URL is taken
   where the request is described") worth writing beside `request_job`
   for the doors to come?
