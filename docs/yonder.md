# yonder - a user's guide

yonder is os64's graphical web browser. It reads HTML and CSS, shows
pictures, fills in forms, keeps cookies, and runs a page's JavaScript when
you turn that on (Settings, below; it is off until you do). With it off, a
page that builds itself with a script shows what its HTML holds, and no
more. Each window remembers the pages it has been to while it is open;
nothing is remembered after it closes, and there are no bookmarks.

This is how to use it. How it is built is in `docs/design/completed/YONDER.md`
and the documents that page names.

## Starting it

From a shell on the desktop (a gterm):

```
yonder                          an empty window: type an address
yonder https://danlegt.com/     open a page
yonder danlegt.com              the same, over http: a bare name means http
yonder /home/page.html          open a file on this machine
```

The window is named after the page on screen, `<title> - yonder`, and
follows it: to the next page, and when the page's `<title>` changes, by
its scripts or otherwise. A page with no title leaves it "yonder". Pictures, style
sheets and links beside a file are read from beside it, and `file:///path`
works in the address field too.
Clicking a local-file link from an already open local page opens that file.

## The window

```
+------------------------------------------------------------------+
| [Back] [Forward] [Reload] [Stop]  [ address        ] [Downloads] |
+------------------------------------------------------------------+
|                                                                  |
|   the page                                                       |
|                                                                  |
+------------------------------------------------------------------+
| the status line                                                  |
+------------------------------------------------------------------+
```

The five buttons wear the icon set's pictures beside their labels (the
set `icons.conf` names, the shipped one beneath it); without a set they
are their labels alone.

The window remembers its normal position and content size in
`yonder-window.conf`, using the configuration search path. Moves and resizes
save that normal rectangle. On orderly exit, it also saves whether it was
maximized; the next launch opens in that state, with the normal rectangle
available through Restore. Minimizing does not become a saved startup state.

The buttons, address field and status line follow Appearance Workshop's
Interface font. A font that leaves too little room for the address field is
refused without changing the current font. Enlarging or maximizing the window
retries that setting automatically. Page text and form controls use their own
Web font settings.

- **The address field** takes an address (`https://...`, `http://...`, or
  a bare `host/path`, which means http) or a path that begins with `/`.
  Press Enter to go. **Ctrl+L** takes you there from anywhere in the
  window with the whole address selected, so what you type replaces it.
- **Back and Forward** walk the pages you have been to, and the places
  within a page you went to by its `#` links or, with page scripts and
  the modern web surface both on (Settings, below), that its scripts added
  (a single-page app's views); going back to one of those fetches nothing.
  yonder remembers where on each one you were and puts you back there as
  the page arrives, unless the page keeps its own place (a script that
  sets `history.scrollRestoration` to `manual`); a page that grows after
  it arrives, as its pictures or scripts come in, can come back short of
  where you were. A page fetched again by Back or Forward, or by Reload,
  gets back what its scripts left with that entry, as a browser gives it.
  Going back to the reply to a form asks for that page again without
  sending the form, which can be a different page.
- **Reload** fetches the page again, once it has finished loading. What
  the page uses comes from the cache when the server said it may (see
  "The cache" below). On the reply to a form it asks first whether to
  send the form again (Questions, below).
- **Stop** is lit while a page loads, and stops it.
- **The status line** says what yonder is doing, where a link goes when
  the pointer is over it, and how long the last layout took, with the
  zoom when it is not 100%, how many of the page's pictures are in, and
  the name sites are being told when it is not yonder's own. When a page
  is incomplete - cut off, too big, missing pictures - it says so there.
  A page's `alert` appears there too, as `Page says: ...`; no box opens,
  and a page that asks a yes-or-no question or for a line of text
  (`confirm`, `prompt`) is answered no and nothing. At its right end,
  `MISSING n  FAILED n` counts what the page asked for that yonder does
  not have and what went wrong; the page's diagnostics file (`diagnostics`,
  below) lists them.
- **Downloads**, at the address row's right end, is lit while its list
  has anything in it, and a bar along its foot shows how far the
  downloads running have got (below).
- **The title bar's Settings icon** (at its right) opens Settings.

## Moving around a page

