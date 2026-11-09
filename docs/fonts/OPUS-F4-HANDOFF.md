# Opus assignment — F4 widgets and Scribe

**Ready to begin on this machine.** Implement F4 against the committed F2.5
provider/adoption foundation below. This assignment does not depend on chat.

## Exact branch point

- Repository: `/home/yogi/src/os64`; remote `https://github.com/VBWizard/os64.git`.
- Foundation commit: **788de9900282e941a7a93aa110ffa69154daa066**.
- Foundation branch: **`codex/font-provider`**. The commit above is the base;
  its branch may gain later handoff/review records. Use the pinned commit.
- Your branch: **`opus/font-widgets`**.
- Your isolated worktree: **`/home/yogi/src/os64/.worktrees/font-widgets`**.
- Prerequisites already included: accepted F2 `4b0a839` (implementation `32d8243`),
  reviewed F1 `f065c7c`, and frozen F0 `23bf6dd` plus receipt `0ab3cee`.
- F2.5 is committed locally and available through the shared repository. It has
  coordinator verification; independent review/remote publication are separate.
  Do not assume the new branch has already been pushed.

From the repository root:

```sh
git show --no-patch --oneline 788de9900282e941a7a93aa110ffa69154daa066
git worktree add -b opus/font-widgets .worktrees/font-widgets 788de9900282e941a7a93aa110ffa69154daa066
cd .worktrees/font-widgets
```

If the named branch/worktree exists, inspect it and coordinate reuse; do not
reset or remove someone else's work. **Do not branch from `userland`, the older
F2 tip `4b0a839`, or the F1 worktree.** Those do not contain this complete shared
boundary. The foundation has the runtime interfaces and F4 packet; this handoff
is an assignment receipt committed immediately afterward on the coordinator
branch. Keep this document available while working on the pinned foundation.

## Read before implementation

Read repository AGENTS.md, FONTS.md, FONTS_WORK_PLAN.md, FONT_CONTRACTS.md,
FONT_PROVIDER.md, docs/fonts/04-widgets-editor.md, docs/fonts/FABLE-REVIEW-F2.md,
and docs/fonts/F25-REPORT.md. The F4 packet is the detailed acceptance checklist;
this assignment supplies its branch, ownership and integration decisions.

The reviewed F0 backend/text headers stay frozen. F2.5's public headers are
`os64/font_provider.h` and `os64/font_adopt.h`. They have implementations, not
just declarations. Use `os64_font_context_create`, `os64_font_set_prepare/view`,
set retention/release and `os64_font_adopt`. The default context factory selects
FreeType inside libos64; Scribe need not link the backend directly.

## Scope and ownership

You own font-related changes in:

- `userland/libos64/ui.c`, `ui_controls.c`, `ui_list.c`, `ui_text.c`, and related
  new/internal UI helpers. Keep textfields and textviews under your ownership.
- Additive UI integration changes in `userland/libos64/include/os64/ui.h` and
  `userland/libos64/ui_internal.h`: runtime font bindings, application layout
  planning and a public way to obtain/use your generic consumer descriptor.
  Keep legacy theme `font.w/h` fixed at 8/16; do not change their schema.
- `userland/apps/scribe/` for editing, document/help binding, layout, source-byte
  positions, scrolling and font-adoption integration.
- Focused host/guest tests and your F4 report/evidence under `docs/fonts/`.

You may make narrowly necessary userland/GNUmakefile additions for your own
sources/tests on this isolated branch. List those shared hunks in your report;
Quinn owns final build/image integration. Other application layout migrations,
root image rules and draw.h changes require a named coordination extension.

Do not edit gterm (Quinn/F3), the new provider/adoption implementation or headers
(Quinn), F0 text/backend/draw headers, the rasterizer/cache, ui_theme.c,
ui_session.c, ui_saved.c, font configuration/discovery or Appearance Workshop
(F5), kernel, syscalls or PTY ABI. Bring a concrete interface deficiency back to
the coordinator rather than forking an API or hiding it behind private headers.
Normal F4-owned implementation choices do not require repeated approval.

## What F2.5 supplies, and what you implement

