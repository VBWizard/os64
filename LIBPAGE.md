# LIBPAGE.md — a tree in, what the page MEANS out

*Written 2026-09-12 by Fable, for Opus to build, after the seven review
rounds of PR #98 (`wend`) and Chris's rulings of the same morning. The
name `libpage` is a placeholder until Chris settles it; nothing below
depends on the name.*

## The ruling

Three rulings, all Chris's, all 2026-09-12:

1. **This layer is built to the standard, whole, before a page asks.** The
   campaign's stance — implement a thing when something asks for one — stays
   for FEATURES (frames, table alignment, images in a text face). It never
   licensed a half-rule, and every P1 on PR #98 was a half-rule: a
   submitter's method honoured for `input` and not `button`, a disabled
   `fieldset` honoured on the visible walk and not the hidden one. The
   precedent is libhtml: built to the WHATWG algorithm with a pinned suite,
   and parsing was not the hard part BECAUSE of that. What a parsed page
   means gets the same treatment. The alternative is what the first weeks of
   the graphical browser would otherwise be: several stop-and-build-
   conformance rounds a day.
2. **JavaScript comes after the initial daily-driver graphical browser is
   done.** Not never. So nothing here may assume the tree never changes: the
   page model is DERIVED from the tree and can be rebuilt from it, and a
   person's edits are kept apart from the model so a rebuild does not lose
   them. A mutable DOM is the engine's problem on the day JS arrives; this
   library must merely not make that day worse.
3. **PR #98 takes this as a stacked PR**, both merging together. `wend`
   stops taking instance fixes; the three findings Opus predicted after round
   seven (`method=dialog`, `formmethod=dialog`, a disabled default button)
   are fixed here, at the one door, and nowhere else.

The design test for everything below: **where is the single place this rule
is decided?** Six sites computed "which submitter applies and what does it
impose" across two files of `wend`; nine P1s came through those six doors.
A rule with two implementation sites grows a third the next time the
surface grows. That is the whole diagnosis, and it is why this is a library
and not a tidier `render.c`.

## Where it sits

```
  face (wend today; the graphical browser later)   draws, edits, asks, navigates
  ─────────────────────────────────────────────── the seam ───────────────────
  libpage                                          what the page offers, and what
                                                   leaves the machine when it is used
  libhtml (the tree)   libos64 url (resolution)    below it
  libfetch                                         carries what libpage produces
```

- **Above the seam** is everything a face does with its own hands: draw the
  page in cells or pixels, take keystrokes, keep a history, ask a person a
  question. A face never decides what a control's value is, which form a
  control belongs to, or what a submission sends. It asks.
- **Below the seam**, libpage answers from the tree. It holds no history, no
  terminal, no window, and never asks a person anything: it states FACTS
  ("this request leaves an encrypted page for a plain one") and the face
  decides what to do with them. The confirm that saved passwords in three
  rounds stays a face concern; the fact it needs is libpage's.
- libhtml's tree is read-only and owned by its document; libpage keeps
  POINTERS into it and never copies text it can point at. libos64's URL
  parser (`os64/url.h`) is the only URL code; libpage calls it and never
  re-implements a byte of it.
- libfetch carries what libpage builds. Today it sends no request body, so a
  POST is a REFUSAL BY NAME at the face; the body is still built and the
  corpus checks its bytes, so the day libfetch grows a body (a Fable-tier
  slice, LIBFETCH.md) nothing here changes.

Both faces link it. That is the point: the graphical browser inherits every
rule below byte for byte, because a form is submitted identically whether it
is drawn in cells or in pixels.

## What goes in

- **A document**: libhtml's `os64_html_document_t`, complete or refused
  (a refusal still yields a tree; libpage builds what there is and reports
  `incomplete`). Its `charset` is the document's encoding, which the
  entry-list encoding falls back to.
- **The document's URL**: where the page CAME FROM — libfetch's final `url`
  after redirects, with a scheme-default port handed over as 0 the way
  `os64/url.h` asks. This is the address that decides whether a request is
  a downgrade, and it is NOT the base URL: a page can move its base to
  `http://` with `<base href>` while the page that collected the values
  stays encrypted (round 2). Two addresses, two jobs, never conflated.
- **Edits**, later: what a person typed into a box, ticked, or chose. These
  are the standard's "dirty value" and they live in libpage, keyed by the
  control, because "an untouched value goes out as the page's own bytes,
  only a change is stored" (round 1) is a rule about what leaves the
  machine, not about drawing.

