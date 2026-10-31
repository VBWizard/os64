# BYTECODE_CACHE.md — a script compiled once, run from its bytecode after

*Opus, 2026-10-30, for MODERN_WEB.md's slice 7. The slice's paragraph there
is the brief; this note is the design it needed before code — the key, the
framing, the name a script runs under, where each step runs, every piece of
state against every event — and says where the build departed from the
paragraph, and why.*

## 1. What it is for

The engine recon (MODERN_WEB.md § 3 of the findings) measured QuickJS
compiling at 6 to 20 MB/s and reading its own bytecode about thirty times
faster: YouTube's 10.6 MB bundle took 1,653 ms to compile and 56 ms to read
on the host, 4 to 5 s on the P5 before a byte of it ran. A page's bundles
change far less often than it is visited, so the second visit should pay
the read, not the compile.

The number this slice moves is the record's `script <address>: ran in <n>
us` (MODERN_WEB.md § The instrument), which counts compiling. It drops by
the compile on a page's second visit and by nothing on its first, and the
line says which it was: `ran in <n> us from its bytecode` when the script
was read rather than compiled.

## 2. The shape

Three parts, each in the library that owns its half:

- **libjs** runs a classic script through an `os64_js_bytecode_t`: from a
  blob when one is given and good, else compiling the source and handing
  back the compile's blob when asked. The blob is libjs's framing round
  the engine's own `JS_WriteObject` bytes (§ 4), so a caller holds an
  opaque blob and never meets the engine's format.
- **libway's cache** keeps the blob in a COMPANION file beside the
  script's own entry, `<hash>.b` beside `<hash>.e`, and removes it with
  the entry (§ 5).
- **yonder** reads the companion on the script's worker, after the fetch;
  runs through it on the window thread at the script's turn; and hands a
  compile's fresh blob to a job of its own to write (§ 6).

## 3. The name a script runs under

A classic script is compiled under the binding's name for it,
`os64-script:<n>` (libdom `report.c`, `os64_dom_script_name`), where `n` is
the page's record of that script. libdom reads the name back off a thrown
error's frames to tell which script threw, and so whether its error is
MUTED — another site's script reports "Script error." and nothing more
(HTML's muted errors). The name is compiled into every function's debug
information, so it is in the bytecode.

`n` is per page: one bundle is script 3 on one page and script 7 on
another, or on the same page after a change in what precedes it. A blob
written under one name and run under another would attribute its errors to
whichever script the old number names on this page — wrong at best, an
unmuted foreign error at worst.

**So the name is not part of the blob's identity: the read gives it.**
Patch 0015 adds `JS_ReadObjectNamed`, `JS_ReadObject` with a filename:
every function the read makes takes that name instead of the one written.
Nothing else in a classic script's bytecode depends on its name. The
alternative, keying the blob by name too, would miss whenever a script
moved in a page's order and thrash between pages that share a bundle.

A module is named by its address, which is its identity for resolution
too (`import` resolves against it), so 7b's blob carries the name and is
read only under it, rather than renamed (§ 9).

## 4. The blob (libjs)

    "os64-jsbc 1\n"     magic, 12 bytes
    engine identity     OS64_JS_ENGINE_ID, 64 hex digits
    kind                'c', a classic script, and three zero bytes
    payload length      8 bytes, little-endian
    payload digest      FNV-1a 64 of the payload, 8 bytes, little-endian
    payload             JS_WriteObject(compiled, JS_WRITE_OBJ_BYTECODE)

**The engine's build identity** is a SHA-256 over the prepared engine
sources — upstream as pinned with the patch series applied — and the
manifest's configuration, computed by `tools/js_prepare.py` and written
into the generated tree as `os64_js_engine_id.h`. The bytecode's format is
the engine's (`BC_VERSION`, the opcode and atom tables), and a patch can
change what the compiler emits without changing `BC_VERSION`, so the
identity is the sources', not upstream's number. A blob from any other
build is a miss.

**The digest is the payload's own**, not the source's (Codex's P1 on the
slice paragraph): a source digest says which text was compiled, never that
the bytes on disk are the bytes that were written. The read checks magic,
engine, kind, length and digest before the engine sees a byte, and any
mismatch is a miss: the source is compiled as if there were no blob.
`JS_ReadObject` trusts its input — it is written to read what its own
writer wrote, not to defend against anything else — so nothing reaches it
that libjs did not frame. (Someone who can write `/var/cache` can write
`/bin`; the digest is against tearing and rot, not against them.)

**The verb** (`js.h`) is `os64_js_run_bytecode(runtime, source, length,
name, &bytecode, outcome)`: `os64_js_run` through an
`os64_js_bytecode_t` that carries a blob in (`blob`, `blob_length`, may be
none), a request to keep (`keep`), and two answers (`kept`, an
`os64_malloc`'d blob of the compile it made, and `from_blob`). One verb and
not a pair, because a refused blob — another build's — is exactly when a
compile's fresh blob should come back in the same call. The source is
always carried: a blob is a shortcut to it, never a substitute.

**Judged by the allocator.** Keeping is the host's convenience, never the
script's work: a write that runs out of the runtime's memory leaves no
blob, and its refusal is taken back off the allocator's sticky flags (the
writer frees what it took). And the write is judged by the allocator, not
by its answer: upstream's `JS_WriteObject` returns what it wrote whatever a
failed growth of its buffer or a failed atom index left out — found by
yonder's round trip (§ 8) under a cut allocation, a payload that read back
as a function that crashed when run. A write the allocator refused
anything during is no blob. The reader needs no such judge — it answers
every allocation it is refused with an exception, which the read sweep
shows refusal by refusal — and a read that failed for the runtime's own
reasons (its memory, its deadline) is the runtime's failure, as a
compile's would be. A compile that throws keeps
nothing; one whose run then fails keeps its blob, which describes the
source whatever became of the run.

The execution deadline starts before the read or the compile, as it always
started before `JS_Eval`, so the instrument's number is comparable before
and after.

## 5. The companion (libway)

`way/cache.h` gains, beside the store:

- `way_cache_companion_put(cache, key, tag, bytes, len)` — written as
  entries are, under a writer's own temp name and renamed over its place.
- `way_cache_companion_get(cache, key, tag, &bytes, &len, cap)` — the
  companion of the entry `key` names, when its head names that key and
  `tag`, whole and of exactly its length; false otherwise.

`key` is the entry's (CACHE.md: the page's partition, the credentials, the
address; SCRIPT_CACHE.md § 3). Its head is `os64-cache-companion 2`, the
key's parts (checked, as an entry's are: the file name is a hash), the
`tag`, and the length. A tag is one line of the face's choosing; yonder's
for a classic script is the decoded source's FNV-1a and length
(`yonder_script_bytecode_tag`), so a companion is used only for the very
text it was compiled from, whatever became of its entry between. A
module's (`yonder_module_bytecode_tag`) is its body's, which names its text
because a module is always read as UTF-8; the two kinds tag apart, so an
address fetched as both keeps both.

**Stored as it is, not gzip'd** — the slice paragraph said compressed with
libgzip, and measurement said otherwise. On the host, libgzip deflates a
1.6 MB bundle at 22 MB/s and inflates it at 68 MB/s (to 22%), while the
engine reads its own bytecode at about 600 MB/s (YouTube's 33 MB in 56 ms).
The inflate sits between the fetch and the run, so it would have been the
slowest step of a cached script's path — half a second for YouTube's
bytecode against a 56 ms read — giving back most of what the cache is for.
The disk pays about three times the source instead, within the cache's
cap like everything else.

**It lives and dies with its entry.** `way_cache_drop` removes both; so
does `way_cache_clear`, which counts only the entries it removed; the
sweep lists companions with entries and removes by the same oldest-first
rule, and they count against the same cap; a companion over an eighth of
the cap is not written, as an entry is not. The fetch decides nothing about
it: yonder reads one only for a body the cache holds (`way_whole_t.kept`:
served from the cache, fresh, stale or confirmed, or stored by this fetch),
so a response that said `no-store` never has bytecode kept from it, and
one that stops being kept takes its companion with it.

## 6. Where each step runs

| Step | Thread | Why there |
|---|---|---|
| fetch, decode, the source's key | the script's worker (`script_job.c`) | as before |
| the companion read | the same worker, after the fetch, for a kept body (a module's: its fetch job's worker, § 9) | disk never on the window |
| blob checks, `JS_ReadObjectNamed`, run | the window thread, at the script's turn | the runtime is the window's |
| compile, `JS_WriteObject`, run | the window thread | as before, plus the write: a copy |
| the companion write | a job of its own (`YONDER_JOB_BYTECODE`) | the window never waits on a disk |

The write job holds the blob, the entry's key and the tag, nothing of the
page: a page left while it runs does not stop it, and nothing of the
page's is freed under it. Writes waiting behind the workers are held to 64
MiB (`BYTECODE_QUEUED_MAX`); one that would pass it is not kept, which
costs the next visit a compile and nothing else.

## 7. Lifecycle

State: the COMPANION on disk; the script product's `yonder_bytecode_t`
(the blob read, the address and key a compile's blob is kept under), which
the window's script item takes; the FRESH blob a compile kept, on its way
to the write job.

| Event | Companion | Bytecode record and blob read | Fresh blob |
|---|---|---|---|
| fetched, body kept, companion's tag matches | read | made on the worker, taken by the script's item | — |
| body kept, no companion or another tag | untouched | made with no blob: the compile's will be kept | — |
| body not kept (`no-store`, cache off, too big, a redirect not kept) | not read; removed with the entry it would have replaced | none | none: never kept |
| run from the blob | — | freed after the run | none |
| blob refused at the run (framing, engine, digest, a read the engine refused) | left until the fresh one replaces it | freed after the run | kept |
| compile threw | — | freed after the run | none |
| compiled, its run failed | — | freed after the run | kept: it describes the source |
| the write the allocator refused | — | — | none, and the run goes on |
| handed to the write job | — | — | the job's: written, or freed when the write fails or the queue is full |
| page gone before its script landed | — | freed with the answer | — |
| page left while its script waited | — | freed with the item, as its source is | — |
| page left while the write job runs | written regardless: it describes the source, not the page | — | the job's |
| entry replaced by a new body | stays until its tag misses, then is replaced by the next compile | — | — |
| entry dropped (a no-store answer, `way_cache_drop`) | removed | — | — |
| cache cleared | removed | — | — |
| swept | removed, oldest first, as an entry | — | — |
| two windows, one script | each may write; the rename publishes one whole, as for entries | — | — |
| short of memory anywhere | a miss, or nothing kept; the page runs from source as it always did | freed | freed |

**Caps.** A companion is read only up to `SCRIPT_BYTECODE_MAX`, four times
`SCRIPT_SOURCE_MAX` (bytecode measures about three times its source), and
the script job's `SCRIPT_RESERVE` grows by it. A fresh blob past it is not
kept. The write job reserves the companion's head; its blob is its input,
held to the queue's 64 MiB.

## 8. Proof

- **libjs** (`tools/test_js_runtime_host.sh`, both probes): a blob runs in
  another runtime as its script and keeps nothing; a nested function's
  throw carries the run's name, never the compile's; a blob with another
  magic, engine identity or kind, a flipped payload byte, a length one
  short and a head cut short are each refused, the source run and its blob
  kept; a compile that throws keeps nothing; and two allocation sweeps on a
  small script and on one large enough to grow the writer's buffer and atom
  table: every cut of a keeping run is the write's to absorb (the run going
  on, keeping nothing) or the runtime's, every blob kept reads back and runs
  to the script's answer, every cut of a read is the runtime's failure, and
  nothing leaks. `tools/test_js_prepare.py`: patch 0015's placement, and an
  engine identity that is the same for the same sources and changes with
  any of them.
- **libway**: companion put and get against a temporary directory (the tag,
  the address and the length checked; a torn or grown file a miss; a key
  that is not one line refused; an eighth of the cap; off keeps and reads
  nothing; dropped and cleared with its entry; swept oldest first and
  counted in the bytes, not the entries); and `way_fetch_whole`'s `kept`
  for each way a body is served.
- **yonder, the oracle that matters**: libjs built with
  `OS64_JS_BYTECODE_ROUND_TRIP` (`JS_ROUND_TRIP=1` for the host suites)
  compiles every classic evaluation under a name of its own, writes it,
  reads it back under its real name and runs what was read. The scripted-
  page suite and every WPT directory with expectations run in that build
  and must answer as they do in the normal one: everything the binding has
  been proven on, run through the bytecode, muted foreign errors and every
  error's script name included. A blob that was made and not read back is
  fatal there; one the suite's own injected allocation failure denied is
  counted and compiled instead. The scripted-page suite's own cases hold
  the window's half: a kept body's compile handed to a write job under the
  entry's address and key, its queue let go with the job; a kept form of
  another text is what runs (so the page says which ran); a damaged one is
  refused and the source's kept instead; a body the cache does not hold
  keeps nothing; a form for a page that is gone is freed.
- A mutant harness for the rules (`tools/test_bytecode_mutants.py`).
- **The P5**: a page with a large classic bundle loaded twice; its
  `ran in` before and after, and `ls /var/cache/yonder/*/*.b`.

## 9. Cuts, and what waits

- **7a** — everything above, for `<script src>`.
- **7b** — modules, built 2026-10-30 on the HTTP cache's script requests
  (SCRIPT_CACHE.md § 8 and § 11). They arrive by the fetch job (slice 2's
  path, CORS), not `script_job.c`, which reads the companion on its worker
  for an answer whose body the entry holds, and are compiled with
  `JS_EVAL_FLAG_NO_RESOLVE` before their imports exist. The blob kind is
  `m`, with the module's name in its head (libjs CONTRACT.md § Modules);
  the read makes the module unresolved and leaves its resolution to the
  loader as a compile does. A dynamic `import()`'s module, which the
  engine's own loader compiles, is not among them (DEBTS.md).

Booked, with triggers (DEBTS.md):

| Debt | Why it waits | Trigger |
|---|---|---|
| Inline scripts | a page is not cached, so an inline script has no entry to sit beside; keyed by text alone it would be a second cache with its own cap | a page measured slow on a large inline script |
| `SCRIPT_SOURCE_MAX` is 4 MiB | YouTube's 10.6 MB bundle is refused before it is compiled; the source limit and the runtime's memory are a per-page policy (MODERN_WEB.md's reopened decision) | the per-page memory policy |