The provider supplies immutable UI, terminal and document roles, borrowed F2
font lists, primary metrics and identities. It appends builtin fallback and
validates the terminal role. You must not build a competing font loader/cache,
repeat terminal validation or reach into text_internal.h. Test harnesses may
read fixture files with the existing bounded reader and supply bytes to the
shared constructor. That is test injection, not an alternate production config.

UI labels/buttons/list/menu captions/textfields use the UI role; textviews and
Scribe's document/help use DOCUMENT. Initialize builtin defaults through the
shared provider so legacy startup remains usable. Use injected outline sets for
tests and demonstration until F5 supplies resolved production choices. Do not
add a private fonts.conf parser, environment variable or persistent test selector.

Implement the **whole-window consumer adapter**, including Scribe's application
layout. F5 obtains an `os64_font_consumer_t` from your UI/application integration
and calls the common adoption coordinator. The adapter's concrete initializer
and internal plan types belong to F4; expose the initializer in your UI API and
identify it in the report. The callback signatures and lifetime rules are fixed
by font_adopt.h, so F5 need not depend on your private plan representation.

Prepare stages runs, caches, bounds, metrics and any app-owned layout together;
it retains the candidate, leaves active state alone and cleans up partial work
on failure. Do not use the existing mutating resize callback as a prepare hook.
Provide an F4-owned layout-planning hook/plan as needed. Commit transfers the
candidate reference, swaps staged state and cannot allocate or fail. Abort frees
the plan and retained references. Preserve focused widget, document bytes,
legal selections and caret; clamp scrolling and restore visibility on success.

Primary row pitch stays fixed when a fallback/marker is taller. Clip to the row;
use F2 selection X with the row's Y. A nominal size is not a row height. Do not
change live widget geometry or actual window minimum/size before preparation
succeeds. FONT_PROVIDER.md defines the optional final external barrier and its
failure rule. If layout cannot fit the current content area, preserve the old
state and report the failure; still deliver an intentional usable layout for
the packet's supported larger-font 1024x768 cases.

## Completion gates

Complete the F4 packet, including the difficult cases rather than a visual-only
migration:

- Measurements, painting, caret/hit-testing and selection use the same F2 runs.
  Test kerning, overhang, blank/trailing-space rows, tabs, composed/decomposed
  accents, unsupported marks, malformed bytes and fallback.
- Document positions remain original byte offsets. Exercise editing/deletion,
  vertical remembered pixel X, horizontal pixel scrolling, formerly widest lines
  shrinking, help-view vtable swaps, find/selection and allocation failures.
- Font changes with unsaved edits preserve bytes, focus and selections; failed
  changes retain a usable old layout. No full-document relayout on each keypress.
- Lines over 1 MiB and clusters longer than a run use bounded source windows or
  the specified diagnostic placeholder. Test navigation, splitting, copy/delete
  across omitted bytes and byte-identical unchanged save. Any editor-owned
  boundary scanner must match W1 and be tested against F2 legal boundaries;
  do not treat a truncated run as the entire document line.
- Run existing widget/editor regressions, focused host tests, strict build,
  diff/stale-reference checks and real QEMU interaction with proportional and
  monospace faces at multiple sizes. Show unsaved-edit switching and refusal.
  Host fixtures alone do not prove the UI works in the guest.

Return `docs/fonts/F4-REPORT.md` with source/commit identities, API initializer
names, cache/window invalidation design, test commands and separate host,
cross-build, QEMU and hardware evidence. List remaining application consumers
rather than claiming the entire desktop has migrated. Retain artifacts in
`docs/fonts/f4-evidence/`, not a session scratchpad.

## Delivery and coordination

Quinn implements F3 in a separate worktree from the same F2.5 foundation.
F3/F4 share the provider and callback contract; neither waits for F5's settings
UI to begin.

Deliver a local committed implementation on `opus/font-widgets`, with the report
and evidence. Ask for review when ready. The standing publication workflow is to
commit the approval record and push once the contents are approved; no merge is
authorized by this handoff. Do not rewrite the prerequisite history. For access
from another machine, ask Quinn to publish the exact foundation first rather
than silently substituting an older remote base.