## What comes out

**The page model**, built by one walk of the tree, in tree order, every item
carrying the `os64_html_node_t *` it came from so a face can find its own
geometry for it. Faces render nodes; libpage never learns about rows,
cells or pixels.

- **Links**: every `a` and `area` with an `href`, resolved against the base;
  whether the reference asked for a fragment at all (the text cannot tell
  `href="#"` from `href=""`); whether it names THIS document (a move, not a
  fetch).
- **Anchors**: every `id` and every old-style `<a name>`, the names a
  fragment is matched against, with the match made on the DECODED fragment
  (round 7: a heading with a space in its name).
- **Forms**: every `form`, with its `id`, its resolved action (or the
  refusal: unresolvable, too long), its method, its enctype, its
  `accept-charset`, its `novalidate`, and its listed controls.
- **Controls**: every submittable element — `input` of every type, `button`,
  `select` with its `option`s and `optgroup`s, `textarea` — with its form
  owner, its name, its value as the standard defines it after sanitization,
  its disabled state (own attribute, or a disabled `fieldset` ancestor
  outside that fieldset's first `legend`), its `readonly`, whether it sits
  under a `hidden` attribute (drawn nowhere, still submitted), and for
  buttons which submitter overrides it carries.
- **A declared refresh**: the `meta` pragma that asks to send a reader
  somewhere with no redirect and no script, with its delay, its resolved
  address, and whether that address is the page it was declared on.
- **The base**: the first `base` with an `href`, wherever libhtml kept it —
  the body too, since libhtml leaves a misplaced one where it found it
  (round 7) — an EMPTY href included, because `<base href="">` resolves to
  the document URL and outranks a later base (round 5).

**Requests**, from the one door. Activating something yields exactly one of:

- `NAVIGATE` — a URL (with the fragment the reference or action asked for
  carried separately, since an address cannot hold it), a method, an
  enctype, and for POST a body; plus the FACTS a face must know before it
  sends: `downgrade` (the document is `https` and this request is not),
  `leaves_machine` (an http/https navigation, as opposed to a `mailto:` or
  `data:` the face handles some other way).
- `FRAGMENT` — a move inside this document to a named anchor, or to the
  top for `#` and for `#top` when nothing claims the name. No fetch.
- `NOTHING` — the standard says a browser does nothing: a `dialog` method
  (the form closes a dialog and sends nothing to any server), a disabled
  default button on implicit submission, an implicit submission blocked by
  two or more text-like fields when the form has no submit button, a
  disabled control activated.
- `REFUSED`, by name — an action that does not resolve, a URL longer than
  an address may be, a scheme this browser does not navigate. A refusal is a
  verdict, not a fallback: an action too long to resolve was once sent to
  the page's own host instead, which is a different host to be wrong about
  (round 7).

The face gets one enum and one struct, and the wire never sees anything the
door did not produce.

## The rules, by family

Each family names its section of the standard, the single function that
decides it, and the finding that taught it. The corpus (below) is organised
by these families, and coverage is counted per family.

**A. The base and reference resolution** (§4.2.3 `base`, URL §5). One
door: `page_resolve(page, ref, out, &asked_fragment)`. First `base` with an
`href` wins, empty included; a relative reference resolves against it; an
EMPTY reference names the document itself (every reload link on the old
web, round 3); a scheme-default port is never spelled into a link (the
`:443` finding); a reference that does not resolve or does not fit is a
refusal, never a fallback. Fragment-only references never fetch.

**B. Form owner** (§4.10.17.3). One door: `page_form_owner(page, control)`,
settled ONCE for every control after the walk and before anything asks, so
a question during rendering is answerable (round 7's structural miss). The
`form=` attribute names a form by `id` anywhere in the document; a name that
matches no form leaves the control in NO form, not the nearest one (round
4); otherwise libhtml's `form_owner` insertion record takes precedence over
nearest `form` ancestry. Table parsing can leave a form empty while controls
in its table rows belong to it. The B7 corpus cases cover explicit and
implicit submission through that parser association; `form=` still wins.

**C. The submitter** (§4.10.21.2, §4.10.21.3 implicit submission). One
door: `page_submitter(page, form, activated)` → the submitter element or
none, and `page_submitter_overrides(submitter)` → the effective action,
method, enctype, novalidate. Both `input type=submit|image` and `button`
(whose default type IS submit) are submitters, through the same code — the
crash on Wikipedia's Search was this rule finished for one element type
(round 3). Explicit activation names the submitter. Implicit submission
(Enter in a box) fires the form's DEFAULT BUTTON: the first submit control
in tree order whose owner is that form, wherever it stands — inside a
`hidden` subtree (round 6), out of reach of any cursor, bound by `form=`
from document scope (round 7). If that button is disabled, NOTHING. If the
form has no submit control at all: NOTHING when more than one field blocks
implicit submission (an `input` whose type is text, search, url, tel,
email, password, number or one of the date types — `readonly` does not
exempt one, and this is a DEPARTURE FROM `wend`, whose round-5 truth pass
let a readonly box stand aside; that rule does not survive the lift),
otherwise the form submits with itself as submitter and no overrides. A submitter's own `formaction` (presence
recorded apart from the string — `formaction=""` is the page, round 4),
`formmethod`, `formenctype`, `formnovalidate` overrule the form's.

**D. Constructing the entry list** (§4.10.21.4). One door:
`page_entry_list(page, form, submitter, &entries)`. Tree order — visible
controls and `type=hidden` and everything under a `hidden` attribute in ONE
list, because the order is what a server sees exactly when two controls
share a name (the truth pass, round 5). Skipped: a control with a
`datalist` ancestor; a disabled one (own attribute, disabled `fieldset`
ancestry with the first-`legend` exception, on every path — round 7 found
the hidden path without it); a submit control that is not the submitter;
an unchecked checkbox or radio; a control with no name unless it is an
image button. A `select` contributes every selected option that is not
disabled (own attribute or its `optgroup`'s), `multiple` or not; the
option's value is its `value` attribute or its text. A checkbox or radio
with no `value` attribute sends `on`; `value=""` sends the empty string
(round 5). A radio GROUP is one name under one OWNER, searched over the
whole document (round 7), and a nameless radio is in no group. An image
button sends `name.x` and `name.y`, or `x` and `y` when it has no name
(round 7), never its value. A `file` input sends its filename, empty when
no file. A hidden input named `_charset_` sends the selected encoding's
name whatever its `value` attribute says (round 7 found it going out
empty). `dirname` appends the control's DIRECTIONALITY as the standard
defines it: `dir=ltr` or `rtl` on the control or the nearest ancestor that
says; `dir=auto` decided by the value's first strong character (an R or AL
before any L means `rtl`); `ltr` when nothing decides. The Bidi_Class
ranges that needs are GENERATED, not written (the 2026-09-12 entry on
Q5). A `readonly` control IS sent; `readonly` means nothing on
ticks and lists (round 7). Every name and value has its line breaks
normalised to CRLF before encoding (round 5's `%0A`).

**E. The encoding** (§4.10.21.7-9). One door: `page_encode(entries,
enctype, encoding, &bytes)`. The encoding is the first label in the form's
`accept-charset` that names an encoding libhtml supports — asked of
libhtml's OWN label lookup (issue #99 asks it to export the one it has;
a second copy of that table is the thing this library exists to stop) —
else the document's encoding, else UTF-8; then the standard's output-
encoding step turns either UTF-16 into UTF-8, so what can actually leave
is UTF-8 or windows-1252. Values are held as UTF-8 (the tree's text and
the dirty value alike) and encoded at serialisation: UTF-8 as they are;
windows-1252 through the encoder libhtml exports from the same table its
decoder reads (issue #99 again); and a code point the encoding cannot
hold becomes a numeric character reference `&#NNN;` — the standard's
`html` error mode — whose `&`, `#` and `;` are then percent-encoded like
any other byte, so it reads `%26%231234%3B` on the wire. That last step
is the classic mistake and has its own corpus case. `wend` sends UTF-8
for everything today; the lift retires that. Three serialisers, each the
standard's: `application/x-www-form-urlencoded` (the safe set is
alphanumerics and `*-._`; space is `+`; everything else percent-encoded
from the selected encoding); `multipart/form-data` (RFC 7578 — a
boundary, one part per entry, a file part carries `filename`);
`text/plain` (`name=value` lines, CRLF). An invalid `enctype` or
`formenctype` is urlencoded, which is what the standard says an invalid
value means. The body is built for POST even though libfetch cannot yet
carry it: the corpus checks bytes, not intentions.

**F. Method, action, and where the data goes** (§4.10.21.3 steps 17-22).
One door: `page_activate` (below) applies the standard's table. Method is
`get`, `post`, or `dialog`; an invalid or absent method is `get`. `dialog`
is NOTHING — the form closes a dialog and no server hears of it; a
`formmethod=dialog` is the same NOTHING through the submitter's door. The
action is resolved through family A; absent or empty means the document
URL. Then by scheme and method: `http`/`https` GET replaces the action's
query with the urlencoded entry list and keeps the action's fragment (a
form's `#name` was once dropped where a link's was kept, round 2); POST
carries the body with the enctype's content type. The standard's table
for the rest: `data:` GET mutates the query like http; `data:` POST,
`ftp:` and `javascript:` (both methods) navigate to the action with no
data at all; `mailto:` GET puts the entry list in the header query and
`mailto:` POST puts it in the body. Anything else is a refusal by name.

