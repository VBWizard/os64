# Font architecture review — brief for Fable

Review candidate R2, 2026-09-16. Chris requested that the coordinator reconcile
Opus's six F1 questions and prepare this design review. This file is the review
assignment to share; creating it does not mean a review has been sent or completed.

## Problem and expected behavior

os64 paints byte-oriented 8x16 cells today. The new feature lets users install
standard static outline fonts, select independent UI/terminal/document roles,
resize text, and use proportional Scribe text. A single positioned run must
drive measuring, drawing, carets, selections and scrolling so those operations
cannot disagree about where a character is. Scribe retains original file bytes.
The first language profile is Western UTF-8 and common symbols; its exact W1
repertoire and unsupported-input behavior are in the contract.

Please review whether the proposed boundaries and semantics can deliver that
behavior. Apply repository AGENTS.md's hazard-focused review rules. Prioritize
concrete correctness, ownership, integration and product-scope failures; this
is not a wording pass or a replacement for the later F1 implementation review.

## Checkouts and authoritative inputs

Both checkouts are based on `3b82356413ab8f183bd6506febe8d06fea6e7de0` and contain
uncommitted local work. Neither has been pushed for this handoff.

| Checkout | What to read |
|---|---|
| `/home/yogi/src/os64/.worktrees/font-design` (`codex/font-design`) | F0 authority: files below |
| `/home/yogi/src/os64/.worktrees/freetype-backend` (`opus/freetype-backend`) | F1 source/audit and runtime evidence |

Reading order in F0:

1. [FONTS.md](../../FONTS.md): accepted product scope and architectural context.
2. [F0-F1-DECISIONS.md](F0-F1-DECISIONS.md): the six dispositions and rationale.
3. [FONT_CONTRACTS.md](../../FONT_CONTRACTS.md): R2 ownership, metrics, text,
   encoding, configuration and invalidation rules.
4. [font_backend.h](../../userland/libos64/include/os64/font_backend.h),
   [text.h](../../userland/libos64/include/os64/text.h),
   [text_draw.h](../../userland/libos64/include/os64/text_draw.h).
5. [FONTS_WORK_PLAN.md](../../FONTS_WORK_PLAN.md) and
   [fixture instructions](../../tools/fonts/README.md).

In F1, read `docs/fonts/F1-REPORT.md`, `docs/fonts/F1-REPLY-R1.md`,
`userland/libfreetype/UPSTREAM_REVIEW.md` and `fixtures/FIXTURES.md` under that
library. Inspect `port/backend.c`, `port/os64_ftoption.h`, `shared.mk` and the
separate shared-build patch where needed to check architectural feasibility.
The F1 header is its copied R1 header; take candidate semantics from F0's R2
header. Its declarations remain compatible and the table revision stays 1.

`f0-review-inputs.json` in this directory records SHA-256 digests of the selected
inputs in both trees. It excludes itself. Before reviewing a changed working
tree, compare against that manifest and state which revision you actually read.
Use `python3 tools/fonts/verify_review_inputs.py` from F0 to check the local pair;
`--f1 /path/to/freetype-backend` selects another F1 checkout. For another machine,
transfer both source trees or their eventual commits along with these documents;
a branch name without the uncommitted files does not supply the inputs.

## Decisions to challenge

