# os64get: replacement backups and contained temporary files

For commands, server setup, configuration, and recovery, see the
[os64get and os64serve user guide](OS64GET.md). This document records the
installation design and its validation.

Implemented in userland on the `userland` checkout based at `6e52bd0`.
Whole-system installation with `os64get -a` is the primary workflow
(about 95% of use). No kernel changes are part of this work.

## Desired behavior

- A download to an absent destination creates no archive entry.
- Replacing different contents preserves the existing destination's bytes in
  the configured archive before publishing the download.
- An unchanged file creates no archive entry. `-f` may force a download, but
  should not create a redundant backup when the verified contents match.
- `-n` explicitly disables replacement backups. Configuration lookup uses
  the system search path, with `/home/os64get.conf` ahead of
  `/etc/os64get.conf` by default.
- Successfully installed incoming bytes have no retained staging or archive
  copy. Old files are archived under their destination paths, not server names.
- Handled failures and Ctrl+C during preparation clean up incoming temporary
  files across the whole run. Crashes leave them in designated scratch
  directories, with a bounded place to look for cleanup.

HTTP/HTTPS downloads do not make replacement backups or consult the install
configuration. They share managed staging, verification, publication, and
cleanup with valet installs; archive policy applies to valet installs only.

## Staging placement

Before this change, the client downloaded beside the destination and copied
the new file into the archive. That behavior retained incoming files rather
than replaced originals.

Use `/tmp/os64get/<run>/` when `/tmp` and the destination resolve to the same
filesystem. Otherwise use `<destination-mount>/.os64get-tmp/<run>/`. For the
usual layout this means `/tmp/os64get`, `/home/.os64get-tmp`, and
`/fat/.os64get-tmp`. Download directly into the selected scratch directory;
do not first spool a large `/home` download onto the smaller root partition.

Read the mounted filesystem report, normalize relative paths against cwd,
and match mount prefixes at component boundaries, following the kernel's
resolution rules. Verify the scratch location as well as the destination:
`/tmp` could itself become a separate mount. Refuse if a suitable writable
scratch area cannot be prepared; do not fall back to truncating the target.

Claim run directories with atomic mkdir and files with exclusive `open("x")`.
Scratch run names use a task ID and collision-retried counter; archive run
names add a UTC timestamp. Exclusive creation supplies uniqueness. Track the actual paths created in
a per-file record. Never reconstruct cleanup paths from `DEST + ".part"`.
Reserve managed scratch paths against use as installation destinations, and
account for filesystem lookup aliases, including FAT case folding.

Keep this mechanism small: unique names and recorded ownership of temporary
files, without application locks, stale-lock recovery, or automatic scavenging.
The names prevent another run from changing a file after verification or
deleting it during cleanup; supporting competing installations is not a goal.
Ordinary success, errors, and cancellation reclaim this run's scratch files.
Crash leftovers remain until manually removed or a separate cleanup feature
exists; unique naming does not solve that remaining storage issue.

## Backup and publication

1. Resolve the destination and scratch paths. Receive, validate, sync, and
   close the incoming file, retaining each protocol's current validation.
2. Inspect the destination after the transfer. If it is absent, publish using
   `OS64_RENAME_NOREPLACE`; if a file appears meanwhile, stop rather than
   overwrite an unbacked-up file. A stat error is not proof of absence; the
   no-replace operation provides the final protection in this branch.
3. For an existing regular file with different contents, copy that file into
   scratch space on the archive filesystem. Measure its own length and CRC;
   the incoming file's advertised values do not describe this backup. Sync,
   close, reopen, and verify the copied backup before finalizing it.
4. Finalize the backup with a no-replace rename into, for example,
   `<archive>/YYYY-MM-DD/HHMMSS-<unique>/bin/prog` for destination `/bin/prog`.
   Preserve destination-relative paths to distinguish equal basenames and
   explicit renamed destinations. Create the archive run lazily, when an old
   file actually needs backing up. Reject unsafe overlaps with archive and
   scratch trees; enforce the kernel's path-length limit before writing.
5. Publish the incoming file by a same-filesystem rename into its destination.
   Then remove empty scratch directories. No incoming data is copied into
   the archive. Backup failures prevent publication of the associated file.

Keep a completed old-file backup if publication fails or is interrupted.
It is useful recovery data, even if replacement did not finish. Cleanup must
distinguish a provisional backup from a finalized one. The archive is a set
of originals preserved for replacement, not a claim that every replacement
completed. Report the retained backup path when publication fails.

## Whole-system installation and Ctrl+C

Structure `-a` as three batch phases, with run-wide cleanup:

1. Resolve and check the manifest, skip unchanged destinations, and receive
   and validate the incoming files. Reject duplicate or filesystem-equivalent
   destination mappings before staging.
2. Prepare and verify the required backups of old destinations. All required
   downloads and backups must be ready before the first installation rename.
3. Publish the prepared files with same-filesystem renames, then clean up.

An error during either preparation phase prevents publication of the batch.
Ctrl+C in either phase stops further work, closes open handles, and removes
the whole run's incoming scratch files and incomplete backup files, including
files staged earlier in the batch. Finalized old-file backups remain.

Install a SIGINT handler that sets a cancellation flag. Check it in manifest,
receive, checksum, copy, and verification loops; treat OS64_INTERRUPTED as
cancellation when the flag is set. Close handles and remove files from normal
control flow, not from the signal handler. Do not continue to the next file
after cancellation, or turn cancellation into a retry of the interrupted I/O.