**G. Links and fragment navigation** (§4.6, §7.4.2.2). One door:
`page_activate` on a link. The resolved URL compared to the document URL
WITHOUT fragment decides fetch or move; `#` is the top; `#top` is the top
when no anchor claims it; a match is made against the decoded fragment.

**H. Value sanitization** (§4.10.5.1, per input type). One door:
`page_control_value(control)`, which every other family calls and nothing
else reads the attribute behind. Draft one: the single-line types (`text`,
`search`, `tel`, `password`) with line breaks removed, which is the
standard's sanitization for them; `url` and `email` with line breaks
removed and surrounding whitespace stripped; `hidden` and `textarea`
verbatim (then the line-break rule of D at submission); `number` and the
date family sanitised to the empty string when the value does not parse
as the type requires; `checkbox`/`radio`/`submit`/`image`/`button` as D
says. Anything not listed is `text`, which is the
standard's own fallback for an unknown type.

**I. Constraint validation** (§4.10.20). One door: `page_validate(page,
form, submitter)`, run before the entry list unless the form's `novalidate`
or the submitter's `formnovalidate` says not to. A form that does not
validate is NOTHING, with the first failing control reported so a face can
put the cursor on it. Draft one honours `required` (empty text-like value,
no checked radio in a required group, no selected option in a required
`select`) and `maxlength`/`minlength` on the dirty value. **Booked**:
`pattern`, `min`/`max`/`step`, the typed-value checks (`email`, `url`,
`number`); each is one predicate in this one door when it comes.

