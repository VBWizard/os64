# An XMLHttpRequest's bytes, outside the page's heap

*Opus, 2026-11-04. DEBTS' "An XMLHttpRequest keeps its body in the page's
script memory", which Chris ruled a fast follower to Streams S3.*

## 1. Why, measured

XHR keeps every byte a response brings (XHR § "received bytes"), and
libdom keeps them in the engine's heap (`xhr_part`'s `js_malloc`), whose
64 MiB is the page's whole script budget. A speed test that asks for
25 MiB on each of up to eight connections passes the budget at the third,
and the runtime retires: "this page runs without scripts". The DOM host
holds that bound today (`dom_xhr_downloads_case`: two of 25 MiB fit, three
do not).

The bytes are not the page's script memory in other browsers: a response
is held in the browser's blob storage, and a `blob` response refers to it
without a copy. A page that never reads the body (a speed test reads only
`progress`) costs its scripts nothing for it.

## 2. What the page can do with them

| `responseType` | What the page gets | Where the bytes must be |
|---|---|---|
| `""`, `text` | a string, decoded | the heap (a string is the page's) |
| `json` | the parsed value | the heap |
| `arraybuffer` | an ArrayBuffer it may write | memory the ArrayBuffer owns |
| `blob` | a Blob, immutable | anywhere, referred to |
| `document` | (none: yonder makes no document) | |

So text and JSON cost the heap whatever this does, and only when the page
reads them; the arraybuffer and the blob need not cost it at all.

## 3. The choice

**A body is a libdom object of its own (`DReceived`, `received.c`: XHR's "received bytes"): bytes in the process's
memory, not the engine's, refcounted, with a bound of their own.**

- **While it arrives** a body is its pieces, each kept as it came (as
  `xhr_part` kept them in the heap before). With a declared
  Content-Length within the bound, it is one block reserved at the head
  and filled in place, so a body that keeps to its declared length is
  never joined; bytes past it become pieces after the block, joined at
  the end like any other.
- **At its end** a body in pieces is joined into one block, once. The
  join's peak (pieces and block at once) is counted against the bound like
  any other byte; a body that cannot be joined is refused as below.
- **Text and JSON** are decoded from the block into the heap when read, as
  before. Text read while LOADING joins what has come so far (more pieces
  may follow it); a join the bound refuses there throws
  QuotaExceededError, and the response goes on arriving.
- **An arraybuffer** response is an ArrayBuffer made OVER the block
  (QuickJS's `JS_NewArrayBuffer` with a free function), which then owns
  it: the engine does not count memory it did not allocate. The XHR keeps
  no other use of the bytes (its `response` answers the same object after,
  and `responseType` cannot change at DONE), so handing it over is safe.
- **A blob** response is a Blob that REFERS to the body (a `DBlob` gains a
  body reference beside its own bytes; `d_blob_get` still answers a
  borrowed contiguous pointer, so no reader changes). A slice of such a
  Blob refers to the same body.
- **The bound** is the page's, across every body it holds: XHR bodies,
  Blobs over them, ArrayBuffers made over them. Passing it fails THAT
  REQUEST as a network error (its `error` event, status 0) and frees its
  bytes; the page and its runtime go on. A page that outgrows its scripts'
  heap is a broken page; one that asked for one download too many has a
  failed download, which is what other browsers' storage limits look like
  too.
- **What a body costs is visible**: the binding counts the bytes held and
  the requests refused, and yonder's page record says so.
- **No body waits on a finalizer** (libjs/TEARDOWN.md): an engine torn
  down by reclaiming runs none, so the binding keeps its bodies on a list
  and frees what is left of it when the binding goes. An ArrayBuffer's
  free function gives the body's reference back with the bytes, once:
  QuickJS calls it again with no bytes after a detach or a transfer.

## 4. Rulings wanted

1. **The bound.** My lean: **256 MiB a page** (`os64_dom_options_t`'s
   `max_body_bytes`, the host's to set), four times the script heap: eight
   25 MiB downloads at once, with room to keep what they made. Other
   browsers' blob storage spills to disk and is effectively unbounded;
   yonder has no disk tier, and the P5's memory is the machine's.
2. **The failure.** My lean: the request fails as a network error and the
   page goes on (above). The alternative, retiring the runtime as today,
   punishes the whole page for one download.

## 5. Cut, booked

- **fetch()'s Response bodies** stay in the heap: since Streams they are
  chunks of a ReadableStream the page may read as they come, which is a
  different shape (S1–S3's), and `arrayBuffer()`/`blob()` join them there.
  The same `DReceived` would serve them; booked.
- **Spilling to disk** (other browsers' blob storage): no.
- **A Blob made by the page** (`new Blob([...])`) stays engine memory; only
  a response's bytes move.

## 6. Proof

- The DOM host: `dom_xhr_downloads_case` turned around (three, then eight
  25 MiB downloads complete in a page whose script heap is 64 MiB; under a
  64 MiB body bound, three declared ones finish two and three chunked ones
  one, the join's peak counting, the rest network errors, and the page's
  scripts run on, its counts said by `os64_dom_body_stats`); each response
  type read from a declared and a chunked body (`dom_xhr_body_types_case`:
  text, JSON, an ArrayBuffer the page writes, a Blob and a slice of it that
  outlives its XHR), and every byte counted back to zero once the page lets
  them go; the stream sweep fails every allocation of an XHR's head, piece
  and end in turn, the received body's included. Hand mutants of the bound
  and of a Blob's release each fail.
- WPT `xhr/`, `fetch/`, `streams/`: nothing lost.
- QEMU: a page (`tools/pages/`) that keeps more responses than the script
  heap holds, with scripts still running after.
