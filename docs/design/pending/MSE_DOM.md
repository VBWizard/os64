# MS.3: the web's side of Media Source Extensions

MEDIA_SOURCE.md § 7 made exact: the design state Fable approved on
2026-11-27 (all eight decisions, and the conditions it set), kept true to
what MS.3.1 built. `libdom/mse.c` is the code, `dom/media.h` § THE FED
SOURCE the seam, and `tools/test_dom_mse.inc` the proof. "§" alone is the
MSE spec (w3c media-source, ED 2026-11-09); "MS §" is MEDIA_SOURCE.md.
**§ 4's table is the contract between libdom (MS.3.1) and the fed session
(MS.2); a change to it by either side goes through Fable, with one line
saying why.**

## 0. Decisions (one line each; the tables say why)

1. **One FIFO for every MSE event, and it is media's own** (`DMediaTask`'s
   queue, drained by `os64_dom_media_fire`), widened to carry a non-node
   target. MSE events and the element's events are then ordered against each
   other exactly as queued, which the spec's orders (`sourceended` before the
   element's `durationchange`, `error` before `updateend`) need.
2. **An append's work is a TASK on that same queue**, between `updatestart`
   and `update`/`updateend`; the parse runs inside it (MS § 9, "where an append
   is parsed"). The seam lets that task answer PENDING, so moving the parse to
   the session's thread later changes yonder, not libdom (§ 4).
3. **The bytes are copied once, by `appendBuffer`, straight into the parser's
   input** (provider `take`), never into a libdom buffer first.
4. **A second registry for object URLs of a MediaSource**, beside the Blob
   store: the Blob store holds byte snapshots and cannot hold a live object
   (`blob_url.c`). Same `blob:<origin>/<uuid>` spelling, one revoke for both.
5. **libdom owns MediaSource's duration and the element's while attached**;
   the session's `facts.duration` is not read for a fed element, and nor is
   `facts.probed`: HAVE_METADATA is every SourceBuffer's first
   initialization segment, which libdom saw.
6. **`buffered` is computed in libdom** from each SourceBuffer's ranges (the
   provider's), by § 10.2's intersection; `DTimeRanges` grows to N ranges.
7. **`isTypeSupported` parses the whole type and reads only `codecs`**, as
   both other browsers do (§ 7's table, § 9.4: a revision of this state's
   first range checks).
8. **v1 has no `audioTracks`/`videoTracks`** on the element or the
   SourceBuffer: libdom has neither list today. The SourceBuffer attributes
   are absent (not empty lists), and a DEBTS row books them (§ 10).
9. **`getVideoPlaybackQuality` is every video's**, not only a fed one's, from
   two counters the facts gain (shown, dropped).
10. **Without the binding's `media_source` switch (dom.h) none of it
   exists**: no MediaSource, SourceBuffer, SourceBufferList or
   VideoPlaybackQuality on window, no `getVideoPlaybackQuality`, and
   `createObjectURL` takes Blobs alone. A page's feature test then answers
   as it did before MSE, so a player takes the plain file it would take in
   a browser without MSE (YouTube's 360p on the P5, 2026-11-20). A host
   sets the switch when it installs a fed source (yonder in MS.3.2, from a
   `yonder.conf` switch; the DOM host's MSE fixture). And the second line of
   defence: **with the switch set but no fed host installed,
   `isTypeSupported` answers no to everything**, so a player is never told
   yes for a MediaSource that can never open.

## 1. The interfaces (§ 3, § 5, § 6, § 10, media-playback-quality)

### MediaSource : EventTarget (constructible; Window only in v1)

| Member | Spec behaviour (algorithm) | Throws (condition, in the spec's order) |
|---|---|---|
| `constructor()` | readyState "closed", duration NaN, empty lists | — |
| `sourceBuffers` | [SameObject] SourceBufferList; empty while closed | — |
| `activeSourceBuffers` | subset in sourceBuffers' order (§ 3.3, 3.15.5) | — |
| `readyState` | "closed" / "open" / "ended" | — |
| `duration` get | NaN if closed, else the value | — |
| `duration` set | duration change (§ 3.15.6) | TypeError (negative or NaN); InvalidStateError (not "open"); InvalidStateError (any SourceBuffer updating); InvalidStateError from the algorithm (below a buffered frame's pts) |
| `addSourceBuffer(type)` | § 3.7: new SourceBuffer, mode from the registry ("segments" for MP4), appended to sourceBuffers, `addsourcebuffer` queued at sourceBuffers | TypeError (empty); NotSupportedError (`isTypeSupported` false, or with the others); QuotaExceededError (more than v1's limit: 2 per MediaSource); InvalidStateError (not "open") — this order, so a closed source asked an unsupported type says NotSupported |
| `removeSourceBuffer(sb)` | § 3.8, table 3.4 | NotFoundError (not in sourceBuffers) |
| `endOfStream(error?)` | end of stream (§ 3.15.7, table 2.3) | InvalidStateError (not "open"); InvalidStateError (any updating) |
| `setLiveSeekableRange(s, e)` | sets the live range (normalized, one range) | InvalidStateError (not "open"); TypeError (s < 0 or s > e) |
| `clearLiveSeekableRange()` | empties it | InvalidStateError (not "open") |
| `static isTypeSupported(type)` | § 7's table; never throws | — |
| `static canConstructInDedicatedWorker` | false in v1 (the spec says true where workers are supported; v1 has no worker MSE, MEDIA_SOURCE.md § 0) | — |
| `handle` | absent in v1 (worker-only) | — |
| `onsourceopen` `onsourceended` `onsourceclose` | event handler attributes | — |

### SourceBuffer : EventTarget (no constructor)

"Removed" below = no longer in its parent's sourceBuffers (§ 5's first check).

| Member | Behaviour | Throws (spec order) |
|---|---|---|
| `mode` get | last set value; initial from the registry | — |
| `mode` set | § 5.1: ended→open (+`sourceopen` queued) BEFORE the parsing check; "sequence": group start = group end | InvalidStateError (removed); InvalidStateError (updating); TypeError (generate-timestamps flag and "segments": never for MP4); InvalidStateError (append state PARSING_MEDIA_SEGMENT) |
| `updating` | true from appendBuffer/remove until their end | — |
| `buffered` | § 5 getter: highest end over tracks; intersect audio and video track ranges; if parent "ended", the last range of each extends to the highest end; cached, a new object only when the ranges change | InvalidStateError (removed) |
| `timestampOffset` get/set | get: libdom's stored value, which every DONE `parse` result overwrites with the SB's offset after the append (sequence mode moves it in coded frame processing, CFP 1.3); set: ended→open before the parsing check; "sequence": group start = the new offset; pushed with `set` | InvalidStateError (removed); InvalidStateError (updating); InvalidStateError (PARSING_MEDIA_SEGMENT) |
| `appendWindowStart` set | stored | InvalidStateError (removed); InvalidStateError (updating); TypeError (< 0 or ≥ end) |
| `appendWindowEnd` set | stored | InvalidStateError (removed); InvalidStateError (updating); TypeError (NaN); TypeError (≤ start) |
| `appendBuffer(data)` | table 3.1 | from prepare append: InvalidStateError (removed; updating; the element has an error); QuotaExceededError (still full after eviction); TypeError (detached buffer: WebIDL) |
| `abort()` | table 3.2 | InvalidStateError (removed); InvalidStateError (parent not "open"); InvalidStateError (range removal running) |
| `remove(start, end)` | table 3.3 | InvalidStateError (removed); InvalidStateError (updating); TypeError (duration NaN); TypeError (start < 0 or > duration); TypeError (end ≤ start or NaN) |
| `changeType(type)` | § 5: reset parser state, pending-init flag set; ended→open | TypeError (empty); InvalidStateError (removed); InvalidStateError (updating); NotSupportedError. **v1 answers only MP4→MP4** (`avc1`↔`avc3`, a level change): MS M5 owns the rest |
| `audioTracks` `videoTracks` `textTracks` | absent in v1 (decision 8) | — |
| `onupdatestart` `onupdate` `onupdateend` `onerror` `onabort` | handler attributes | — |

### SourceBufferList : EventTarget (no constructor)

`length`; indexed getter (undefined past the end); `onaddsourcebuffer`,
`onremovesourcebuffer`. Two instances per MediaSource (sourceBuffers,
activeSourceBuffers). Every add/remove event on them is QUEUED (§ 6).

### The element's additions

| Member | Behaviour |
|---|---|
| `src` = a MediaSource object URL | attach (table 2.1) |
| `buffered` (attached) | § 10.2: activeSourceBuffers' ranges intersected from [0, highest end]; "ended" extends each last range; a new object only when it changes |
| `seekable` (attached) | § 10.1: NaN → empty; +∞ → live range ∪ buffered as one range, else empty if buffered empty, else [0, highest buffered end]; finite → [0, duration] |
| `duration` (attached) | MediaSource's (decision 5) |
| `HTMLVideoElement.getVideoPlaybackQuality()` | a new VideoPlaybackQuality: `creationTime` = performance.now(), `totalVideoFrames`, `droppedVideoFrames`, `corruptedVideoFrames` (0 in v1; deprecated). Counters reset at the element's load algorithm; total counts dropped frames too |
| `srcObject` = MediaSourceHandle | not in v1 (worker MSE) |

## 2. The state machines

### 2.1 MediaSource.readyState and attachment

"Q" = queued on the media FIFO, in the order listed. "S" = synchronous.

| From | Trigger | To | Effects, in order |
|---|---|---|---|
| — | `new MediaSource()` | closed | duration NaN, lists empty |
| closed | element's resource fetch reaches local mode with this source (src = its object URL, the URL resolved in the MediaSource registry) | open | S: `[[has ever been attached]]`; S: element's delaying-the-load-event flag false; S: provider `fed_open` (§ 4); S: readyState "open"; Q: `sourceopen` at the MediaSource. Element continues its local-mode resource fetch |
| open or ended | attach attempted again (another element, the same URL) | unchanged | the OTHER element runs "cannot be fetched at all" (§ 3.15.1 step 1.4: readyState not "closed") |
| open | `endOfStream()` / `endOfStream("network"/"decode")`; or append error (decode) | ended | table 2.3 |
| ended | appendBuffer, remove, `mode`/`timestampOffset` set, changeType | open | S: readyState "open"; Q: `sourceopen` — before the operation's own checks that may still throw (§ 5's quirk, kept) |
| open or ended | detach: the element's load algorithm, a new `src`, `removeAttribute('src')` + load, a failure that empties the element, page teardown (no events then) | closed | S: provider `fed_close`; S: readyState "closed"; S: duration NaN; S: activeSourceBuffers emptied, Q: `removesourcebuffer` at it; S: sourceBuffers emptied, Q: `removesourcebuffer` at it; Q: `sourceclose`. Each SourceBuffer becomes "removed" (every member that checks throws). Each SB with an APPEND in flight: Q `abort`, Q `updateend` at it FIRST (§ 9.1), then the lists' events and `sourceclose` as listed; a REMOVE in flight: cancelled, no events |

The element's own events at attach are the media element's as today for a
local resource (`loadstart` from the load algorithm; no `suspend`, MSE §
3.15.1's note), and `loadedmetadata` waits for HAVE_METADATA (table 5).

### 2.2 SourceBuffer.updating

| From | Trigger | To | Effects, in order |
|---|---|---|---|
| false | `appendBuffer(d)` passes prepare append | true | S: bytes copied into the parser's input (provider `take`); S: updating true; Q: `updatestart`; Q: the APPEND task |
| true | the APPEND task runs, parse DONE, no error | false | (inside the task) S: updating false; Q: `update`; Q: `updateend` |
| true | APPEND task: parse PENDING (only if the parse ever moves off-thread) | true | the turn polls (`fed_poll`); on DONE as the row above |
| true | APPEND task: parse ERROR | false | append error (§ 5.5.3): S: reset parser state (provider `reset`); S: updating false; Q: `error`; Q: `updateend`; then end of stream "decode" (table 2.3: S readyState "ended", Q `sourceended`, element error) |
| true | `abort()` while the APPEND task is queued | false | S: the task CANCELLED (it never runs); S: updating false; Q: `abort`; Q: `updateend`; then (always, updating or not) S: reset parser state; S: appendWindowStart 0, End +∞ |
| false | `remove(s, e)` passes | true | S: updating true; Q: `updatestart`; Q: the REMOVE task |
| true | REMOVE task runs | false | S: provider `remove`; element readyState may drop to HAVE_METADATA (§ 5.5.9 3.5); S: updating false; Q: `update`; Q: `updateend` |
| true | `abort()` during a REMOVE | true | throws InvalidStateError; nothing changes |
| true | `removeSourceBuffer(this)` | false | table 3.4 |
| true | detach | false | APPEND: Q `abort`, Q `updateend`; REMOVE: no events (table 3.4) |

"Running" for abort: the parse is a whole task, so a buffer append is never
part-way when script runs; abort's "abort the buffer append algorithm" is the
queued APPEND task cancelled. Its bytes are already in the parser's input, so
reset parser state's step 1 runs on them: complete coded frames of a media
segment being parsed are processed, the rest discarded (§ 5.5.2).

### 2.3 End of stream (§ 3.15.7)

| Error | Steps, in order |
|---|---|
| none | S: readyState "ended"; Q: `sourceended`; S: duration change to the highest end over all track buffers (Q: element `durationchange` if it changed, AFTER `sourceended`); S: provider `fed_end` (the session knows nothing more is coming: playing to the highest end is `ended`, not `waiting`) |
| "network" | S: "ended"; Q: `sourceended`; element at HAVE_NOTHING: the "cannot be fetched at all" steps (MediaError 4, `error` at the element); above it: "connection is interrupted" (MediaError 2, `error`) |
| "decode" | S: "ended"; Q: `sourceended`; element at HAVE_NOTHING: "unsupported format" (MediaError 4); above it: "media data is corrupted" (MediaError 3, `error`) |

### 2.4 Duration change (§ 3.15.6)

| Step | What libdom does |
|---|---|
| equal | nothing |
| new < highest buffered pts (any SB) | InvalidStateError (from the setter) |
| new < highest end time | raised to it |
| set | MediaSource duration; element duration; Q: element `durationchange`; S: provider `fed_duration` |

Init segment received with duration NaN: the init's duration, or +∞ if it has
none (the provider's result carries it).

## 3. The append and the removal, as libdom runs them

### 3.1 appendBuffer(data)

| # | Synchronous, in the call |
|---|---|
| 1 | removed? InvalidStateError. updating? InvalidStateError |
| 2 | the element has a MediaError? InvalidStateError |
| 3 | parent "ended"? S "open", Q `sourceopen` |
| 4 | coded frame eviction: provider `evict(sb, incoming = byteLength)`; answers FULL if still over the quota → QuotaExceededError, nothing taken (MS § 4) |
| 5 | provider `take(sb, bytes, len)`: the one copy, into the parser's input. A refusal for memory is QuotaExceededError |
| 4a | an append larger than the SB can ever hold (more than its quota with everything evicted) fails at 4 with QuotaExceededError (prepare append 6–7), nothing taken; an append whose boxes break the parser's limits (4 MiB a box, 128 MiB an `mdat`, media_fragment.h) is taken and the APPEND task's parse answers ERROR: append error, end of stream "decode" — never a crash of the task |
| 6 | updating true; Q `updatestart`; Q APPEND task |

| # | The APPEND task (window thread, a realm task like any media task) |
|---|---|
| 1 | provider `parse(sb, &result)`: the segment parser loop over the input, frames into the track buffers, under the fed media's lock |
| 2 | result INIT (one or more init segments seen): § 5.5.7 per init: duration NaN → duration change to the init's or +∞; the first init: activeSourceBuffers gains this SB (Q `addsourcebuffer` at it); every SB's first init done → element HAVE_NOTHING → HAVE_METADATA (Q `loadedmetadata` via the element's machinery, table 5) |
| 3 | result FRAMES: element readyState re-judged from the next facts (table 5); a media segment past the duration: duration change to max(duration, group end) |
| 4 | result ERROR (a rule of the byte stream broken, an init the codecs refuse, a second init changing track counts): append error (table 2.2) |
| 5 | result PENDING: keep updating; the turn polls |
| 6 | DONE: updating false, Q `update`, Q `updateend` |

### 3.2 abort() — table 2.2's rows, plus: `mode` and `timestampOffset` untouched.

### 3.3 remove(start, end) — checks of § 1; then S updating true, Q `updatestart`, Q REMOVE task (table 2.2). Ranges in seconds become ticks by nearest at the provider (MS § 4).

### 3.4 removeSourceBuffer(sb) and detach

| # | removeSourceBuffer (§ 3.8) |
|---|---|
| 1 | not in sourceBuffers → NotFoundError |
| 2 | updating: S the queued APPEND/REMOVE task cancelled; S updating false; Q `abort` at sb; Q `updateend` at sb (no `update`, no `error`) |
| 3 | (track lists: none in v1) |
| 4 | in activeSourceBuffers: removed, Q `removesourcebuffer` at activeSourceBuffers |
| 5 | removed from sourceBuffers, Q `removesourcebuffer` at sourceBuffers |
| 6 | S provider `fed_remove_buffer(sb)`: its parser, track buffers and held slabs freed (a frame the decoder holds keeps its slab until the decoder drops it, MS § 4) |

| Detach with an SB updating | v1's answer (the spec is silent; both browsers read, § 9.1) |
|---|---|
| an APPEND in flight | cancelled; updating false; Q `abort`, Q `updateend` at the SB, before the lists' `removesourcebuffer` and the `sourceclose` |
| a REMOVE in flight | cancelled; updating false; no events (Chromium's `CancelRemove`) |

## 4. The provider seam (libdom ↔ yonder), in media's shape

A new block in `dom/media.h`, `os64_dom_fed_provider_t`, beside the media
provider and installed with it. Every call is on the window thread, returns
at once, never enters script, and copies out what it answers. libdom never
holds a lock of the session's.

| Call (libdom → yonder) | Arguments | Answers | yonder does (MS.2's entry points) |
|---|---|---|---|
| `fed_open` | node | session or NULL (NULL: "cannot be fetched at all") | a fed session: a player over a `media_fed` with no SourceBuffer yet; `video_add` for a `<video>` as `media_open` does |
| `fed_add` | session, the type as given (MIME type and its parameters, one string isTypeSupported said yes to) | SB handle or NULL (QuotaExceededError) | a parser and its track buffers, quota by kind |
| `fed_remove_buffer` | session, sb | — | frees them (§ 3.4) |
| `evict` | session, sb, incoming bytes | true / false (FULL: QuotaExceededError, also for an append no buffer could ever hold) | MS § 4's eviction |
| `take` | session, sb, bytes, length | true / false (no memory) | copies into the parser's input |
| `parse` | session, sb, `out *` | DONE / PENDING / ERROR + INIT facts (duration, tracks of each kind) + group end + highest pts + the SB's `timestampOffset` as it stands after the append | the segment parser loop and coded frame processing |
| `fed_poll` | session, sb, `out *` | as `parse` | only when the parse runs elsewhere |
| `reset` | session, sb, `out *` | the result's offset, highest pts and parsing flag after it | reset parser state (§ 5.5.2) |
| `remove` | session, sb, start, end (seconds), `out *` | as `reset` | coded frame removal |
| `set` | session, sb, mode, timestampOffset, window start, window end | — | the SourceBuffer's values, applied at the next frame |
| `ranges` | session, sb, `out[]`, max, `*highest_end` | count (seconds, the round trip exact), and the largest end of any of the SB's tracks (the buffered getter's ended step, the duration's floor) | the SB's intersected ranges (MS § 4) |
| `fed_duration` | session, seconds | — | the player's end |
| `fed_end` | session | — | no more frames: the end of the highest range is `ended` |
| `fed_close` | session | — | detach: the session and every SB freed |
| (existing) `play`, `pause`, `seek`, `settings`, `facts` | session | as today | as today; `facts` fills current/future/enough from the ranges and the play position (MS § 5), the two playback-quality counters (`frames_shown`, `frames_dropped`), and `error` = `OS64_DOM_MEDIA_ERR_DECODE` when the decoder refuses a frame; `probed` and `duration` are not read |

| Kept by | What |
|---|---|
| libdom | the JS objects; readyState; duration; the lists and their order; `updating`, the queued APPEND/REMOVE tasks and their cancellation; mode/offset/window values (and pushes them with `set`); every event and its order; the element's states and its promises; the MediaSource URL registry; `buffered`/`seekable` composition |
| yonder | the fed session, its threads (the player's decoding and picture threads, the voice); per SB its parser input, track buffers, slabs and quota; the facts |

## 5. The element's states with a fed source

| State or event | Comes from | Notes |
|---|---|---|
| HAVE_METADATA, `loadedmetadata`, `durationchange`, `resize` | every SB's first init (§ 5.5.7 10), as libdom saw them (`d_mse_probed`) | `videoWidth`/`Height` from the init's picture size |
| a new init with the element above HAVE_CURRENT_DATA | § 5.5.7 9: back to HAVE_METADATA | the facts' current flag drops until a frame is decoded |
| HAVE_CURRENT/FUTURE/ENOUGH, `loadeddata`, `canplay`, `canplaythrough`, `playing` | `facts.current/future/enough` (the session judges the ranges against the position, MS § 5); libdom's existing `ready_to` | thresholds in yonder, reported |
| `waiting` | the play position leaving the buffered range (facts drop to HAVE_METADATA or CURRENT while playing) | existing `ready_to` |
| seeking into a gap | `seek` as today; the session holds `seeked` until frames arrive there; § 3.15.3 drops readyState to HAVE_METADATA | `seeking` S as today; `seeked` when `facts.seeked` matches |
| `buffered` / `seekable` | § 1's table, from `ranges` | `DTimeRanges` grows to N ranges (decision 6) |
| `duration`, `durationchange` | MediaSource (table 2.4) | `fold_facts`' duration path is skipped for a fed record |
| `ended` | after `fed_end`, the position reaching the highest end (`facts.ended`) | as today: `timeupdate`, `pause`, `ended` |
| decode error during play (a frame refused long after its append) | `facts.error` = `OS64_DOM_MEDIA_ERR_DECODE` (the existing fact and code: the decoder's error, not the network's) | libdom runs the end of stream algorithm with "decode" for an attached fed element (MediaSource "ended", Q `sourceended`, then the element's media-data-corrupted path: MediaError 3, `error`); afterwards prepare append throws InvalidStateError (recent element error) |
| `getVideoPlaybackQuality()` | facts' shown and dropped counters since the load | every video |

## 6. What libdom needs inside (each a function of `media.c` or a new `mse.c`)

| Need | Where |
|---|---|
| MediaSource / SourceBuffer / SourceBufferList classes, handler attributes | new `libdom/mse.c` (TextTrackList's no-constructor pattern; FileReader's constructible one) |
| a media task with a non-node target, and an internal task (APPEND, REMOVE) that can be cancelled | `media.c`'s `DMediaTask` widened |
| the fed record: the element record points at its MediaSource (a strong reference while attached: decision for § 8) | `DMediaElement` |
| `select_resource` resolving a `blob:` URL in the MediaSource registry before `provider.open` | `media.c` |
| the MediaSource URL registry | `blob_url.c` beside the Blob store; `createObjectURL` accepts a MediaSource; `revokeObjectURL` removes from either |
| N-range TimeRanges, intersection and union | `media.c` |
| VideoPlaybackQuality class | `video.c` |

## 7. isTypeSupported (and addSourceBuffer's type check)

Parse: `type/subtype` (case-insensitive) then `;`-separated parameters
(`name=value`, value quoted or bare, names case-insensitive). Only `codecs`
is read (§ 9.4c). A string that does not parse as a MIME type → false.

| Asked | Answer |
|---|---|
| empty, or not a valid MIME type | false |
| type/subtype not `video/mp4` or `audio/mp4` | false (WebM, MP2T, `audio/mpeg`, `audio/aac`: not v1) |
| `video/mp4` or `audio/mp4` with no `codecs` (or an empty one) | false (§ 9.4a) |
| each codec in `codecs` must be one of: | |
| — `avc1.PPCCLL` / `avc3.PPCCLL` (six hex digits), PP ∈ {42 Baseline, 4D Main, 64 High}, LL ≤ 0x33 (level 5.1) | true; false for 6E (High 10), 7A, F4, 58 (Extended) and any level above 5.1 (the decoder takes them; the P5's picture path is 8-bit 4:2:0, and 1080p60 is level 4.2) |
| — a bare `avc1` / `avc3` | false (§ 9.4b) |
| — `mp4a.40.2` (AAC-LC) | true |
| — `mp4a.40.5`, `mp4a.40.29` (HE-AAC) | false until MS.3.2 proves them (§ 9.3) |
| — anything else (`av01`, `vp09`, `vp8`, `opus`, `mp4a.69`, `ec-3`, `av99`…) | false |
| `audio/mp4` with a video codec | false |
| `video/mp4` with only an audio codec, or a video and an audio codec (muxed) | true |
| more than one video or more than one audio codec in one type | false (v1: one track of a kind per SourceBuffer) |
| any parameter other than `codecs` | ignored (§ 9.4c) |

`canPlayType` (the element's) is not changed by MS.3.1; it stays the media
provider's answer for a file.

## 8. Lifetime

| Case | What happens |
|---|---|
| a MediaSource attached and no script reference | the element record holds it (a strong reference held from C, as the record's other values are) until detach; the registry entry holds it too while not revoked |
| the URL revoked while attached | the attachment goes on (FileAPI: a request already started still succeeds; MSE says nothing more); a later `src =` that URL fails as a revoked blob URL (network error) |
| the URL assigned to a second element while attached | that element: "cannot be fetched at all" (readyState not closed) |
| detached, then the URL assigned again (not revoked) | attaches again (closed); `[[has ever been attached]]` only bars a handle, § 3.15.1 1.3 |
| a SourceBuffer kept by script after removeSourceBuffer or detach | its JS object lives; it is "removed": every checking member throws InvalidStateError, `buffered` throws; its provider handle is gone (freed at removal) |
| page teardown (navigation, window close) with an append queued | the media FIFO is freed without running; `fed_close` frees the session; no events; the registry is freed with the scripts host (Blob store's rule) |
| the MediaSource collected while closed | its registry entry (if any) keeps it until revoke or teardown, as a Blob's |
| a MediaSource, SourceBuffer or SourceBufferList with an event or an APPEND/REMOVE task queued, every script reference dropped | the queued task holds its target (a strong reference held from C) until it has run or been cancelled; the event fires on it. Cases (`dom_mse_lifetime_cases`): a MediaSource held only by its revoked URL hears its `sourceclose` after a detach and a collection; a SourceBuffer removed with its append queued, every reference dropped and collected, hears `abort` and `updateend` |

## 9. The questions, answered from source (2026-11-27, MS.3.1)

Read from Chromium (raw GitHub, `main`) and Firefox (gecko-dev `master`).
Fable's rule: follow the two if they agree, Chromium if they differ.

| # | Question | Chromium | Firefox | v1 |
|---|---|---|---|---|
| 1 | detach with an append in flight | `SourceBuffer::RemovedFromMediaSource` (source_buffer.cc): a pending remove → `CancelRemove()` (updating false, NO events); else `AbortIfUpdating()`: updating false, schedules `abort` then `updateend`. Called per SourceBuffer from `MediaSource::OnReadyStateChange` on close, before `sourceclose` | `SourceBuffer::Detach` → `AbortBufferAppend` → `AbortUpdating`: updating false, queues `abort` then `updateend` | an APPEND in flight: Q `abort`, Q `updateend` at the SB, before the lists' `removesourcebuffer` and `sourceclose`; a REMOVE in flight: cancelled, no events (Chromium's; Firefox's remove not read) |
| 2 | revoke while attached | `PublicURLManager::Revoke` unregisters the URL; the attachment is dropped only on close (`media_source_attachment_.reset()`) | `RevokeObjectURL` removes the data entry; `MediaSource::Attach/Detach` never touch the URL | revoke removes the registry entry only; the attachment goes on |
| 3 | HE-AAC (`mp4a.40.5`/`.29`) | — | — | FFmpeg's `aac` decoder carries SBR and PS within it, so the built decoder should take them; still false in MS.3.1, and turned on in MS.3.2 once a stream proves it plays |
| 4a | `video/mp4` with no `codecs` | `CheckTypeAndCodecs` (stream_parser_factory.cc): kMaybeSupported, and `isTypeSupported` is true only for kSupported → **false**; `addSourceBuffer` throws NotSupportedError | not rejected in MediaSource.cpp (DecoderTraits not read) | **false** (Chromium) |
| 4b | a bare `avc1` | patterns `avc1.*`/`avc3.*`: a bare `avc1` matches neither → **false** | delegated, not read | **false** (Chromium) |
| 4c | parameters other than `codecs` | **ignored**: only the type and `codecs` are read | `width`/`height`/`framerate`/`bitrate` parsed, a missing or odd value becomes −1, **nothing rejected** | **ignored** (both agree) |

**4c reverses § 7's range checks.** `width=99999`, `channels=99` and
`eotf=catavision` answer as their codecs do, as in both browsers. The
triage's "a player that is told yes to everything picks AV1 and Opus" is about
codecs (`av01`, `av99`), and those still answer no; the parameters were
never the test that browsers pass.

## 10. Proof

| Where | Cases |
|---|---|
| DOM host (`tools/test_dom_host.c`), a FAKE fed provider (scripted results: INIT, FRAMES, ERROR, PENDING, FULL; a ranges table) | every row of tables 2.1–2.4, 3.1–3.4 with its events IN ORDER (an event log); every exception of § 1 in the spec's order (a closed source with an unsupported type → NotSupported); the ended→open quirk on a throwing setter; abort cancelling a queued APPEND; removeSourceBuffer while updating; detach with an SB updating; isTypeSupported's whole table, absurd pairs included; buffered/seekable composition (N ranges, ended's extension, +∞ with a live range); lifetime (removed SB throws; revoke while attached; second element refused; teardown with a queued append frees everything under ASan, no event) |
| DOM mutants | one per row of § 2's tables and § 7's table |
| scripted host (yonder) | the provider wired; `select_resource` attaching; `getVideoPlaybackQuality` |
| pages (`tools/pages/mse-*.html`), local fragmented files fed by `fetch` and by ranges, no site's bytes | two SourceBuffers in step; a quality switch; a seek into a gap; `remove` behind the position; `endOfStream` and `ended`; an append error |

**Not testable until MS.2 exists:** real parsing and coded frames, the ranges
from real frames, readyState from a playing fed session, eviction under a
real quota, decode errors, and every page in QEMU. Until then the fake
provider is the seam's contract, and MS.2's yonder glue is tested against the
same table in its own PR.

**DEBTS rows this slice adds:** AudioTrack/VideoTrack lists (the element's and
the SourceBuffer's); `changeType` (MS.5); worker MSE and `MediaSourceHandle`;
`ManagedMediaSource`; HE-AAC's no.
