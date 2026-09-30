# yonder - a user's guide

yonder is os64's graphical web browser. It reads HTML and CSS, shows
pictures, fills in forms, keeps cookies and remembers where you have been.
It runs no JavaScript yet: a page that builds itself with a script shows
what its HTML holds, and no more.

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

A file named on the command line gives the window its title (a window is
named once, so a page from the network leaves it "yonder"). Pictures, style
sheets and links beside a file are read from beside it, and `file:///path`
works in the address field too.

## The window

```
+------------------------------------------------------------------+
| [Back] [Forward] [Reload] [Stop]  [ address                    ] |
+------------------------------------------------------------------+
|                                                                  |
|   the page                                                       |
|                                                                  |
+------------------------------------------------------------------+
| the status line                                                  |
+------------------------------------------------------------------+
```

- **The address field** takes an address (`https://...`, `http://...`, or
  a bare `host/path`, which means http) or a path that begins with `/`.
  Press Enter to go.
- **Back and Forward** walk the pages you have been to; yonder remembers
  where on each one you were.
- **Reload** fetches the page again. Its pictures and style sheets come
  from the cache when the server said they may (see "The cache" below).
- **Stop** is lit while a page loads, and stops it.
- **The status line** says what yonder is doing, where a link goes when
  the pointer is over it, and how long the last layout took. When a page
  is incomplete - cut off, too big, missing pictures - it says so there.
- **The title bar's Settings icon** (at its right) opens Settings.

## Moving around a page

| Key or action | What it does |
|---|---|
| Mouse wheel | scroll three lines a notch (a tilt scrolls sideways) |
| Up, Down | one line |
| Left, Right | sideways, one line's worth |
| Page Up, Page Down, Space | a screen, less one line |
| Home, End | the top, the bottom |
| Click a link | follow it |
| `p` | turn positioning off, and on again (below) |

Resizing the window lays the page out again and keeps your place.

**`p` shows a page with positioning off**: every box in document order,
nothing fixed or stacked over anything else. With no scripts, a
cookie banner or a menu that a script would have closed can cover the page;
`p` reads what is underneath. It stays on as you move from page to page, and
the status line says POSITIONING OFF on every page it lays out, until `p`
again turns it back on.

## Forms

Text fields, password fields, check boxes, radio buttons, lists and buttons
work, and a form is sent with its button or with Enter in one of its
fields. A password field shows bullets only.

Not yet: a text area is one line, a drop-down list is shown as a short list
box, and a file cannot be chosen for upload.

## Questions

Sometimes yonder asks before it does something - most often before a page
or a form would send you from https to plain, unencrypted http. The question
appears in a bar above the status line with **Yes** and **No**. Escape is
No, and Enter does not answer: a question you did not see must not be
answered by a key you pressed for something else.

## Cookies

yonder keeps cookies as other browsers do, so logging in to a site works.
They last while yonder runs: closing it forgets them.

## Settings

Click the Settings icon in the title bar.

- **Identify as**: the name yonder gives every site it asks (the
  User-Agent). Pick one - yonder, Chrome, Firefox, Safari on an iPhone,
  Netscape 4, Lynx - or type another. Sites answer by it: an old-web site
  may serve Netscape 4 a very different page from Chrome.
- **Keep pictures and style sheets, up to [256] MB**: the cache, and how
  big it may grow.
- **Empty the cache**: removes everything the cache holds, at once.
- **Apply** changes this window from the next page on. **Save as default**
  also writes the settings to `yonder.conf`, where every new yonder reads
  them.

When Settings opens, its bottom line says what the cache holds.

## The cache

yonder keeps the pictures and style sheets it fetches in
`/var/cache/yonder`, and uses them again - after a Reload, in a new window,
after a reboot - for as long as the server that sent them said they are
good. A site that says "a year" (danlegt.com does) is loaded from the disk
after its first visit. The page itself is fetched every time.

- A picture or style sheet the server asked to be checked is checked, with
  a small request that is answered "not modified" when nothing changed.
- When the server cannot be reached at all, yonder shows the copy it has,
  unless the server asked for it never to be used unchecked.
- When the cache is full, the oldest files go first. Lowering its size in
  Settings removes the oldest at once.
- To see a site fresh, Empty the cache in Settings and Reload.

The P5's refresh script leaves `/var/cache/` alone, so the cache survives
a system refresh.

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

A value yonder cannot use is ignored and the default used; a bad `agent` or
`cache_mb` also says so in the log.

## Fonts

Pages are drawn in the DejaVu serif, sans and monospace faces that ship
with os64. `fonts.conf` can name others, with its `family.serif`,
`family.sans` and `family.mono` lines; the shipped `/etc/fonts.conf`
explains them.

## What it does not do yet

- **JavaScript.** Menus, galleries, chat boxes and anything a script fills
  in stay as the HTML left them.
- **Selecting and copying text.**
- **Downloads.** An address that is not a page (a zip, a program) is not
  shown; the status line names the `os64get` command that saves it.
- **Several tabs or windows sharing one session.** Each yonder is its own
  browser, with its own history and cookies (the cache is shared).

What the browsers still owe, as people find it, is kept in
`BROWSER_DEBTS.md` at the top of the source tree.
