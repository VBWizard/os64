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
| **yonder cannot say it is another browser.** Sites answer by User-Agent (theoldnet, below, is the first seen), and a person comparing yonder with Chrome needs to be able to ask as Chrome asks. The agent is `YONDER_AGENT` in yonder.c, handed to libway (`session->agent`) and on to every fetch, pages and pictures alike. | 2026-09-26, Chris, on seeing theoldnet answer yonder and Chrome differently: "we're going to have to be able to send Chrome's user agent — that way I can at least SEE if it works." | Opus (yonder) | An `agent =` line in `yonder.conf` on the configuration ladder, read at start, the built-in `yonder/1.0 (os64)` when the line is absent — so it is set where every other setting is and survives a rebuild in /home. Said on the status line while it is in force, so a page answering an assumed identity is never mistaken for one answering yonder. Per-site agents only if one site needs it. |
| **A window that wears no face draws a widget's non-ASCII text one byte per cell.** libui's fallback when no UI face is bound is the 8x16 bitmap cell, measured and drawn a byte at a time (`ui_font.c`), so a UTF-8 `é` in a yonder form field draws as two CP437 glyphs (`caf┬©`). What is SENT is right — httpbin echoed `café` — only the glass is wrong, and only in a window with no face. | 2026-09-26, Opus, in the guest: a form field holding `café`. | libui | Decode UTF-8 in the bitmap path, drawing each character the cell has and a marker for one it lacks, as the console's charset table does. |

## Explicitly not debts

| What was seen | Why it is right |
|---|---|
| On `https://theoldnet.com/`, the Windows 95 picture takes yonder to `theoldnet.com/get?year=1996&...&url=http://windows95.com` and Chrome to `web.archive.org/web/19961023.../http://www2.windows95.com/`. | theoldnet answers by User-Agent (checked 2026-09-26 with both): a browser it does not recognise as modern is served as a VINTAGE browser — the archived 1996 page through theoldnet itself, every link rewritten through `/get?...&timestamp=...` so an old browser can walk 1996 without https, pictures from `web.archive.org` — while Chrome gets a 302 to the Wayback Machine. Same 1996 page either way; yonder's is the trip a 1996 browser would have had. Revisit only if a site misjudges yonder in a way that costs a person something. |
