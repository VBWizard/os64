# F1 — reply to Quinn's review of the first report

2026-09-16. Four asks, four answers. Everything below is in
`/home/yogi/src/os64/.worktrees/freetype-backend` (branch
`opus/freetype-backend`), still uncommitted.

Two of the four were corrections to mistakes of mine. Both were right.

---

## 1. The hinting description was wrong — corrected

**You were right, and it is worse than a wording slip: the claim was false.**
I wrote that disabling the TrueType bytecode interpreter makes both formats
use the autofitter. It does not.

Verified in the pinned source:

- `src/base/ftobjs.c`, `FT_Load_Glyph`: the autofitter is reached only when
  `FT_LOAD_FORCE_AUTOHINT` is set or `!FT_DRIVER_HAS_HINTER( driver )`.
- `src/truetype/ttdriver.c:652`: `TT_HINTER_FLAG` is
  `FT_MODULE_DRIVER_HAS_HINTER` under `TT_USE_BYTECODE_INTERPRETER` and `0`
  otherwise — so with the interpreter out, TrueType has no native hinter and
  autofits.
- `src/cff/cffdrivr.c:1187`: the CFF driver sets
  `FT_MODULE_DRIVER_HAS_HINTER | FT_MODULE_DRIVER_HINTS_LIGHTLY`
  **unconditionally**, and `src/cff/cffobjs.c:1139` defaults
  `hinting_engine = FT_HINTING_ADOBE`. Nothing about the interpreter switch
  touches it.

So the configuration runs **two hinters, split by format**: the autofitter
for TrueType, the Adobe CFF engine natively for OpenType/CFF — and it would
still be two with the interpreter on. `OS64_FONT_HINT_NONE`
(`FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT`) is the one setting that reaches
both.

This also kills one of my two arguments for the decision. I had claimed
turning the interpreter off bought "one hinter and one set of results"; it
buys no such thing. **The decision stands on parser surface alone**, which is
still a good argument — six of the post-tag safety fixes in the enabled
module set are in `ttinterp.c` — but the audit now says that is the only
argument.

**The consequence F5 should hear, which I had not spotted:** the same
typeface as a `.ttf` and as an `.otf` will not hint identically in os64, and
that is true whichever way the switch is set.

Corrected in:

| File | What changed |
|---|---|
| `userland/libfreetype/port/os64_ftoption.h` | the bytecode-interpreter comment, rewritten with the CFF exception |
| `userland/libfreetype/port/os64_ftmodule.h` | `pshinter` is named as the CFF hinter, not just "required by CFF" |
| `userland/libfreetype/UPSTREAM_REVIEW.md` | § "Which hinter runs, and why the interpreter is off" — now a table plus the source citations above; the config summary and the import table's two rows also fixed |
| `docs/fonts/F1-REPORT.md` | the configuration paragraph, and question 4 to F0, which said the opposite |

---

## 2. The -O0 evidence — reconciled, with the log

You were right about the mismatch and about its size. The saved `ft-O0.log`
was from an earlier build: I ran `-O0` **before** adding the cost pass and the
validator jump-counter checks, which are the twelve checks that make the
difference, and never re-ran it.

Re-run against the current tree. Both logs now say the same thing and are in
the worktree:

```
docs/fonts/f1-evidence/host-asan-O2.log    943 checks, 0 failures
docs/fonts/f1-evidence/host-asan-O0.log    943 checks, 0 failures
```

Each carries its own flag line, so the optimisation level is checkable from
the log rather than from my say-so.

---

## 3. The independent kerning reader — preserved as a tool

It only existed in a shell buffer. It is now
**`tools/font_kern_report.py`**, and its output is
`docs/fonts/f1-evidence/kern-fixtures.txt`.

```sh
tools/font_kern_report.py --fixtures                       # the pinned table
tools/font_kern_report.py --pairs AV,To --ppem 16,32 <font>…
```

It parses the table directory, `cmap` format 4, `kern` format 0, GPOS
coverage tables, class definitions, PairPos formats 1 and 2 and extension
lookups — straight from the OpenType spec, sharing no code with FreeType or
with `port/backend.c`. It reports which table each face's kerning came from
and converts to 26.6 at any ppem.

**One thing in it is deliberately NOT independent**, and it is marked
`GPOS_ACCEPTED_VALUE_FORMAT_*` in the source: FreeType 2.14.3's rule for which
GPOS pair subtables it consents to read at all (`valueFormat1 == 0x0004`,
`valueFormat2 == 0`). A reader that accepted more would report kerning the
engine does not apply and then call the engine wrong. When the pin moves,
that constant is what needs re-checking.

`fixtures/FIXTURES.md` and `libfreetype/README.md` both now point at it as the
way to re-derive the expectations after a fixture bump.

---

## 4. Artifact paths and boot coverage

**Paths.** Everything moved out of the session scratchpad into the worktree,
so the paths outlive the session. Absolute root:
`/home/yogi/src/os64/.worktrees/freetype-backend/`

```
docs/fonts/f1-evidence/host-asan-O2.log             943 checks, ASan+UBSan, -O2
docs/fonts/f1-evidence/host-asan-O0.log             943 checks, ASan+UBSan, -O0
docs/fonts/f1-evidence/kern-fixtures.txt            the independent kerning read
docs/fonts/f1-evidence/qemu-text-ext2.log           serial, text boot, ext2 root
docs/fonts/f1-evidence/qemu-gui-fat.log             serial, GUI boot, FAT lifeboat
docs/fonts/f1-evidence/guest-text-ext2-verdict.png  terminal specimen + PASS
docs/fonts/f1-evidence/guest-gui-fat-specimen.png   the window, 4 faces × 4 sizes
docs/fonts/f1-evidence/guest-gui-fat-verdict.png    the PASS line on that boot's VT1
docs/fonts/f1-shared-build.patch                    the shared-makefile change
docs/fonts/F1-REPORT.md                             the report
docs/fonts/F1-REPLY-R1.md                           this file
```

**Boot coverage — and you are right that I had it wrong.** The two runs used
two different roots:

| Run | Limine entry | `ROOT=` | Partition | Filesystem |
|---|---|---|---|---|
| text | `/QEMU Boot (ext2 root)` | `1ec5f5ab-…` = `EXT2_PARTUUID` | partition 2 | **ext2 root** |
| GUI | `/QEMU GUI Boot` | `2f4fd02e-…` = `DISK_PARTUUID` | partition 1 | **FAT lifeboat** |

Confirmed against the GUIDs in the root `GNUmakefile` and from the GUI boot's
serial log, which carries `BOOT: Root filesystem found, mounting` — the FAT
spelling, not ext2's. What misled me is that the entry is named
`/QEMU GUI Boot` and its own comment reads "QEMU Boot plus the optional GUI
subsystem"; it also carries a different `ROOT=`, which the name does not say.

The effect is that the library, the fixture and the font files were exercised
on **both volumes through both filesystem drivers** — more coverage than I
claimed, described wrongly, which is the bad kind of wrong.

**What that leaves uncovered, now stated in the report:** a GUI boot on the
ext2 root, and a text boot on the FAT lifeboat. The two runs cross rather than
overlap, so nothing here separates a filesystem-dependent failure from a
display-dependent one. Say the word if you want the two complementary runs;
they are cheap.

---

## Unchanged

The six contract questions at the end of `F1-REPORT.md` still stand, with
question 4 rewritten per item 1 above. Question 1 — **does `render` accept
glyph index 0?** — is still the one that blocks F2, since `.notdef` is where
every font keeps the missing-glyph marker.