Check cancellation immediately before beginning publication. Once publication
has started, defer requested cancellation through the short rename phase and
cleanup, so Ctrl+C does not deliberately stop a refresh halfway through its
installed files. Keep the handler installed during cleanup so repeated Ctrl+C
does not bypass it. Report the actual installed/failed counts and any deferred
cancellation. A publication error still needs honest partial-install reporting
and cleanup of unpublished incoming files; finalized backups are preserved.
Commit remains a sequence of per-file renames, not an atomic system update;
a crash or forced termination can still interrupt it.

Check for destination changes during backup and before publication. These
checks do not provide an atomic compare-and-replace against arbitrary writers:
files actively being edited and overlapping installations require coordination.
No filesystem snapshot or universal writer lock is available through the
current interface. Adding a concurrent-installer protocol is outside this slice.

## Cleanup and filesystem limits

Use one cleanup path for network, checksum, write, sync, close, backup, and
publication failures in both download modes. Single-file and URL downloads use
the same cancellation and cleanup rules as the batch path. Report failed
removals and their exact paths; do not silently claim a clean run.

A forced termination, crash, or power loss cannot execute cleanup. Automatic
abandoned-run cleanup and a cleanup subcommand are separate future work, not
prerequisites for replacement backups and handled-failure cleanup. Do not guess
that old-looking names or timestamps prove abandonment.

Future boot cleanup must cover the managed scratch directories on each mounted
filesystem, not just `/tmp`; mounts unavailable at boot need cleanup when they
become available. Boot cleanup itself remains a separate feature. Existing
scattered `.part` files and old archive entries should not be deleted by a
filename glob: they lack reliable ownership metadata.

Ext2 supports replacing a file by rename without a missing-name window. Its
filesystem is not journaled, and file sync is not a guarantee against drive
cache loss on power failure. FAT's existing replacement removes the old name
before renaming the new one. A verified backup improves recovery but does not
remove that gap, especially for boot files. Preserve current FAT update support
in this slice and state this limit; requiring atomic replacement would instead
refuse existing FAT targets. Changing that behavior needs a separate decision.

## Reviewable implementation slices and evidence

1. Introduce explicit stage records, exclusive scratch creation, mount-aware
   placement, and run-wide cleanup and Ctrl+C handling, led by the `-a` path.
   Cover URL staging as well as valet staging.
2. Change archive contents to verified copies of old destinations, including
   path-preserving archive names and batch preparation. Update help, config,
   source comments, and relevant DEBTS.md claims in the same changes.
3. Validate the batch workflow and its failure/cancellation boundaries in the
   guest, then exercise the single-file and URL paths with the same helpers.

The current merged HTTP parser is retained. The installer and cancellation
checks wrap the existing body validation rather than changing HTTP framing.

Use host filesystem fixtures with injected failures, followed by builds and
guest checks on ext2 and FAT. Essential cases: absent target (including repeated
delete/download of the large fixture); replacement of a locally edited file;
unchanged and forced-identical fetches; `-n`; same basename at different paths;
FAT aliases; temporary/archive collisions; full disks and failed backup reads;
batch preparation and commit failures; Ctrl+C during manifest retrieval, a later
download after earlier files staged, backup copying, verification, and cleanup;
and deferred Ctrl+C after publication begins. Verify that a cancelled batch
leaves its installed destinations unchanged when publication has not started,
and removes incoming scratch files on each involved filesystem. Repeat Ctrl+C
to check that cleanup completes. Crash tests should show contained leftovers,
without claiming automatic reclamation. Lead success coverage with `-a` across
root, /home, and FAT destinations and include replacements of running programs.
Verify bytes at the installed and archive paths, not just success messages.

## Verification

`tools/test_os64get_host.sh` passes 29 scenarios using the production application control flow with
host filesystem and transport adapters under ASan/UBSan (leak detection disabled
for the execution environment). It covers successful and cancelled batches,
new and unchanged destinations, forced identical downloads, backup read/write/
sync/close failures and corruption, partial publication, FAT alias conflicts,
archive/scratch aliases, no-replace races, unsafe valet names, and single-file
and URL success/failure/cancellation. HTTP and proxied HTTPS replacements leave
no archive directory; HTTP also succeeds with an unusable archive path.
Repeated SIGINT requests during cleanup
and an interrupted first publication rename are included.

The strict userland and full image builds pass, as do the existing HTTP and
gzip host suites. `git diff --check` passes. The stale-reference scan's server
and CRC-test hits were checked: `tools/os64serve.py` and
`tools/test_crc32_host.c` still exist; these are live references.

Private QEMU tests use copied root and home disks, virtio networking, and a
single-instance runner because this boot launches two shells. A batch targeting
`/bin`, `/home`, and FAT preserved the three original text files (18 bytes
combined). A new 176 MiB file downloaded twice, with deletion between runs;
both runs exited zero, the final file matched the host source byte for byte,
and the archive still contained just the three originals. Scratch directories
were empty on all three filesystems and both ext2 filesystem checks were clean.
The large-transfer fixture extends the host server's send timeout to allow
TCG emulation; this does not change the production server's timeout.

A further guest batch replaced the running os64get executable with a valid
copy carrying one appended byte, then successfully ran that installed copy.
Its backup matched the original binary. A forced batch was interrupted through
`/proc/<pid>/ctl` after an earlier incoming file had staged: exit status 130,
installed files preserved, and scratch directories empty on root, home, and FAT.
The host harness covers the precise defer-cancellation boundary during commit;
the guest test covers real SIGINT delivery and run-wide preparation cleanup.

Chris also tested valet installs against several worktrees on the P5 and
reported that the installer behaved as documented. His subsequent webpage
download prompted the separate URL policy: HTTP/HTTPS downloads omit backups.
The 29-scenario host run and strict userland build include that correction;
the guest installer evidence above predates it.

Boot/crash reclamation and concurrent-installer serialization remain outside
this change. Filesystem power-loss atomicity has the limits described above.
