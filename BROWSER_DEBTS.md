# BROWSER_DEBTS.md — what the browsers owe, found by using them

DEBTS.md's shape, for the browser campaign: wend, yonder, and the
libraries under them (libhtml, libpage, libflow, libway, libfetch,
libimage). A row is something a person hit on a real page, or a known gap
with its trigger. What yonder's own design deliberately leaves for later
is booked in its constitution (`docs/design/pending/YONDER.md` § Booked);
this file is for what USE finds.

Each row names who owns the fix, because the browsers sit on libraries
with different owners and a finding in one is not a licence to edit
another.

## Open

| Debt | Found | Owner | What would pay it |
|---|---|---|---|
| **A GIF ending in a graphic control extension with no picture after it is refused whole.** `userland/libimage/gif.c:102` answers MALFORMED at the trailer while a GCE is pending (`!pending_control`). Old GIF tools wrote a timing block for a frame that never came, so the web has these; Chrome and PIL ignore the dangling block, since a timing block with no frame after it controls nothing. yonder draws such a picture as its grey frame. | 2026-09-26, Chris: `https://theoldnet.com/images/bullet02.gif` (14x14, seven frames, a local palette each; the eighth GCE sits before a comment and the trailer), on `https://theoldnet.com/`. Diagnosed with libimage's own GIF host harness: "malformed image". | Quinn (libimage) | Accept a pending GCE at the trailer — a stray timing block controls nothing — and add this file's shape as a fixture to `tools/test_gif_host.py`: seven frames and a dangling GCE, decoding to its first frame. Whether the sequence decoder should do the same is the owner's call; Chrome plays the seven. |
| **yonder shows a GIF's first frame, never its animation.** libimage decodes sequences (GIF_ANIMATION.md); what yonder lacks is a frame clock — its window wakes only on events. Booked in YONDER.md; the shape: a ticker thread rings the window's doorbell at the next frame's deadline, the window advances only the animations on screen and repaints only their boxes, a GIF's loop count is honoured (why Chrome plays some only once), and nothing ticks when nothing animates. | 2026-09-26, Chris, on `https://theoldnet.com/`: `images/guestbook4.gif` (8 frames), `images/gc_icon.gif` (5), `images/angelfire.gif` (16), `images/bullet02.gif` (7, after the row above). | Opus (yonder) | A yonder slice of its own (Y5b), placed in the order when Chris says. |
| **yonder cannot say it is another browser.** Sites answer by User-Agent (theoldnet, below, is the first seen), and a person comparing yonder with Chrome needs to be able to ask as Chrome asks. The agent is `YONDER_AGENT` in yonder.c, handed to libway (`session->agent`) and on to every fetch, pages and pictures alike. | 2026-09-26, Chris, on seeing theoldnet answer yonder and Chrome differently: "we're going to have to be able to send Chrome's user agent — that way I can at least SEE if it works." | Opus (yonder) | An `agent =` line in `yonder.conf` on the configuration ladder, read at start, the built-in `yonder/1.0 (os64)` when the line is absent — so it is set where every other setting is and survives a rebuild in /home. Said on the status line while it is in force, so a page answering an assumed identity is never mistaken for one answering yonder. Per-site agents only if one site needs it. |

## Explicitly not debts

| What was seen | Why it is right |
|---|---|
| On `https://theoldnet.com/`, the Windows 95 picture takes yonder to `theoldnet.com/get?year=1996&...&url=http://windows95.com` and Chrome to `web.archive.org/web/19961023.../http://www2.windows95.com/`. | theoldnet answers by User-Agent (checked 2026-09-26 with both): a browser it does not recognise as modern is served as a VINTAGE browser — the archived 1996 page through theoldnet itself, every link rewritten through `/get?...&timestamp=...` so an old browser can walk 1996 without https, pictures from `web.archive.org` — while Chrome gets a 302 to the Wayback Machine. Same 1996 page either way; yonder's is the trip a 1996 browser would have had. Revisit only if a site misjudges yonder in a way that costs a person something. |