| Key or action | What it does |
|---|---|
| Mouse wheel | scroll three lines a notch (a tilt scrolls sideways): a box under the pointer that scrolls by itself first, then the page |
| Up, Down | one line |
| Left, Right | sideways, one line's worth |
| Page Up, Page Down, Space | a screen, less one line |
| Home, End | the top, the bottom |
| Ctrl+= or Ctrl++, Ctrl+-, Ctrl+wheel | zoom in and out, in steps from 25% to 500% |
| Ctrl+0 | back to the zoom Settings names |
| Ctrl+L | to the address field, its text selected |
| Escape, in the address field | back to the page |
| Click a link | follow it |
| Right-click a link or a picture | its menu (below) |
| Shift+F10 | the same menu, for what is under the pointer, while the page has the keys |
| `p` | turn positioning off, and on again (below) |
| `d` | write the page's diagnostics file now (`diagnostics`, below) |

The page has the keys when yonder opens on an address, after a click on
it, and after Escape in the address field. With page scripts on, a page
that listens for keys hears them first and may keep one for itself. There
is no find-in-page, no key for Reload or Back, and a middle click or
Ctrl+click on a link does nothing more than a click.

Resizing the window lays the page out again and keeps your place.

**The right-click menu** opens over a link, a picture, or a picture that is
a link, and offers what fits: **Open Link in New Window**, **Save Link
As...** and **Copy Link Address** for the link, **Open Picture in New
Window**, **Save Picture As...** and **Copy Picture Address** for the
picture, all six for a picture that is a link. A frame inside a page is
not loaded: it is drawn as a pale panel naming its address, and that panel
is a link, to click or to open from this menu. Over
anything else nothing opens. The two Save items are grey for an address
that is not `http` or `https` (a file on this machine). A copied address goes to the system clipboard,
so it pastes into a gterm or any field (Ctrl+V). A new window is another
yonder, with its own history; it shares the cookies through their file
(unless `cookies` is `session` or `off`), so a site you are logged in to is
logged in there too. A link that runs a
script (`javascript:`) can be copied but not opened in a new window, which
has no page for it to run in.

**Saving a file.** *Save Link As...* and *Save Picture As...* ask where it
goes in the Save dialog, which opens on the `downloads` folder with a name
filled in: the one the server gives (its `Content-Disposition`), else the
last part of the address, else `download`, numbered (`tool-1.zip`) when
the folder already has one. A click on a link to something yonder cannot
show (a zip, a program, a PDF) opens the same dialog instead of leaving
the page. While the dialog is up, yonder's window takes no clicks or keys;
a click on it brings the dialog forward.

The file is written as `<name>.part` (or `<name>.1.part` when that is
taken) and renamed when the last byte is in,
so a name in the folder is always a whole file; one that fails or is
cancelled leaves nothing behind. Downloads go on while you move to other
pages, three at a time (the rest wait their turn), and stop when yonder
closes. Nothing about them goes on the status line: **Downloads**' bar
fills as they arrive (a short band moves along it when a server does not
say how big the file is), blinks green three times when one finishes, and
turns red when one fails, until you open the list. Click
**Downloads** for the list, newest first: each file's name and how far it
got, or why it failed and how far it had got. While it is open it keeps
up: a running row says its speed over the last few seconds and the time
that leaves (`stalled` when nothing has come for a while), and a finished
one its average speed. Pick a row for **Cancel Download** (one waiting or
running) or **Show in Roam** (one that finished); **Clear finished** at
the foot empties the list of the ones that are over. The list keeps 32
rows, and the oldest finished one makes room for a new one.

**`p` shows a page with positioning off**: every box in document order,
nothing fixed or stacked over anything else. With page scripts off, a
cookie banner or a menu that a script would have closed can cover the page;
`p` reads what is underneath. It stays on as you move from page to page, and
the status line says POSITIONING OFF on every page it lays out, until `p`
again turns it back on.

## Forms

Text fields, password fields, check boxes, radio buttons, drop-down lists,
list boxes and buttons work, picture buttons and Reset among them, and a
form is sent with its button or with Enter in one of its fields. A
password field shows asterisks only.

A form is not sent while one of its fields breaks the page's own rule for
it - left empty when it is required, not an email address or a web
address where one is asked for, too short or too long, not in the pattern
the page gives. yonder goes to that field and the status line says what
is wrong with it.

Not yet: a text area is one line, a field for a date, a number, a colour
or a range is a plain text field, and a file cannot be chosen for upload
(its button is drawn and does nothing).

## Questions

Sometimes yonder asks before it does something - before a page or a form
would send you from https to plain, unencrypted http, and before Reload
sends a form again. The question
appears in a bar above the status line with **Yes** and **No**. Escape is
No, and Enter does not answer: a question you did not see must not be
answered by a key you pressed for something else.

## Cookies