**K. The navigation a document declares** (the `refresh` pragma directive
and its shared declarative refresh steps). One door:
`os64_page_refresh(page)` → the refresh the page asks for, or none.
`<meta http-equiv="refresh" content="0;URL=...">` is the only way a page can
send a reader somewhere with no server redirect and no script, which is why
it is still everywhere — and why a page that draws to NOTHING is not a page
with nothing to say. DuckDuckGo's HTML results wrap every link in a click
logger that answers 200 with a script for a browser that runs one and this
for a browser that does not; we are the reader that fallback was written
for, and `noscript`'s contents are already in the tree for exactly that
reason. THE GRAMMAR IS WRITTEN AS THE STANDARD WRITES IT, jumps included:
the `url` keyword is scanned a letter at a time and left at three different
points, each keeping a DIFFERENT candidate address, so `content="0;used=x"`
asks for an address called `used=x`. A tidier parser is one that disagrees
with every browser. The first VALID pragma in tree order wins and an invalid
one declares nothing, which is what leaves a later one free to. An address
that will not resolve abandons the whole pragma rather than refreshing
somewhere else. Following it goes through the SAME door a link does, so the
request carries `downgrade` and `leaves_machine` — and that matters more
here than for a link, because this hop is invisible to the fetch: libfetch
judges the redirects inside one fetch and this is a new fetch that starts
where the page said.

**J. Facts, not questions.** Every request carries `downgrade` and
`leaves_machine`. libpage never confirms anything; the face's confirm —
and the type-ahead discipline that kept a queued `y` from answering it
(rounds 2 and 5) — stay above the seam, because they are about a person and
a terminal, not about a page.

## The one door

