# E4 — try Roam

Implementation: `243663dc` on `codex/roam-e4`, based on `userland` merge
`5a521b63`. Worktree: `.worktrees/roam-e4`. [Design](E04_ROAM.md) and
[evidence](evidence/e04/README.md) describe scope and validation. P5 acceptance
and independent PR review are separate from host/QEMU checks.

## Build and install

```sh
cd /home/yogi/src/os64/.worktrees/roam-e4
make -C userland
python3 tools/os64serve.py userland/bin userland/bin/tests=tests etc assets/icons/default=icons
```

Use the normal P5 `os64get` workflow against that server. Install the matching
`roam` and `libos64.so`, plus `libimage.so` and its normal dependencies. Existing
`*.so` routing sends libraries to `/lib`; Roam uses the ordinary `/bin` route.
The PNG icon set uses the existing `@icons` route to `/etc/icons/default`.
No kernel update is required for E4 when E1K/E3 prerequisites are installed.

`etc/menu.conf` adds a Roam desktop-menu item. A private `/home/menu.conf`
overrides that whole file; add `item "Roam" /bin/roam` there if desired.

Launch on Os64:

```sh
roam /home
```

An omitted path uses the captured working directory. A relative path resolves
against it. Roam does not change the process working directory.

## P5 walkthrough

1. Browse a folder through Enter/double-click, Location, breadcrumbs and Places.
   Check both ext2 and FAT. File activation explains that opening files is not
   configured; it launches nothing.
2. Select a few rows, scroll, visit another folder and go Back. Selection and
   viewport should return when names still match. Refresh preserves matching IDs.
3. Use Ctrl+L, Ctrl+A, type a path and Enter several times. A missing path keeps
   the old committed address, rows and selection, with a visible error.
4. Ctrl+F opens the name filter. Escape clears it. Ctrl+H toggles dotfiles.
   Filtering searches the loaded directory snapshot, not descendants.
5. Hide each View-menu pane, including the menu. Alt or F10 reveals the menu;
   right-click offers Show Menu Bar. Ctrl+L and Ctrl+F reveal their hidden fields.
6. Change Interface font and palette in Appearance Workshop while Roam is open.
   Check menus, icons, navigation and the Details view, then resize the window.
7. Try repeated navigation and close while a large folder is loading. The UI
   should remain responsive and shutdown should finish without a task fault.

Back/Forward shortcuts are **Ctrl+[ / Ctrl+]** because Os64 uses Alt+Left/Right
for virtual terminals. Alt+Up goes to the parent; Backspace does so when Details
owns focus. F5 refreshes and Ctrl+Q closes.

A listing holds at most 10,000 entries and visibly reports when incomplete.
History keeps 32 paths, with selection/viewport preservation limited to eight
cached states and 64 MiB. Panes, favorites and column choices are session-local;
editable/persistent favorites and additional views belong to later slices.

After E4 review and hardware feedback, discuss **file associations with Chris
before designing E5**. This implementation does not settle their policy.
