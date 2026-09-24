# Included composition collection

2026-09-24, `codex/frame-collection`, based on `userland` at `ad6e8159`.

Six prepared compositions ship in `frames/` and install into `/etc/frames`
on ext2 and FAT. Frame Studio merges included and personal entries by name,
marks included entries, and saves edits into the personal collection.
See [the collection contract and P5 command](../../FRAME_COLLECTION.md).

![Production-rendered active and inactive samples](collection/preview.png)

## Verification

- Full strict `make -j8` passed, including userland, kernel, both filesystems,
  and the bootable ISO. The final incremental build had no compiler warnings.
- The [ASan/UBSan decoration and storage suite](collection/host.txt) passed,
  including discovery, source-pinned loading, active matching, personal
  shadowing, corrupt/missing personal-file refusal, deletion revealing an
  included original, collection bounds, and protection when the config target
  aliases `/etc/frames` through repeated slashes, `.` or `..` components.
  Leak detection was disabled because LeakSanitizer cannot run under this
  sandbox's tracing restrictions; address and undefined-behavior checks ran.
- The [sanitized generator check](collection/generator.txt) rebuilt the six
  files byte-for-byte with the pinned production FreeType port, preparation,
  encoder, decoder and painter. Normal OS builds use the checked-in assets.
- [Installation and serving checks](collection/install.txt) compared all six
  ext2 and FAT copies with the source bytes. The actual server's LIST and GET
  handlers, driven with an in-memory connection, returned the `frames` lot,
  matching lengths, CRCs and bytes. This was not a network transfer to the P5.
- QEMU used eight cores, 1920x1080, and private disk copies. The Saved page
  discovered the included collection. Loading Orchard enabled its preview and
  disabled Delete. Saving made a [personal copy](collection/personal-copy.png);
  deleting it [revealed the original](collection/original-revealed.png).
- Applied [Blue Hour](collection/blue-hour.png) and [Orchard](collection/orchard.png)
  to the live window frame. Closing and reopening Studio
  [matched the active included Blue Hour](collection/reopened.png).
  The [guest log](collection/guest.txt) records both Apply generations and clean
  exits. A fresh boot of the final binary also discovered the included files;
  its [Saved page](collection/final-list.png) and
  [clean exit](collection/final-guest.txt) are recorded separately.
- After the save/delete session, read-only `e2fsck -fn` on the guest's private
  home filesystem completed with no errors. `git diff --check` and
  `tools/stale_refs.sh` passed.

The P5 command and routing rule are prepared; hardware installation and a
Windows-hosted network transfer remain for the user's normal update workflow.