```c
typedef struct os64_page os64_page_t;

os64_page_t *os64_page_build(const os64_html_document_t *doc,
                             const char *document_url,
                             const os64_page_options_t *opt);   // opt NULL = defaults
void         os64_page_free(os64_page_t *page);

// The model, by index, in tree order. Every item names its node — AND THE
// NODE NAMES ITS ITEM, settled at build. The face draws by walking the tree
// and must ask "which control is this node?"; a face keeping its own
// counter would be a second walk skipping different subtrees, which is the
// two-sites failure wearing a hat.
int32_t                  os64_page_nlinks(const os64_page_t *);
const os64_page_link_t  *os64_page_link(const os64_page_t *, int32_t i);
int32_t                  os64_page_link_for(const os64_page_t *, const os64_html_node_t *);    // -1: not a link
int32_t                  os64_page_nforms(const os64_page_t *);
const os64_page_form_t  *os64_page_form(const os64_page_t *, int32_t i);
int32_t                  os64_page_ncontrols(const os64_page_t *);
const os64_page_control_t *os64_page_control(const os64_page_t *, int32_t i);
int32_t                  os64_page_control_for(const os64_page_t *, const os64_html_node_t *); // -1: not a control
const os64_html_node_t  *os64_page_anchor(const os64_page_t *, const char *decoded_fragment); // the node a fragment names, NULL for none; the face finds the row it drew for it

// Edits — the dirty value, kept apart from the model (ruling 2): a table
// INSIDE the page, KEYED BY NODE and never by index, so a rebuild over a
// changed tree can re-key what survives. The model is width-independent,
// so a face builds it ONCE per page and a re-wrap never touches it.
int64_t os64_page_set_text(os64_page_t *, int32_t control, const char *utf8, size_t len);
int64_t os64_page_set_checked(os64_page_t *, int32_t control, bool on);   // a radio unticks its group
int64_t os64_page_set_chosen(os64_page_t *, int32_t control, int32_t option, bool on);
// Put a form back the way the page wrote it, which is what a reset button
// does: the door reports one as NOTHING with a RESET reason and a face that
// draws the button calls this, the same split as the downgrade confirm.
int64_t os64_page_reset(os64_page_t *, int32_t form);

// The navigation the DOCUMENT declares (family K), or NULL for a page that
// declares none. Followed through the door like anything else.
const os64_page_refresh_t *os64_page_refresh(const os64_page_t *);

// THE DOOR. `what` is a link, a submit control, an implicit submission from
// a control (Enter in a box), or the refresh the page declared. Everything
// in families B-K runs here and nowhere else. The request owns its bytes
// whatever the verdict; free it.
typedef enum { OS64_PAGE_NAVIGATE, OS64_PAGE_FRAGMENT, OS64_PAGE_NOTHING, OS64_PAGE_REFUSED } os64_page_verdict_t;
os64_page_verdict_t os64_page_activate(const os64_page_t *, os64_page_what_t what,
                                       os64_page_request_t *out);
void os64_page_request_free(os64_page_request_t *);
```

`os64_page_request_t` carries: the URL and its fragment; `method`;
`enctype` and the content type it implies; `body`/`body_len` for POST;
`downgrade`; `leaves_machine`; and for NOTHING and REFUSED a `reason` from
one enum with one value per rule that can say no (`DIALOG`,
`DEFAULT_BUTTON_DISABLED`, `IMPLICIT_BLOCKED`, `INVALID` with the control,
`BAD_ACTION`, `TOO_LONG`, `SCHEME`). A face renders reasons; it never
invents one.

Everything is pure computation over the tree: no I/O, no syscalls, no
terminal, no allocation that is not accounted for (the allocation-failure
sweep runs here as it runs in libhtml).

## Bounds

- **The radio search is bounded.** A group is settled by a whole-document
  search per group under a work budget; `wend` renders a 40,000-radio page
  in 0.45 s and this must not regress. Settle every group ONCE at build,
  not at every tick.
- **The forms table is swept once**, at build, so `form=` is answerable
  during the walk.
- **Sizes ride libhtml's.** A page libhtml accepted is a page libpage
  builds; a URL that does not fit `OS64_URL_*` is a refusal at family A;
  a body that would exceed a stated `max_body` is a refusal at family E.
- **Nothing here blocks.** A face that wants to cancel cancels its fetch;
  libpage has nothing to cancel.

## Proof before integration

**The corpus is the durable artefact.** Markup in, expected request out,
host-side under ASan, exactly the way `tools/test_wend_host.sh` runs today.
It outlives every renderer written over it, which is the property the
first draft lacked: 62 findings of specification reading stored in a file
marked throwaway.

