# E4 — Roam's first real directory browser

## Status and scope

2026-10-13. Quinn owns design/integration; Sol handles delegated implementation.
Branch `codex/roam-e4`, worktree `.worktrees/roam-e4`, base `5a521b63` (E3 PR
#264 merged). Chris authorized making Roam after the E3 architectural check.
E3's recorded P5 acceptance remains separate from this authorization.

Deliver `/bin/roam [directory]`: real FAT/ext2 browsing, Details/icons, async
loading, Back/Forward/Up/Refresh, editable address, clickable breadcrumbs,
quick filter and initial favorites/mounted volumes. Menu, navigation toolbar,
address, breadcrumbs, filter, sidebar and status independently hide; Details
fills the remaining space. Alt/F10 and right-click recover a hidden menu.
Interface font and light/dark appearance follow the existing toolkit.

**File associations are reserved for a conversation with Chris before E5.**
No extension-to-application launcher, Open With implementation or file launch
in E4. Activating a directory navigates; activating a file reports that opening
files is not configured. No rename, writes, delete, clipboard file operations,
recursive tree, tabs, split panes, previews or IPC/kernel changes. A file type
label/icon is metadata presentation, not an association or content inspection.

## Shared boundaries and frozen interfaces

`os64/directory.h` / `directory.c` owns path normalization, bounded immutable
raw listings, exact-name ID matching, metadata presentation, projection creation
and mounted-volume discovery. `os64/browser.h` / `browser.c` owns background
requests, generation checking, committed location, bounded navigation history,
filter/hidden state and transactional publication into an E3 Details widget.
`os64/ui_browser.h` / `ui_browser.c` composes reusable navigation/address/
breadcrumb/filter/places widgets around that controller for E5 picker reuse.
`apps/roam` owns its window, menu/action policy, pane visibility and app lifetime.
No libos64 -> libimage dependency; the application lends the E2 provider.

Public directory/browser headers in this packet define the integration names.
Changes require coordination between component owners. Root integration owns
E3's two focused additions: public detached selection cloning, and transactional
view restore from such a selection plus viewport. Restore must prepare before
publishing, maintain revision/owner guards and leave the old view on failure.
These are general toolkit operations, not filesystem behavior.

## Listing, identity and metadata

Paths are absolute, normalized lexically against the committed location (startup
uses a captured cwd). Collapse repeated '/', '.' and '..'; root stays root.
Do not mutate process cwd. Enforce the syscall's 255-byte path payload without
truncation, including joined entry paths. Preserve raw filename bytes for
navigation; display escapes NUL/control/newline concerns rather than using the
escaped label as a filename. Ignore '.' and '..' entries. Reject malformed or
duplicate names rather than manufacturing a different path.

At most 10,000 entries per listing. Read one additional eligible entry to detect
the bound; say "Showing first 10,000 entries; listing incomplete" persistently.
Filtering that snapshot must not claim to search omitted entries. A directory
read error, failed close or allocation failure discards the candidate and keeps
the current listing; an initial failure leaves an empty view with an error.
Empty directories are successful results, distinct from failure. Cancellation
is checked between reads and bounded conversion stages. No unbounded I/O in
paint, action queries, layout or the UI event handler.

Raw immutable listings have thread-safe references, but they do not own an E3
snapshot: E3's references are lifecycle-thread serialized. Worker-produced E3
data/projections transfer exclusive ownership to the UI on reap; cancelled
products were never published, so their worker cleanup cannot race UI retains.
Previous raw listings can be read by workers for ID matching; retain them before
submission and release through their thread-safe lifetime API.

IDs are nonzero and scoped to a location domain. Refresh/history reload matches
exact raw names within that domain and allocates IDs above its retained high
water for new entries; removed IDs do not get reassigned while that domain is
retained. This is best-effort matching, not stable filesystem object identity.
A new navigation gets a new domain. FAT case changes may lose preservation.
Unknown times (mtime zero or unrepresentable) stay blank; use typed unsigned
mtime sort keys and existing calendar formatting. File size is bytes, directory
size blank. Folder/mount/file kind and a small extension-based display-type
vocabulary are permitted; directories stay first in both sort directions.

Quick filter is an ASCII-case-insensitive substring of the raw name, with other
bytes compared literally. It differs from E3 typeahead and does not traverse.
Leading-dot names are hidden by default; Show Hidden Files is independent of
filter text. Filtering intersects selection; clearing the filter does not
resurrect it. Snapshot IDs and raw entries stay available for later re-filtering.

## Requests and publication

The browser owns one small os64_work pool and a reserved doorbell bit. Input
and products have declared bounded memory reservations. At most two admitted
listing jobs plus one bounded latest-request intent; obsolete requests are
cancelled. If admission is temporarily full, retain only the latest intent and
retry at a completion/space doorbell, never a polling loop. Allocation/refusal
before accepting a request preserves the earlier request and committed view.

Each accepted navigation increments a request generation; completion checks
both its job identity and generation before touching the view. Loading status
names the requested destination while the committed address, breadcrumbs and
rows continue to describe the old location. E3 replacement/restore preparation
runs before location/history/filter state is committed. A failed bind discards
the candidate and keeps the old location, history cursor, rows and selection.
Callbacks request UI updates, not nested dispatch or immediate teardown.

User activation from an E3 callback queues the ID/domain for processing at the
app's dispatch boundary. Revalidate it against the current listing; do not
borrow a row pointer across callbacks or asynchronous work. Cancel/close makes
late completions irrelevant. Stop/join the pool and release its jobs before
releasing the view, icon provider or native owner window. No forced worker kill.

Keep menu targets stable: a saved context ID/domain is checked at invocation.
The UI may defer publishing a completion while a menu is open, retaining a
single result until the menu closes, so a native popup cannot silently retarget.
Do not suppress close/appearance/doorbell processing while menus are open.

## History and restored presentation

Maintain up to 32 path history entries. Only successful navigation pushes an
entry or moves the history cursor; a new success drops the Forward tail.
Refresh does not push history. Navigation to the already committed path is
Refresh. Back/Forward re-read the directory, using cached raw names/IDs where
available. Preserve per-location filter, hidden flag, sort, selected/focus/anchor
IDs and viewport when entries still match. Preserve column layout across
locations as a view preference.

Retain at most eight cached location states, with a 64 MiB aggregate budget
covering raw listings, retained data/projections and saved selection. Count
shared allocations once or conservatively more, never less. Evict least-recent
cached states first (not the active location); retain their paths in history.
Returning to an evicted state still navigates, with fresh identity/selection and
default viewport; filter/sort choices may remain in the small history record.
Selection preservation is best effort within this explicit cache, not unbounded
snapshot retention. Failure to allocate a history bookmark must refuse the
navigation before replacing the old view, not silently half-save the state.

## UI and delivery

Start at the optional directory argument, otherwise captured cwd, with '/' as
fallback only if cwd retrieval fails. Show Roam's name in its window title;
location lives in the address/breadcrumb area so long paths do not hit the
native title limit. Default window fits a 1024x768 screen. Keep controls compact
and readable; no engineering diagnostics in production chrome.

Places initially contains Root and Home favorites (absolute HOME, otherwise
`/home`; omit the duplicate if HOME resolves to `/`) plus real mount points from
`/sys/mounts`, with bounded parsing and owned labels/paths. Refresh volumes off
thread alongside listings; absence/failure must not fabricate mounts or fail
an otherwise readable directory. Sidebar item activation navigates. Persistence
and favorite editing are later work; label the group Favorites, not a claim of
user-authored saved settings.

Menu groups File/Edit/View/Go/Help expose implemented commands, column options,
Show Hidden Files and the independent pane toggles. Include Ctrl+L (reveal/focus
address), Ctrl+[ / Ctrl+] for Back/Forward, Alt+Up or Backspace for Up when the view owns focus,
F5 Refresh, Ctrl+F (reveal/focus quick filter), Ctrl+H hidden files, and Ctrl+Q.
Os64 reserves Alt+Left/Right for virtual-terminal switching; Roam preserves that
platform binding. Keep textfield editing and E3 selection keys owned by their controls. Header
column menus and row context menus use E1B. Hide-all leaves the Details view;
Alt temporarily reveals menus and right-click offers Show Menu Bar.

Breadcrumbs are bounded clickable path components, with an overflow affordance
or tail display on narrow windows; retain Root/Up/address access. Text and hits
must share prepared geometry at the active Interface font. Font adoption plans
all widget bounds before commit. On tiny windows, clip/hide unusable controls
without negative geometry or phantom hit targets; never lose menu recovery.

Install through normal userland build discovery, FAT/ext2 image recipes and
os64get's existing bin/lib routes. Add Roam to the desktop root menu. No new
assets or icons need a bespoke transfer step; reuse E2's installed collection.

## Acceptance and work order

1. Directory/controller core and host tests: normalize/join bounds, real names,
   missing/zero metadata, folder sort, filter, duplicate/malformed input, EOF
   versus read/close error, allocation refusal and 10,001-entry truncation.
2. E3 clone/restore tests plus browser fake-work sequences: A then B completing
   out of order, failed publication, failed Back, cancellation, history eviction,
   refresh ID preservation, filter changes during load and shutdown cleanup.
3. Shared browser UI and Roam: real-font host tests where practicable, menu/input
   precedence, hidden-menu recovery, tiny/large font layout and context lifetime.
4. Cross-build; native QEMU on real FAT/ext2 with empty/large/long-name fixtures,
   repeated navigation, partial limits, bad paths, appearance and pane toggles.
   Capture installed hashes/screenshots and a reproducible P5 walkthrough.
5. Ordinary code review, publication/PR and Chris's P5 acceptance. Record each
   environment separately; never extend earlier E3 evidence to E4.

Preserve ASan/UBSan/LeakSanitizer. Apply E3's invariant/transition method to all
state-changing paths. Tests are not run at design time. Fable architectural
review is not required for this userland slice. File-association design waits
for the explicitly requested conversation with Chris.
