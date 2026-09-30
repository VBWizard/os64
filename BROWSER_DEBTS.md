# BROWSER_DEBTS.md — what the browsers owe, found by using them

DEBTS.md's shape, for the browser campaign: wend, yonder, and the
libraries under them (libhtml, libpage, libflow, libway, libfetch,
libimage). A row is something a person hit on a real page, or a known gap
with its trigger. What yonder's own design deliberately leaves for later
is booked in its constitution (`docs/design/completed/YONDER.md` § Booked);
this file is for what USE finds.

Each row names who owns the fix, because the browsers sit on libraries
with different owners and a finding in one is not a licence to edit
another.

## Open

| Debt | Found | Owner | What would pay it |
|---|---|---|---|
| **A window that wears no face draws a widget's non-ASCII text one byte per cell.** libui's fallback when no UI face is bound is the 8x16 bitmap cell, measured and drawn a byte at a time (`ui_font.c`), so a UTF-8 `é` in a yonder form field draws as two CP437 glyphs (`caf┬©`). What is SENT is right — httpbin echoed `café` — only the glass is wrong, and only in a window with no face. | 2026-09-26, Opus, in the guest: a form field holding `café`. | libui | Decode UTF-8 in the bitmap path, drawing each character the cell has and a marker for one it lacks, as the console's charset table does. |
| **A hop's Cookie and Referer lines get 1024 bytes between them.** libfetch hands `headers_for` a buffer of `OS64_FETCH_EXTRA_MAX` (`fetch.c`, `request_extras`). A site's cookies pass that easily: a news site's analytics alone can, and Chrome sends up to 4096 bytes a cookie. libway fills it with the cookies first, whole, and leaves the rest off, so a login still works when its cookie is among the first by path and age; when it is not, the site sees a stranger. | 2026-09-26, Opus, sizing yonder's jar (YONDER.md § Y3b). | Quinn (libfetch) | A `headers_for` buffer sized for the header a server will accept (8 KiB is where servers start refusing a request head), or the request built with the callback writing into the request itself. |
| **libgarb's numbers are not correctly rounded, and at the ends of a double's range that changes what they are.** The tokenizer scales a 19-digit mantissa by powers of ten it multiplies out (`consume_number`, `tokenize.c`), which lands within a few units in the last place: `1.7976931348623157e308` comes out Infinity where it rounds to DBL_MAX, `2.4703282292062328e-324` zero where it rounds to the smallest subnormal. The text as written is kept beside the value, so a serializer loses nothing; what reads the value sees the difference only in numbers no real stylesheet writes, and libflow saturates lengths far inside that range anyway. | 2026-09-28, Codex on the parser's review, past the dump's range check. | Opus (libgarb) | One correctly rounded decimal-to-binary64 for the browsers: libpage already carries one (musl's `decfloat`, `libpage/number.c`); hoisted where libgarb can link it, the tokenizer hands it the digits it read. |
| **A block-level picture that failed shows nothing where its alt text should be.** A page's sheet can make `img` a block (`img { display: block }` is in the common resets — Tailwind's preflight among them), and a block-level replaced box is laid out by its size alone (`layout.c`'s `block`, `FB_REPLACED`): `replaced_size` answers "lay out the alt text" for a picture that is not known and has one, which the INLINE path does (`text_segments`), but the block path has no line to put text on and lays out 0 by 0. A picture still arriving is 0 by 0 in the browsers too, and one that arrives, or whose size the page gave, is laid out right; the difference is only a picture that FAILED and has alt text — Chrome shows the alt there, yonder shows nothing. | 2026-09-29, Fable's hand-off on the cascade slice; confirmed on the flow slice, where the cascade first makes `img` block-level. | Opus (libflow) | A block-level replaced box that is laid out as its alt text gets a line of its own: the alt text shaped as an inline would be, the box as wide as that line (so `margin: 0 auto` still centres it) and one line high, with TEXT boxes under it that `flow_image` names as the picture's placement, as it does for an inline alt. Its own slice: the box tree has no block with an inline run it did not take from the document. |

## Explicitly not debts

| What was seen | Why it is right |
|---|---|
| On `https://theoldnet.com/`, the Windows 95 picture takes yonder to `theoldnet.com/get?year=1996&...&url=http://windows95.com` and Chrome to `web.archive.org/web/19961023.../http://www2.windows95.com/`. | theoldnet answers by User-Agent (checked 2026-09-26 with both): a browser it does not recognise as modern is served as a VINTAGE browser — the archived 1996 page through theoldnet itself, every link rewritten through `/get?...&timestamp=...` so an old browser can walk 1996 without https, pictures from `web.archive.org` — while Chrome gets a 302 to the Wayback Machine. Same 1996 page either way; yonder's is the trip a 1996 browser would have had. Revisit only if a site misjudges yonder in a way that costs a person something. |