- **The 45 `expect_url` cases move in as KNOWLEDGE, not as code.** Each of
  them is a rule of families C to F, and every rule they were checking has a
  case here written from the standard's own step instead — which is what
  makes the coverage number mean something, since a ported case proves what
  the old code did and a case written from the step proves what the standard
  says. Every round-N finding they carried is a named case. The 42
  `expect_lines` cases stay with `wend`, because they test drawing.
- **A case per step.** Each family lists the standard's steps; each step
  has at least one case, and each round-N finding above is a named case
  (`R5: hidden subtree submits`). Coverage per family = steps with a case /
  steps, and that number is printed by the harness and written into
  VERIFICATION.md. "Are we conformant" becomes a number that moves rather
  than a verdict that is argued.
- **URL resolution can run the web platform's own data.** `urltestdata.json`
  is pure data; a host case can feed it to `os64/url.h` through family A,
  and whatever fails there is a libos64 finding, not a libpage one. The form
  submission tests in WPT are mostly script-driven, so family D-F cases are
  ours, written from the algorithm's steps.
- **The pathological page** (lists five deep round a `pre`, form-in-table-
  in-form, unclosed inline tags, an empty `select`, a readonly textarea, a
  disabled fieldset with a bold legend) stays, and grows the three predicted
  findings.
- **The allocation-failure sweep** asks every control what it would send at
  every failure point, as the `wend` sweep does at 700 deep today.