yonder keeps cookies as other browsers do, so logging in to a site works,
and it keeps them after it closes, so you stay logged in: the next yonder,
and every other yonder window open now, sends them too. A cookie a site
sets only "for this session" (one with no expiry date) is never kept past
the window that heard it, as in every browser.

They are kept in ONE file for every site, **`/home/yogi/.yonder/cookies`**
(`cookies` in your profile folder, which `profile` in `yonder.conf` can
move; nothing goes under `storage/`, which is page storage's). It is the same `cookies.txt` that curl and wget read and
write, a line a cookie. **It is a file of your logins: anyone who can read
it can sign in to your sites as you**, and it says so in its first lines.
To log out of everything at once, press **Clear cookies** in Settings,
which empties the file and this window's cookies. Another yonder window
open now lets its cookies go at its next page; until then the page it
shows still sends them, and a cookie a site set in it during the last
second (or ten, after a write that failed) is written back into the file.
Close the other windows first to be sure. The diagnostics files (`diagnostics`,
below) are not emptied: a page's record lists the cookies its scripts
can read, and if you keep records, they hold those too.

yonder makes the file itself the first time it has a cookie to keep. When
it cannot keep them (a folder where the file goes, a disk that refuses the
write), the status line says so once, and Settings says why every time it
opens.

`cookies` in `yonder.conf` (or the two boxes in Settings) says how much is
kept:

- `cookies = on`, the default: kept in the file, as above.
- `cookies = session`: kept while the window is open and never written
  down, and the file is emptied when yonder closes (and when a yonder
  starts this way). Ticking the box again before then loses nothing.
- `cookies = off`: no cookie is sent or kept, and the file is emptied.
  Most sign-ins stop working.

yonder sends a site at most 8 KB of its cookies in one request, which is
as much as most servers read. A site that has set more than that is sent
first the cookies its pages' scripts cannot read (where a login lives),
then its newest, and the page's diagnostics file says `FAILED cookies`
with how many were left out.

## Settings

Click the Settings icon in the title bar.

- **Identify as**: the name yonder gives every site it asks (the
  User-Agent). Pick one - yonder, Chrome, Firefox, Safari on an iPhone,
  Netscape 4, Lynx - or type another. Sites answer by it: an old-web site
  may serve Netscape 4 a very different page from Chrome.
- **Keep pictures and style sheets, up to [256] MB**: the cache, and how
  big it may grow. It keeps more than the label says (The cache, below).
- **Empty the cache**: removes everything the cache holds, at once.
- **Zoom every page to [100] %**: the zoom a page starts at, and where
  Ctrl+0 goes back to.
- **Dark pages**: pages with a dark design of their own use it, and the
  rest are darkened.
- **Run page scripts**, and the **Script time limit** beside it: whether
  a page's JavaScript runs, and how long one piece of it may run. Scripts
  are off until this is ticked (or `yonder.conf` says `scripts = on`).
- **Run animations**: whether a page's CSS animations move. Turn it off if
  one makes a page too slow. yonder stops them by itself on a page where
  one frame takes more than 200 ms, and the status line says ANIMATIONS
  PAUSED (SOME ANIMATIONS PAUSED when only some were stopped) with what
  the frame took.
- **Reduce motion**: tells pages you would rather see less movement; many
  turn their animations off when told.
- **Modern web surface**: everything the Modern Web work has added to what
  a page's scripts and style sheets can use. On unless `yonder.conf` says
  `modern = off`. If a site that used to work stops working, turn it off:
  the names that work added are absent from the page's scripts and the
  CSS it added is not read, while the page's objects keep the same levels
  of prototypes either way. The next page you open takes the change, and
  that page's diagnostics file says `modern: off`.
- **Clear page storage**, at that row's right: removes what every site
  has kept in its page storage (below), at once, and the pages open now
  forget theirs. A page in another yonder window that writes at that
  moment keeps what it writes then, and may bring back what its site had
  kept.
- **Accept cookies** and **Keep them when yonder closes**: both is
  `cookies = on`, Accept alone is `session`, neither is `off` (Cookies,
  above). **Clear cookies**, at that row's right, empties the cookie file
  and this window's cookies at once.
- **Apply** changes this window. The zoom, Dark pages, the two animation
  boxes and the script time limit change the page on screen at once;
  turning page scripts on or off loads it again; the modern web surface
  waits for the next page. **Save as default** also writes the settings
  to `yonder.conf`, where every new yonder reads them. **Close** shuts
  Settings.

When Settings opens, its bottom line says what the cache holds.

## HTTP/2

To a site reached over `https://`, yonder offers HTTP/2, and a site that
takes it carries a whole page's requests on one connection, all at once.
A site that does not is spoken to over HTTP/1.1, on up to six connections
to a host, as is any site reached over plain `http://` or through a proxy.
Nothing needs setting; most sites take it.

- **Identify as** decides how the connection is opened as well as the
  name sent. As Chrome or as Firefox, yonder opens an HTTP/2 connection
  the way that browser does; as yonder, as Safari on an iPhone or under a
  name you typed, it opens one its own way. As Netscape 4 or as Lynx it
  speaks HTTP/1.1 only, as those browsers do.
- **To turn it off**, put `http2 = off` in `net.conf`. That file is the
  system's and every program that fetches reads it, so the line is not
  one of yonder.conf's. Each program reads it once, as it starts fetching:
  start yonder again after changing it. A `/home/net.conf` is read in place
  of `/etc/net.conf`, not on top of it, so one made for this line needs the
  `nameserver` line too. The shipped `/etc/net.conf` says the same beside
  the line, commented out.
- **To see which a page used**, turn on `diagnostics` (yonder.conf, below).
  A page's file says `HTTP: 2` with the stream that carried it, or
  `HTTP: 1.1`, and its `wire` lines count the connections the page made
  and the requests sent, by which of the two carried them.

## The cache

yonder keeps what a page is made of - its pictures, style sheets, scripts
and web fonts, and what its scripts fetch when they allow it - in
`/var/cache/yonder`, and uses them again - after a Reload, in a new window,
after a reboot - for as long as the server that sent them said they are
good. A site that says "a year" (danlegt.com does) is loaded from the disk
after its first visit. The page itself is fetched every time.

- One the server put no time on is kept for a tenth of how long it had
  gone unchanged when it was sent, a week at the most.
- One the server asked to be checked is checked, with
  a small request that is answered "not modified" when nothing changed.
- When the server cannot be reached at all, yonder shows the copy it has,
  unless the server asked for it never to be used unchecked.
- When the cache is full, the oldest files go first. Lowering its size in
  Settings removes the oldest at once.
- To see a site fresh, Empty the cache in Settings and Reload.

The P5's refresh script leaves `/var/cache/` alone, so the cache survives
a system refresh.

## Page storage

A site's scripts can keep things on this machine between visits
(`localStorage`: a login token, a setting, what you last read). yonder
keeps each site's in a file of its own under your **profile folder**,
`/home/yogi/.yonder/storage/`, named for the site (`https://example.com`
is `https%3A%2F%2Fexample.com`); a page from a file or `about:blank`
keeps its storage only while it is open. Every yonder window shares the files: what one site's page
writes in one window, its pages in another read. What a page keeps for
one window only (`sessionStorage`) lives as long as the window and is
never written down. A site may keep up to 5 MB, counted at two bytes a
character, so about two and a half million characters. If the profile folder
cannot be made or a file cannot be written, the status line says so and
the page's storage lasts as long as the page. Clear page storage in
Settings removes it all.

## yonder.conf

yonder reads `yonder.conf` from the configuration path (by default
`/home/yonder.conf`, then `/etc/yonder.conf`). Settings' Save as default
writes it for you; by hand it looks like this:

```
agent = Mozilla/5.0 (Windows NT 10.0; Win64; x64) ...
cache = on
cache_mb = 2048
cache_dir = /var/cache/yonder
```

| Key | Means | Default |
|---|---|---|
| `agent` | what yonder tells sites it is | `yonder/1.0 (os64)` |
| `cache` | `on` or `off` | `on` |
| `cache_mb` | how many megabytes the cache may hold, 1 to 65536 | 256 |
| `cache_dir` | where the cache lives, an absolute path | `/var/cache/yonder` |
| `zoom` | the zoom every page starts at, a whole percent from 25 to 500 | 100 |
| `appearance` | `dark` or `light` pages | `light` |
| `scripts` | `on` or `off`: whether a page's JavaScript runs. Written exactly `on`, in small letters: anything else is off | `off` |
| `script_seconds` | how long one piece of a page's JavaScript may run, 1 to 60 seconds | 5 |
| `script_memory_mb` | how much memory one page's JavaScript may hold, 16 to 2048 megabytes; read when yonder starts | 512 |
| `page_memory_mb` | the memory budget one page's document is cut from, 64 to 4096 megabytes: its tree, its layout, its styles and the objects its scripts are given each take a share as their ceiling (the tree two ninths, the layout two, the styles three, the objects two). No share is smaller than the tree's, because the tree's refusal is the one a page's scripts are told about. What reading the stylesheets costs keeps a ceiling of its own, the layout's computed styles (one for each element) are bounded by the tree's, and a page's JavaScript (`script_memory_mb`) and pictures are counted apart; read when yonder starts | 1024 |
| `animations` | `on` or `off`: whether a page's CSS animations move | `on` |
| `motion` | `reduce` or `no-preference`: what pages are told about movement | `no-preference` |
| `diagnostics` | a directory to write a file about each page into, an absolute path | none |
| `keep_pages` | `on` or `off`: keep each page's own bytes, scripts and style sheets beside its diagnostics file; keeps nothing without `diagnostics` | `off` |
| `census` | a host whose pages' files say in detail who holds their memory, by kind and over time (MEMORY_CENSUS.md): that host and its subdomains, or `*` for every page. Repeat the line for more hosts. Every page's file has the one-line summary either way | none |
| `modern` | `on` or `off`: the Modern Web surface | `on` |
| `shadow_dom` | `off`: no shadow trees (`attachShadow`, scoped style, `adoptedStyleSheets` and the rest), for a developer comparing a page without them; anything else leaves them on, with `modern` on; read when yonder starts | on |
| `ranges` | `off`: no DOM Range (`Range`, `StaticRange`, `createRange`) and an always-empty Selection, for a developer comparing a page without them; anything else leaves them on, with `modern` on; read when yonder starts | on |
| `profile` | the folder for your own files (page storage, the cookie file), an absolute path; made when first needed | `/home/yogi/.yonder` |
| `downloads` | the folder the Save dialog opens on, an absolute path; made when first needed | `/home/yogi/downloads` |
| `cookies` | `on`, `session` or `off`: whether cookies are kept in the profile's `cookies` file, only while the window is open, or not at all | `on` |

A value yonder cannot use is ignored and the default used. A bad `agent`,
`cache_mb`, `zoom`, `script_seconds`, `script_memory_mb`, `page_memory_mb`,
`keep_pages` or `diagnostics` says so in the log; a `diagnostics` folder
that cannot be made says so on the status line as well, and so does a bad
`modern`, `profile` or `cookies`, and a bad `downloads` the first time a
file is saved. A bad `cache` or `cache_dir` is replaced without a word.

No `yonder.conf` ships with os64: until Save as default writes one, or you
do, every default above applies.

## Fonts

Pages are drawn in the DejaVu serif, sans and monospace faces that ship
with os64, and in the fonts a page brings with it (`@font-face`: TrueType,
OpenType, WOFF and WOFF2), which are fetched and kept in the cache like
its pictures. `fonts.conf` can name other faces for the three families,
with its `family.serif`, `family.sans` and `family.mono` lines and their
bold, italic and fallback forms, and says which face form controls are
drawn in (`web.face`) and how big a page's ordinary text is (`web.size`);
the shipped `/etc/fonts.conf` explains them. A line yonder cannot use is
named on the terminal it was started from, and the shipped faces are used.

## For measuring

Three switches before the address are for measuring yonder and are of no
use for browsing: `--script-audit`, `--layout-profile` and
`--exit-when-loaded`.

## What it does not do yet

- **Video and sound.** A page's `<video>` and `<audio>` show and play
  nothing.
- **Frames.** A page inside a page (`<iframe>`) is not loaded; a panel
  naming its address stands in for it, and is a link to it.
- **New windows a page opens.** A script cannot open a window, so a
  "Sign in with ..." button that opens one fails, and a link meant for a
  new window opens in this one.
- **Selecting and copying a page's text.** Text in a field can be
  selected; of a page, only the address of a link or a picture can be
  copied.
- **Finding text in a page, bookmarks, a history kept between runs,
  printing, and showing a page's source.**
- **Multi-line text areas, date and number fields, and choosing a file to
  upload** (Forms, above).
- **Boxes a page opens to tell or ask you something** (`alert`, `confirm`,
  `prompt`): the first goes on the status line and the other two are
  answered no.
- **Several tabs or windows sharing one session.** Each yonder is its own
  browser, with its own history; the cache is shared, and so are page
  storage and the cookies, through their files (the cookies unless
  `cookies` is `session` or `off`).
- **gopher.** yonder points you to the `gopher` program.

What the browsers still owe, as people find it, is kept in
`BROWSER_DEBTS.md` at the top of the source tree.
