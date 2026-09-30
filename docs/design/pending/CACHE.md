# CACHE.md — yonder keeps what pages fetch

*Written 2026-09-30 by Opus, at Chris's asking: "what would really help
with testing is a cache." Measured on the P5 the same morning: danlegt.com
fetches 22 style sheets in 10–15 seconds and 82 pictures in about a
minute, and every Reload fetches them all again. The rules are RFC 9111's
(HTTP Caching), the parts a private browser cache uses; Chrome is the
yardstick where the RFC leaves a choice.*

## What a person sees

The second time a page is opened — after a Reload, Back, a new window, a
reboot, a rebuild — its pictures and style sheets come off the disk
instead of the network, when the server said they may. danlegt.com says
it of every one: `Cache-Control: public, max-age=31536000, immutable`, a
year. The page itself is still fetched each time; it is one request, and
whether it changed is the thing a person reloads to see.

yonder's Settings window gains a box to turn the cache off for this
window (Save as default makes that stick) and a button that empties it,
and its status line says what the cache holds when it opens.

## Where it lives

`/var/cache/yonder/` on the root filesystem, one file per response. Root
is the P5's large disk, and both disks persist there. The one thing that
wiped root was `tools/p5-refresh.sh`, which mirrors the build with `rsync
--delete`; it now excludes `/var/cache/`, which rsync's `--delete` leaves
alone. A cache is the system's own disposable state, so it is the
exception the persistence doctrine allows (CLAUDE.md says so where the
doctrine is written). In QEMU a rebuilt image starts it empty.

`yonder.conf` may say `cache = off`, `cache_dir = <path>` and
`cache_mb = <n>` (default 256).

## How it enters the code

**libfetch** reports what the final reply said about keeping it:
`os64_fetch_head_t.keep` holds `Cache-Control`, `Pragma`, `ETag`,
`Last-Modified`, `Expires`, `Date`, `Age` and `Vary` as written (a
repeated field joined by `, `), and `unreadable` when one was longer than
its field holds — a reply the cache cannot read whole is not kept. The
library stays free of caching: POLICY ABOUT THIS MACHINE IS THE CALLER'S
(fetch.h), and a conditional request is the caller's `If-None-Match` or
`If-Modified-Since` in `extra_headers`, which fetch.c already turns into a
304 with no body.

**libway** gets `way_cache_t` (`way/cache.h`), made once per browser like
the jar and shared by every fetch on any thread, and `way_fetch_whole`: a
GET read whole into memory, through the cache when `way_hooks_t.cache` is
set. It is what yonder's picture and style-sheet jobs call instead of
reading libfetch themselves. The rules are pure functions over the stored
fields and a clock, so the host harness drives them with no network:

1. **What is kept**: a plain GET (no headers of the caller's own, which
   could change the answer) whose reply is 200, read whole, with no
   `no-store`, no `Vary` but `Accept-Encoding`, every field readable, and
   either no redirect or only permanent ones (301, 308). A `no-cache`
   reply is kept, and asked about every time it is used. It is kept under
   the address asked for, holding the address it came from, so a sheet's
   relative `url()`s still resolve against the right place. An entry
   larger than an eighth of the cap is not kept.
2. **When it is fresh** (§ 4.2): its lifetime is `max-age`, else `Expires`
   less `Date`, else a tenth of `Date` less `Last-Modified`, at most a
   week (the heuristic of § 4.2.2, with a bound of our own). Its age is
   what the reply said (`Age`, and how late its `Date` was against this
   machine's clock when it was stored) plus how long it has been kept.
   Fresh is a lifetime longer than the age, and no `no-cache` (or `Pragma:
   no-cache` without a `Cache-Control`).
3. **Fresh**: served from the disk, and nothing goes out.
4. **Stale, with a validator**: asked again with `If-None-Match` (the
   ETag) or `If-Modified-Since` (the Last-Modified). A 304 serves the
   stored body and rewrites the entry with the 304's fields, which is what
   restarts its freshness. Anything else is a new reply, kept or not by
   rule 1.
5. **Stale, without one**: fetched as if it were not there.
6. **The network down**: a stale entry is served when the fetch cannot
   reach the server at all (no dial, or silence before a head), unless it
   said `must-revalidate` or `no-cache` (§ 4.2.4). A TLS failure is never
   one of these: it may be somebody in the way.

An entry is a text head — the fields above, the status, the type and
charset, the address asked for and the one it came from, when it was
stored — a blank line, and the body. It is written under a name no other
writer has (`<hash>.<task>.<n>.new`) and renamed over its place, so a
reader sees the old entry or the new one and two yonder windows share the
directory safely. Its name is a 64-bit hash of the address asked for, and
the address inside is checked, so a collision is a miss.

**When it is full** the oldest entries by their file's time — stored, or
revalidated — are removed until it is under nine-tenths of the cap. That
is a little blunter than least-recently-used (a hit does not rewrite the
file), and needs no index that two windows would have to agree on.

**yonder** opens the cache at start (after `yonder.conf`), hands it to
every picture and sheet job through their hooks, and closes it at exit.

**Reload** refetches the page and follows the rules for everything else
— Chrome's behaviour since 2017. Revalidating all 82 pictures would cost
82 round trips with a TLS handshake each for "not modified"; the Clear
button is the way to a truly fresh load.

## Decisions

1. **On disk, on root.** Memory only would help Back and Reload and be
   empty after every restart, rebuild and reboot — the testing loop this
   exists for.
2. **Pictures and style sheets, not pages.** A page is one fetch, and the
   one a person reloads to see change; its body streams into the parser
   rather than being read whole. Booked.
3. **Serve stale when the network is down.** § 4.2.4 allows it, and on
   the P5 it is the difference between a page with its pictures and one
   without while the link is gone.
4. **The oldest-stored go first** (above).

## Booked, with their triggers

| Debt | Why it waits | Trigger |
|---|---|---|
| Pages themselves | one fetch each, streamed into the parser | Back and Forward on a slow link want the page too |
| `Vary` beyond `Accept-Encoding` | the key would have to hold the request's headers | a site whose pictures vary by something else and read wrong |
| Redirects other than 301 and 308 | a 302 is not the server's word that it will always answer so | a site whose pictures sit behind temporary redirects |
| Decoded pictures between pages | the download is kept, the decode is repeated (YONDER.md § Booked's picture cache) | decoding shows in the time to a laid-out page |
| wend | it shares libway, and turns the cache on in a line when wanted | somebody reading the same pages in wend |
| Least-recently-used eviction | a hit would have to rewrite its file's time | a full cache evicting what is in use |
| `Range`, partial and resumed entries | a body is kept whole or not at all | a large download that fails near its end |

## Proof

- libfetch's scripted-peer harness: the fields reported on a final head,
  joined when repeated, cleared across a redirect, `unreadable` when one
  overflows; a 304 answered to `If-None-Match`.
- libway's host harness: every rule above as a pure function (what is
  kept, lifetime, age, fresh, the conditional headers, the 304 merge,
  serving stale), and the store itself against a temporary directory —
  write, read back, a collision, a torn temp file ignored, eviction.
- The guest: danlegt.com in yonder twice; the second load's pictures and
  sheets come off the disk (the status line and `ls /var/cache/yonder`);
  Settings shows the count, Clear empties it, the next load fetches again.