- **In the guest**, a local server's access log is the last word — `GET
  /A?x=1` and `GET /B?x=2` from two radio groups proved round seven on the
  wire, and every family with a wire has one such probe.

## Booked before the first line (the known-debt rule)

| Debt | Why it waits | Trigger |
|---|---|---|
| Parser-inserted form owner | libhtml records no form element pointer | LIBHTML.md request; the corpus skips those cases by name |
| POST on the wire | libfetch sends no body (Fable-tier slice) | the body exists and is checked; the first login worth doing |
| `file` inputs with a file | no face can pick one | the graphical browser's file dialog |
| Constraint validation beyond `required`/length | typed-value families are their own table | the first page whose `pattern` matters |
| Cookies, `Referer` | libfetch's, not the page's | the first site that needs a session |
| `os64_page_rebuild` — re-walk a changed tree, re-key the edits that survive | nothing can change a tree yet (ruling 2); a verb with no caller is speculative | the engine — the edit table is already keyed by node so the verb costs a walk, not a redesign |
| An SVG or MathML `a` is not a link | a foreign element is not an HTML one however it is spelled, and no text face draws SVG. The walk DESCENDS into foreign subtrees, so HTML inside a `foreignObject` is still seen; it is the foreign element itself that is passed over | the graphical browser, which draws SVG and will meet a link inside one |
| A `range` whose span is too wide to hold exactly holds nothing | its default value is the middle of the span, and there is no floating point here — the decimal is carried as an integer and a power of ten, which is exact for every span a page actually writes. Past that it refuses rather than rounding | a real page whose `min`/`max` need an exponent |
| `pattern`, `min`/`max`/`step`, and the typed-value checks | family I's draft one, as this document booked it | the first page whose `pattern` matters |

## What the consumers owe

- **`wend`**: the wire half of `render.c` — form ownership, the entry list,
  the submitter, URL resolution, percent-encoding, charset decoding for the
  query — comes OUT, replaced by calls through the door. The renderer keeps
  the walk, the wrap, the fold, and the cells. `wend_form_url` and the six
  submitter sites are deleted, not wrapped. The face keeps: drawing,
  editing (through `os64_page_set_*`), history, confirm and its type-ahead
  discipline, the POST refusal by name, and rendering a request's `reason`.
- **The graphical browser**: links against the same library and adds
  nothing to it that is about pixels.
- **libhtml**: supplies the parser's `form_owner` insertion record, the
  encoding label lookup, and the windows-1252 encoder (issue #99). libpage
  consumes these exports instead of reconstructing parser state or copying
  the encoding table.
- **libfetch** (Fable): a request body, when the first login is worth
  doing.

## The plan for PR #98

TWO PRs, stacked. **A**: the library and its corpus, off `opus/wend`.
**B**: the `wend` lift on top of A — the six submitter sites and the wire
half of `render.c` deleted, the door called in their place, the three
predicted findings closed there. Both merge together, A first, so round
eight reads a LIBRARY without a renderer's deletions in the same diff.

**A grew two small consumers on the way**, which is a departure worth
stating rather than leaving for a reader to notice. `wend` calls the door
for family K, because a real page needed it the day the library was written
and a library nothing calls is a library nothing proves. That put an
`os64_page_t` on the view, which is the first line of the lift rather than a
detour around it. And `husk` stopped treating an address as a filename
pattern, because a `?` in a query was making `wend <url>` unrunnable
unquoted.

**Everything else in `wend` still goes through `render.c`**, so the three
predicted findings are RIGHT IN THE LIBRARY and still WRONG IN THE FACE
until B lands: a `method=dialog` form typed into wend today still puts its
values in an address. That is the cost of splitting the work, and it is
bounded by B.

Review tier: A is not app code. Its bugs are the class the nine P1s came
from — passwords in query strings — so it takes Codex rounds (requested by
Chris's hand, as always), with a whole-file read of my own each round the
way PR #52's security-shaped files got one; the corpus is what makes those
rounds converge instead of wander. B is deletions and call sites: reviewed
here, tested by Chris in the guest. The `Under review` banner
on BROWSER.md comes down with this ruling written in its place: the rung
between parsing and layout is what a page MEANS, it is built to the
standard whole, and it lives in this library.

## Design changes during implementation

### 2026-09-12: family K, because a real page needed it

Chris followed result 19 on a DuckDuckGo search in `wend` and got "that
answer has nothing in it this browser can show". Nothing was broken: the
address resolved correctly, the fetch worked, the parse worked, and the page
genuinely has no text. DuckDuckGo's HTML endpoint does not redirect with
HTTP at all — it answers 200 with a click logger whose whole body is two
ways of saying "go here", a script and a `noscript` `meta` refresh. Every
result link on every search does the same thing.

So the campaign's own stance fired: implement a thing when something asks
for one, and the search engine a text browser would actually use asks for
this on every link. It is family K above, and it lands with the library
rather than in a renderer because a refresh is a NAVIGATION — it resolves
against the base, it can carry a page from encrypted to plain, and it can
name the page it is already on. Both faces will want it identically.

Three rules about whether to OBEY one stayed with the face, because each is
about a person reading rather than about a page: a delay is patience to
spend and wend has no timer in its key loop, so a delayed refresh is
reported and left alone; a refresh naming the page it is on is a reload with
no exit and is never automatic; and a chain of them is capped the way
libfetch caps redirects. The redirector is also NOT remembered in the
history, or `b` bounces straight off it back into the page it was sending
you away from.

`wend` therefore holds an `os64_page_t` now and calls through the door for
this one rule. That is the first real consumer of the library and it makes
the declared edge honest, and the rest of the wire semantics move onto the
same model with the renderer's lift.

### 2026-09-12: what the building changed, and why

Twelve departures from the text above, each because writing the code made
the text either wrong or under-specified. Nothing here changes a RULING.

1. **`leaves_machine` means "this puts bytes on a network"**, so `ftp:` is
   true where the text's parenthetical ("an http/https navigation") read as
   false. The name has to be true, and the question a face actually has
   underneath — "can my fetch library carry this?" — is answered by the
   SCHEME, which the request now spells. Two facts, two fields, neither
   pretending to be the other.
2. **A LINK is never refused for its scheme.** The standard's table refuses
   an unknown scheme for a form SUBMISSION and says nothing about a link, so
   a link's scheme is stated and the face decides. Which schemes a browser
   follows is a property of the browser, not of the page.
3. **The edit table is the only STORAGE, and the model POINTS at it.** Keyed
   by node, never by index, exactly as ruled — but a control's `value`, its
   `checked` and an option's `selected` are made to reference the edit
   rather than being a second copy of it. Two copies of one fact is what
   this library exists to prevent, and a face that read the wrong one would
   be the same bug in a new place.
4. **No work budget on the radio search.** Every radio sharing a name is
   CHAINED at build, so settling a group is a walk of the group instead of a
   walk of the document, and the whole build is linear. *Bounds*' "must not
   regress" is satisfied by construction; a knob that could never fire would
   be a knob that lies about the cost.
5. **libpage owns the scheme default-port table**, which `os64/url.h`
   declines as policy and will not carry. It is not new ownership — it moved
   out of `wend`'s renderer — but it now also normalises a `<base href>`
   that spells `:443`, which the first draft did only for the document URL.
6. **Two additions to `os64/url.h`, both grammar it lacked.**
   `os64_url_spell` is `os64_url_parse`'s inverse: an address could be taken
   apart and not written back down, so canonicalising one meant assembling
   it by hand. `os64_url_scheme_of` answers which scheme a reference names
   WITHOUT requiring the parser to take it apart, which is the only way to
   learn that `mailto:` is a scheme when the parser's answer for it is "not
   a URL". And `os64_memcmp` was promoted out of the compiler-only body in
   `str.c`, which its own comment invited the first real consumer to do.
7. **A reset button is a FACT, not an action.** Activating one is NOTHING
   with `RESET`, and `os64_page_reset` is the verb a face calls — the same
   split as the downgrade confirm, and for the same reason. Four reasons
   joined the enum for rules that can say no and had no name: `RESET`,
   `NO_SUBMISSION`, `NO_ANCHOR`, and the edit door's `NO_CONTROL` /
   `WRONG_KIND`.
8. **`range` is built whole** rather than falling through to `text`, because
   a range with no value attribute — the common case — HOLDS the middle of
   its span, and a form that sent nothing for it would be wrong on most
   pages that have one. The arithmetic is exact decimal in integers; see the
   booked row for what it refuses.
9. **The multipart boundary is DERIVED FROM THE CONTENT**, so the option for
   pinning one is gone. A library that does no I/O has no randomness to draw
   on and needs none: a boundary must be ABSENT from the content, so it is
   hashed from the content and lengthened until it does not appear. That
   also makes a submission reproducible, which is what lets a case check its
   bytes.
10. **`dirname` is sent for a textarea and for a text or search input**,
    which is §4.10.21.4's own list, though the attribute is spelled on more
    types than the submission step reads.
11. **An unclaimed fragment that is not `top` moves NOWHERE** (the last step
    of scrolling to a fragment), rather than falling back to the top of the document: guessing
    would throw away the place a person was reading.
12. **The build is three passes, not one walk** — ids, then forms, then the
    model — because each needs what the last one found. Nothing walks the
    tree again afterwards, which is the property that mattered.

### 2026-09-12: Opus's read — seven questions, seven rulings

1. **Node-to-item lookup.** Taken. `os64_page_control_for`,
   `os64_page_link_for`, settled at build, and `os64_page_anchor` returns
   the NODE a fragment names. The face draws by walking the tree; a parallel
   counter would be a second walk skipping different subtrees, which is the
   two-sites failure again. Landed in *The one door*.
2. **Where the windows-1252 encoder lives.** In libhtml, exported from the
   same table its decoder reads — not in libpage (a second copy of the
   table), not in libos64 (which stays independent of the parser's data by
   its own rule, `library.mk`). The `&#NNN;` fallback stays in libpage,
   because that error mode is form submission's rule, not the encoding's.
   Landed in family E; the ask is on issue #99.
