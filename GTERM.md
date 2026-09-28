# gterm

gterm is the graphical terminal. It runs husk by default, or a program supplied
as an absolute path, for example `gterm /bin/top`. It follows Appearance
Workshop's **Terminal** font. Its scrollbar and settings controls use the
shared widget theme; the settings window follows the **Interface** font.

## Scrollback

The right-hand scrollbar browses retained output. Drag its thumb or click the
track to move a page. The mouse wheel moves three rows per notch.
Shift+Page Up/Down moves a page; Shift+Home goes to the oldest retained row and
Shift+End returns to the live prompt. These keys work with PS/2 and USB keyboards.

At the bottom, the view follows output. When browsing, it stays on the same
logical row while output arrives. If that row ages out, the view moves to the
oldest remaining row. Typing or right-click pasting returns to live output.
The cursor is shown only in the live view. Dragging selects and copies visible
cells, including historical output. Changing the view or replacing its snapshot
clears selection.

History stores physical terminal rows, including wrapped rows, with their cell
colors and character sets. Width changes preserve left columns without text
reflow. A geometry change returns to the live view. Resizing below the cursor
moves top rows into history so the current line remains visible.

## Terminal Settings

Click the drawn slider icon in the titlebar, or press **Ctrl+Alt+S**. The shortcut
also works with a hidden titlebar or an older composition without Settings.
Clicking again restores/focuses the existing dialog without losing its draft.
Output and paste continue while the dialog is open.

`ScrollbackLines` controls the number of additional rows retained above the live
screen. The default is **2000**; **0** disables retention and **10000** is the
maximum. The value is independent of window height. The dialog shows the
approximate history cell-memory cost at the current column count.

- **Apply** changes this terminal immediately.
- **Save as default** applies the value and writes it for newly opened terminals.
- **Close** discards unapplied edits. Applied changes remain in this terminal.

Reducing the limit discards the oldest rows; increasing it cannot restore them.
Other open terminals keep their limits. If allocation fails, the old capacity
and cells remain intact. If saving fails after Apply succeeds, the dialog says
so explicitly.

Configuration uses the normal search ladder, typically `/home/gterm.conf` before
`/etc/gterm.conf`:

```ini
ScrollbackLines = 2000
```

Saving uses the shared configuration writer, preserving unrelated settings and
comments. No scrollback contents or viewport position are written to disk.

## Implementation

The kernel's GRID PTY ring owns retained output, including bursts between GUI
frames. `os64_pty_history` sets retention; `os64_pty_viewport` returns a coherent
viewport plus logical row IDs and a geometry epoch. Existing live snapshots
retain their 32-byte header and behavior; STREAM PTYs refuse history operations.
VT retention is unchanged.

The [kernel contracts](TERMINAL_CONTRACTS.md) define the 128 MiB retained-row
quota and distinct quota, memory, and contention refusals. Live grids are exempt
from the history quota; a new terminal can start with no history when its
requested capacity is refused. Settings reports the refusal reason and keeps
the current limit. Ordinary physical memory exhaustion can still prevent a
terminal from opening.

Settings are an app opt-in titlebar action, not kernel-owned preferences. The
window manager emits `OS64_GUI_EVENT_SETTINGS`; gterm supplies the fields and
persistence through `os64/ui_settings.h`. Frame Studio provides placement,
Square/Round/Bare housing, fill, and active/inactive ink. Unsupported applications
omit the slot and its automatic spacing. V4–V6 compositions load unchanged;
V7 adds Settings. See [FRAME_STUDIO.md](FRAME_STUDIO.md).

Build and runtime evidence: [validation record](docs/gterm/VALIDATION.md).
