# Fonts in os64: the useful overview

A font file describes how letters should look. os64 now has the other pieces
needed to turn those letters into editable text: sizing, spacing, drawing,
selection, scrolling, and settings that applications can change safely.

There are three independent choices:

- **Interface:** buttons, labels and other libui controls.
- **Terminal:** gterm's character grid; this needs a genuinely fixed-width font.
- **Document:** Scribe's editable text and help, including proportional fonts.

Appearance Workshop's **Fonts** page lets you choose a role, a font and a pixel
size, then see all three roles in the preview. **Apply to session** publishes
those choices for running applications. **Save** writes next-startup choices;
it does not apply them. Use both when you want both effects. Fonts and colors
have separate drafts, and changing one component preserves the others.

The built-in 8×16 font is the dependable default. It has one size. The shipped
DejaVu Sans and DejaVu Sans Mono files are scalable, and their license ships
with them. The first release supports ordinary TrueType and CFF OpenType fonts;
the provider recognizes their contents, rather than trusting the extension.
This is Western UTF-8 text and common symbols, not full multilingual shaping.
Scribe preserves the original file bytes, including text it cannot display.

To add a font, enter its full guest path in **Install a font file**, then press
Enter or **Install font**. Workshop validates it and copies it into `fonts/`
at the top of the configuration ladder, normally `/home/fonts/`. It refuses an
existing destination name. Select the installed face and size, preview, then
Apply and/or Save. **Refresh fonts** combines `/etc/fonts` and `/home/fonts`,
plus a custom installation folder, the folder next to the selected `fonts.conf`,
and files already selected by any role. Saving personal settings does not hide
system fonts. Identical file contents appear once, even under different names;
different versions remain separate. A configured copy is preferred, otherwise
a personal copy is preferred to its identical system copy. Existing configured
paths are preserved. Folders are not searched recursively.

The advanced configuration file is `fonts.conf`. The first file on the normal
configuration ladder wins, usually `/home/fonts.conf` before `/etc/fonts.conf`.
Relative filenames are relative to that file's directory. Saved choices use
absolute filenames so moving the configuration does not change what it selects.
Each role may specify two fallback files in configuration. The UI edits the
primary font and size; it preserves those fallback choices.

If a file or setting is invalid, the whole candidate is refused. An application
keeps its old usable font if it cannot prepare the replacement. Scribe measures
and rearranges its controls before switching; gterm also has to resize its PTY
successfully. Fixed-layout tools keep their old font when the new text rows will
not fit. Applying publishes a request; it does not make every window change at
exactly the same instant. Apply again to retry a refused change or explicitly
reload a font whose file was replaced.

For the first installation of this feature, refresh the compatible userland and
**reboot before using font Apply**. Already-running programs keep the library
with which they started. Later font-file installations need no program rebuild.
The kernel console, desktop/window-title text and direct bitmap drawing apps are
outside this feature; window decorations remain a separate project.

## What the pieces do

| Piece | Responsibility |
|---|---|
| F0 contracts | Define what each piece promises the others. |
| F1 FreeType backend | Read supported font files and rasterize individual glyphs. |
| F2 text engine | Decode text, measure and draw runs, and keep bounded glyph caches. |
| F2.5 font provider | Supply the three roles and coordinate safe replacement. |
| F3 terminal | Fit fixed-width glyphs to the terminal grid and PTY dimensions. |
| F4 widgets and Scribe | Use the same measurements for drawing, editing and selection. |
| F5 settings | Discover/install fonts, preview choices, publish changes and save startup settings. |

A replacement is prepared before it becomes active. Old text retains its old
font objects until it is finished with them; new text receives new objects.
That is why overwriting a font file cannot silently change the meaning of a
selection or invalidate text that is still being drawn.

Technical handoff: [F5 report](F5-REPORT.md), [implementation decisions](../../FONT_SETTINGS.md),
[provider contract](../../FONT_PROVIDER.md), and [combined review packet](FABLE-FONTS-REVIEW.md).
