# F5 — Font discovery, configuration, persistence and settings

Frozen F0 contract: `23bf6dddfd1077bf844c661d8762a9b52e3a68f9` (backend table v1, R3 semantics).

Read [the design](../../FONTS.md), [the work plan](../../FONTS_WORK_PLAN.md), and
AGENTS.md. The parser/resolver starts after F0 freezes the configuration contract.
Live application depends on F3/F4's font-adoption and relayout operations.

The shared provider/replacement boundary is [FONT_PROVIDER.md](../../FONT_PROVIDER.md),
implemented by `os64/font_provider.h` and `os64/font_adopt.h`. Use its role sets
and prepare/barrier/commit/abort sequence. The assignment receipt identifies the
exact F2.5 base; the earlier F2 checkpoint alone does not contain these APIs.

## Scope and ownership

Own new font-configuration/discovery files chosen by F0, sample fonts.conf,
font asset/metadata installation rules, settings UI, and coordinated changes to
ui_theme.c/ui_session.c/ui_saved.c and Appearance Workshop where required by the
approved persistence design. The coordinator merges shared image/build edits.
Do not edit ui_text.c or gterm directly while F3/F4 own those integrations.

Read conf.h/conf.c, the configuration-ladder document, APPEARANCE.md's Apply/Save/
startup contracts, and the current theme schema. font.w/font.h are fixed legacy
metrics; changing their validation alone does not install a font or publish a
working font-size change.

## Deliverable

Resolve fonts.conf and font-file paths according to the frozen rules. Expose
interface/terminal/document choices through one provider shared by applications.
Bound discovery, validate candidate fonts without loading unbounded collections,
and distinguish a readable family/style label from the stable installed asset
identity. Keep previously loaded content valid after a file is replaced.

Follow [F0's metadata/hinting decisions](F0-F1-DECISIONS.md): labels are copied
UTF-8 with deterministic name selection and a documented lossy legacy fallback.
Duplicate family/style labels need filename disambiguation, not shared cache
identity. Judge the actual selected outline files at useful sizes: NORMAL uses
different hinters for TrueType and CFF, so a matching family label does not
promise matching pixels. Font format is determined by content, not suffix.

Installing an ordinary supported font must not require rebuilding programs.
Provide a repeatable install/refresh/select flow and a preview, along with usable
compiled defaults. Show missing, invalid or unsupported choices clearly and
retain usable rendering. Terminal choices obey the frozen monospace suitability
and glyph-coverage rules. License files accompany shipped default fonts.

Configuration remains usable without the settings app. Implement the frozen
startup/session/save contract, with legacy theme compatibility and independent
font roles. Do not create a competing transport or combine window-decoration
settings with font publication. Acquire replacement instances before adoption;
coordinate cache invalidation and relayout through F3/F4's interfaces. Do not
claim atomic global presentation when only publication is atomic.

## Required evidence

Host tests cover configuration precedence, missing/malformed/partial data under
the chosen policy, path resolution, duplicate display names, replacement of font
files, bounded discovery, unsuitable terminal fonts, allocation/I/O failures,
legacy snapshots and save failure. Test role independence and multiple contexts.

QEMU checks cover installing a new supported font, selecting and previewing it,
startup/reboot behavior, and live changes while Scribe contains unsaved edits and
gterm is running. Verify focus, selection, caret/scroll visibility, terminal grid
consistency, and understandable failure behavior at 1024x768. Report hardware
acceptance separately. Return evidence and schema deviations through
[HANDOFF.md](HANDOFF.md).

## R3 compatibility and rollout requirements

Implement the line-envelope protocol in FONT_CONTRACTS with ui_session: tolerate
well-formed unknown dotted keys, validate known keys and full palette snapshots,
and preserve unowned lines verbatim during generation-checked component updates.
Do not round-trip the envelope through a theme struct that drops future fields.
Palette, treatment and fonts writers replace their owned keys only. A conflict
requires re-reading and merging or reporting failure. Validate the complete
merged payload against 4096 bytes, including preserved lines.

Refresh to the compatible libos64, then reboot before publishing fonts.*. Old
processes retain their old library and cannot participate safely. Document this
first-upgrade requirement in the install flow and APPEARANCE.md. The tolerant
reader/preserving writer protocol supports later dotted-namespace extensions.

Test unknown future namespaces through font Apply, palette/treatment Apply,
Save and startup preservation; malformed/unknown-undotted keys; duplicate owned
keys; empty store; required palette omissions; conflicting generations; and
near/over-cap envelopes including long paths and preserved unknown settings.
Test the reboot boundary with a fresh compatible guest. Do not claim that an
old process becomes compatible merely because its library file was refreshed.

One bad fonts.conf line rejects all three roles, with a line diagnostic; role
independence means no inheritance. Resolve outline fallbacks at their role's
size. Publication success is not confirmation of adoption by every process.
