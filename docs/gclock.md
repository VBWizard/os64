# Graphical clock

`gclock` follows Appearance Workshop's **Interface** typeface and colors.
Its configuration follows the normal search ladder: `/home/gclock.conf`
replaces `/etc/gclock.conf` with the default ladder.

```conf
Position = 280,10
SavePosition = true
Titlebar = off
Pinned = true
FontSize = inherit
Blink = true
```

- `SavePosition = true` saves the normal window position on a graceful close.
  Maximized coordinates do not replace the startup position.
- `SavePosition = false` preserves the configured `Position`, even after
  moving the clock. The close handler rereads the file, so changing this
  option while the clock is open takes effect on that close.
- `FontSize = inherit` follows the Interface size. Use an integer from 8 to
  96 for an independent clock size; `32` is a useful larger setting.
  With the built-in face and an override other than 16, the clock uses the
  included `/etc/fonts/DejaVuSans.ttf`. A missing/unusable font is logged and
  the current face is retained (the built-in face on initial failure).
- `Blink = false` keeps the colons visible. Digit slots retain their positions
  with proportional typefaces and during blinking.

AW changes apply live. `FontSize` and `Blink` edits take effect on restart.
The clock measures text before creating its content-sized window, so installed
decorations do not need hardcoded titlebar offsets. Live font changes grow
the minimum size when necessary. Smaller fonts lower that minimum without
shrinking a window the user may have enlarged; resizing recenters the display.

Close saves titlebar and pin state regardless of `SavePosition`. The existing
personal file retains comments and untouched settings. The first personal
save seeds the effective settings so shadowing the system file does not lose
the configured position, size, blink, or save policy. Forced termination does
not run the close handler.

## Focused regression checks

`python3 tools/test_gclock_host.py` exercises production clock callbacks,
libui, FreeType, and the preserving config writer under ASan/UBSan. It covers
position policies, first personal save, edits made while running, maximized
close, malformed values, live font adoption, refused growth, recentering,
blink clearing, and independent size selection. Application allocations are
balanced at teardown. On ptrace-based runners, use `ASAN_OPTIONS=detect_leaks=0`
because LeakSanitizer cannot inspect that environment.

For guest verification, build the gclock target in `userland/GNUmakefile`,
install the binary into an isolated QEMU image, and exercise move/close/reopen
with both position policies. Check the ELF relocation table as well: the
clock obtains its label callbacks through a constructed widget to avoid an
unsupported `R_X86_64_COPY` data import.