| Area | Question and concrete consequence |
|---|---|
| Dependency direction | Can a callback-driven leaf FreeType library sit below libos64 without cycles or hidden libc dependencies? Does unconditional loading impose an unacceptable cost on non-GUI applications? |
| Lifetimes and limits | Can owned glyph masks outlive faces while keeping their engine alive, and can F2's runs/cache share them without eviction bugs, leaked references or double-counted budgets? |
| Missing characters | Does F2's synthetic marker preserve visible diagnostics, byte spans and terminal cell width without relying on a font's .notdef image? |
| Fractional positioning | Do advances, pair adjustments, carets, fits, selections and paint rounding agree for kerning, trailing spaces, overhangs and repeated subpixel adjustments? |
| Encoding/editing | Is W1 an acceptable precise first slice of the approved Western UTF-8 scope? Can supported accents remain one editing cluster without altering saved bytes? Are unsupported/invalid inputs visible and editable? |
| Large input | Do the one-line limits and overflow rules preserve documents and report failure usefully? Is the planned UI response sufficient for a large unrenderable line? |
| Terminal | Can fixed-cell glyph clipping and existing PTY resize calls preserve grid/charset behavior, including refusal during a font change? |
| Names/selection | Can copied labels be displayed safely without becoming file or cache identities, including duplicates, malformed Unicode and fallback names? |
| Startup/live settings | Does the config ladder/path rule survive Save, and can a shared appearance envelope preserve palette, treatment and font roles under concurrent writers and preparation failure? |

The six F1 answers choose: reject glyph 0; limited GPOS coverage with ambiguous
zero; fractional kerning in both hint modes; interpreter-off TrueType with native
CFF hinting retained; explicit variable-table rejection; deterministic Unicode
display-name preference with a lossy legacy fallback. Review these as proposed
contracts. Do not infer approval from the fact that F1 already implements most
of them.

## Known boundaries requiring explicit verdicts

**Backend gate:** the narrow F1 API can freeze separately if its ownership,
format, metric, error and hinting semantics are sound. A backend freeze does
not authorize an unreviewed persistent schema or finish the whole feature.

**Layout gate:** F2 has declarations, a fake backend and golden expectations;
it has no production layout implementation yet. W1's finite accent strategy,
strictly increasing carets, zero-advance policy, run limits and cache ownership
deserve a verdict before editor implementation depends on them.

**Configuration gate:** the live appearance envelope remains a proposal.
Old writers do not know fonts.* fields, and old/new application coexistence has
not been proven. Cross-process publication is atomic, but font loading/relayout
can fail independently afterward. Decide a workable compatibility/rollout rule
before freezing this part. The payload remains capped at 4096 bytes. No new
kernel syscall or transport has been authorized or proposed by this package.

No evidence here proves a CPU-work bound for malformed fonts. Memory caps and
the upstream-fix audit are not a sandbox or an exhaustive security review.
No P5 visual/performance acceptance has been performed. Any proposal needing
kernel scope returns to Chris for discussion.

## Evidence and what it establishes

F0's command is `python3 tools/test_font_contracts.py`. In this environment it
uses `ASAN_OPTIONS=detect_leaks=0` because LSan cannot operate under ptrace.
ASan/UBSan remain enabled and the fixture counts allocations independently.
The candidate headers and fake backend compile with the target compiler. Fourteen
golden vectors have arithmetic consistency checks; they are acceptance targets
for the future layout engine, not proof that it exists or meets them.

F1's preserved evidence is under its `docs/fonts/f1-evidence/`: host logs report
943 checks at both -O2 and -O0; screenshots show text/ext2 and GUI/FAT guest
passes. The independent kerning reader reproduces its saved table. The import
verifier checks 334 files and 11 patches. The coordinator checked those logs,
screenshots, manifests and the independent reader; this does not substitute for
rerunning or reviewing the full implementation. The complementary GUI/ext2 and
text/FAT combinations have not been claimed.

The four `.log` evidence files are currently ignored by the repository-wide
`*.log` rule. Any eventual commit/transfer must explicitly retain them. The
input manifest checks their bytes even while Git ignores them.

## Return format

Return an architecture verdict for each gate: backend, layout, configuration.
For a blocker, give the concrete trigger/failure, affected contract/source,
reason it fails and a proposed correction. Distinguish a blocking defect from
an optional preference. State the input manifest/revision you used, any files
that changed during review, and evidence you actually ran.

Put the review in `docs/fonts/FABLE-REVIEW-R2.md` in F0, or return that text
through Chris. Do not edit implementation or silently revise shared contracts
while reviewing. Approval of one gate should say precisely which other gates
remain open. Merge/publication and the later implementation review are separate.