3. **The label lookup.** Same answer, same issue: libhtml exports the lookup
   it has. One request to Quinn covers the form-owner pointer, the label
   lookup and the encoder.
4. **Edits versus rebuild.** Taken: the edit table lives inside the page,
   keyed by node pointer, never by index. The model is width-independent,
   so a face builds it once per page and the re-wrap dance the current edit
   array exists for goes away. `os64_page_rebuild` is SPECIFIED by that key
   and BOOKED, not built: nothing can change a tree yet, and a verb with no
   caller is speculative. Landed in *The one door* and *Booked*.
5. **`dir=auto` for `dirname`.** Build it, per ruling 1 — but the weight is
   DATA, not code, and data is generated. A `tools/gen_bidi_table.py` in
   the shape of `tools/gen_html_tables.py` (which makes `entities.inc` from
   the pinned WHATWG file) emits the L / R / AL ranges from a pinned
   Unicode `DerivedBidiClass.txt`; the first-strong scan over them is
   twenty lines. It goes in libos64 beside the UTF-8 helpers, because the
   graphical browser's layout of Hebrew and Arabic text is its second
   customer and is not far off. The corpus case is live, not skipped.
6. **Review tier.** Codex rounds on the library PR, at Chris's hand; the
   lift reviewed here. Landed in *The plan for PR #98*.
7. **One PR or two.** Two, stacked, merging together. Same place.
