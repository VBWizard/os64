# KEPT_READ.md — a fresh cascade keeps the sheets it already read

Fable's ruling (BRIEF_CASCADE_TRACKING_C3_ANSWERS § 2, then the L1 approval of 2026-11-22 with five
conditions). The record that asked for it: the load timeline's fresh-cascade split on a large application
page, where reading the sheets was 64% of every fresh build (64 builds, 15.0 of 23.6 s, ~22,000–25,000
rules each), the same sheets each time.

## What the read is

Everything `build()` makes before it touches an element: the rules read with their declarations, the
per-target selector index, the keyframes (built for no element) and faces, the anchors and style readers,
the dependencies, `untrackable`, the scopes (`given`, `roots`, `applies`, `root_slots`), the inputs, and the
skipped records. It lives in a `Read` (cascade.c): its own arena `heap` for everything but the declarations,
which are in `reads`. While `build()` reads, `alloc` and `grow` carve from the read's arena (`c->heap`);
after it, from the cascade's own `self`.

## Who keeps it

A read is refcounted, and a caller may hold one apart from any cascade (`garb_cascade_read_hold`,
`garb_read_release`). A fresh build is handed a held read (`garb_cascade_limits_t.read_from`). When the key
is equal it takes its own reference and copies the read's fields into its own (`read_install`); when not,
it reads again into a read of its own (`read_sheets`). yonder holds the read of the cascade it replaces
before anything can free that cascade (a measuring page's fallback does, before the new build) and releases
it once the build returns. Each page's cascade carries its own read, so the measuring cascade and the
page's never trade one slot.

## The key (`read_key`; the first member that differs is named, `garb_read_refusal_t`)

| member | why | compared how |
|---|---|---|
| the read is keepable | a read that came up short, or whose declarations had no arena of their own, is never taken | `keep` |
| the document | the scopes name its roots | identity |
| the limits | `state_bytes` caps the read's arena, `read_bytes` its declarations | value |
| quirks mode | declarations and `@supports` are read in it | value |
| `garb_env_t`, every field | `@media` groups, sheet and import media are decided while reading | value, all fields |
| each input sheet | its rules are what is read | its parse serial (`garb_parsed_t.serial`), never its address; parent, imported or not, media text, the import's `via` |
| the scopes | which sheets apply in which tree | as `garb_cascade_matches_scoped`: root address and sheet lists |

**The serial has one door** (Fable's condition 1): `start_limited` (parse.c), which every parse passes, so a
parse into a struct used before takes a new one. The struct's comment says that anything changing a parsed
sheet in place must stamp it too. Mutant: a re-parse keeping its serial.

**Not in the read**: viewport units and `var()` (declarations keep them; they resolve at matching), fonts,
pictures and form state (nothing in the read looks at them), the elements (none is visited before the slot
table), `@container` (not supported) and `@layer` (cascaded as unlayered).

## Never written after it is made (condition 2)

Every write into what becomes the read, once `build()` is done with it, and what was done with each:

| write | where | now |
|---|---|---|
| a rule's match mark (`seen`, `update`, `pass`, `slot`) | `consider()`, at every element | `RuleSeen`, the cascade's own, indexed as the read's rules are |
| `dependencies.style_attribute`, `style_bare` | `read_decl()` at a `style` or presentation attribute | the cascade's copy of the fields; the read keeps its sheets' values |
| the skipped records: a count raised, a name added | `skipped()` and its callers, at attribute reads | copied into the cascade's arena before the first write (`skips_own`) |
| `allocs_failed` | `alloc` / `grow` | a value copied: the read's and the cascade's are apart |
| the arena | every later `alloc` | the read's arena is its own; the cascade carves from `self` |

Shown not to exist: `anchor_bits()` only reads; `file_selector()`, `add_face()`, `add_keyframes()` and
`keyframes_settle()` run only while reading. `var()` substitution carves from `heap`, so one that ran while
reading would land in the read's arena, not in the making cascade's.

**Checkable**: with `check_read` (yonder sets it under `--cascade-check`) the read is hashed raw when made
(`read_digest`: every array a cascade could reach, as its bytes lie) and again when taken; a difference sets
`garb_cascade_read_written`. Under the same switch yonder compares a taken read with the sheets read again,
by content (`read_content`, no addresses): the record's `kept read check` counts the checks and those that
differed, and a difference is printed too.

## The scopes' roots (condition 3)

The read hashes and compares a root's address and never follows it (`scope_of_root`; `given` is read only
for its addresses and sheet lists). It takes no pin of its own. A read is taken only when the caller's
scopes, whose roots are live at that moment, equal the read's root by root. If a root the read names had
been reclaimed and its address handed to another node, that node would be passed with the same sheet list
and would map to the same scope, which is what reading the sheets again would have built: the read is equal
by content. No pin is spent on it: a document gives sixteen. Fixture: `shadow_kept_read` (a host removed and
another attached between builds, a read held past its giver's free, under ASan).

## What it costs, and what it keeps

- The retained budget an update may grow to counts the read's size (`Read.bytes`), so updates behave as
  they did when the read lay in `self`.
- `garb_cascade_parts` counts the read in each cascade holding it, so a read shared for a moment by the
  outgoing and incoming cascades is counted twice.
- A cascade's state cap (`state_bytes`) now caps two arenas, `self` and the read's `heap`, where it capped
  one: the read can be as large as before, and so can the cascade's own state beside it.

## The record

The load timeline's `timeline layouts` line says, for each asker's fresh builds, how many took a kept read,
and after the askers, why the others read again (`garb_read_refusal_name`).

## Booked

- **A read that survives a resize**: reading `@media` groups unfiltered, each rule carrying its condition,
  would keep the read across a viewport change. Not now.
- **Condition 5's thrash**: one read per cascade lineage. If a run shows one lineage alternating
  environments, Fable is asked before anything holds a second read.
