# Porter — the Os64 package installer

Design draft after the second review, 2026-11-24. Chris approved one active system preview on a
baseline, with the guest computing the delta. Independent media collections
have their own histories. The CLI ships first and remains the text-mode
recovery interface. **Porter** is the desktop name and `porter` is the shell
command. Storage, build and kernel mechanisms below are engineering proposals
for review. Implementation references use `userland` at `d001ae151`.
[First architecture review](INSTALLER_REVIEW.md)

## The workflow

```sh
make package NAME="Audio preview"
make package
make package BASELINE=1 NAME="November userland baseline"
```

Build a branch as often as needed. Only the explicit package command publishes
to the shared repository outside the checkout. A missing label defaults to the
branch and the date/time with its timezone. Record revision, dirty state,
toolchain, input rules and publication time. The snapshot remains usable after
the worktree disappears.

On the guest, choose a target from **Available**. See the installed and target
builds, additions, replacements (including downgrades), retirements, conflicts,
transfer and backup sizes. One confirmation applies the plan. **Installed
history** shows the transactions on this machine. Undo the third system
transaction and Porter unwinds the newest three system transactions in order.
A theme or recording installed in the meantime stays in place. History, undo
and recovery work offline.

## Full inventories and small transfers

The publisher enumerates the complete installable output for the selected
scope using Make's inventories for programs, aliases, libraries, fixtures and
assets. For each logical entry record the lot, relative path and kind. For a
regular file record length and SHA-256 of its exact installable bytes. Include
empty directories. Freeze a consistent build and asset selection before
publishing. Detect files changing during collection. An unreadable selected
source fails publication. An omitted optional collection is outside scope.
A successfully scanned empty collection can retire its previously managed
files after preview.

A system target covers the coupled programs, libraries and required runtime
data from one build. Its manifest declares the kernel contract. Themes, clips
and other independently usable content can have separate scopes. A lot chooses
a route. Changing a library's lot cannot free it from its system's compatibility
requirements. Publishing a feature that requires a specific asset must include
it in the coupled scope or declare and check the required identity.

A fresh worktree may rebuild all 235 userland ELF files. If only one file has
new contents, the repository needs one new compressed object. The guest first
gets the target manifest, then requests the bytes needed for its own plan.
Publishing a branch or integrated `userland` uses the same operation. Neither
requires a merge-base build, a diff against the last publication or a declared
preview predecessor.

**Three-way comparison.** For every managed path in scope, compare the last
receipt's expected identity, the actual file on the mounted volume and the
selected target. Absence and directory are distinct identities. Verify actual
bytes even for apparently unchanged files. Recheck approved preconditions
before writing. The following example starts with an audio branch which
changed only `libsound.so` when it was built.

| P5 before the install | What Porter previews |
| --- | --- |
| The other 234 files match the target and the receipt. | Fetch and replace the sound library alone. |
| A newer, recorded userland also updated `libimage.so`. | Install the new sound library and restore the older image library specified by the audio build. Both changes are known. Select this target directly. |
| Someone replaced the image library using os64get after the recorded install. | Report the local change and stop before fetching or backing up. Restore the receipted bytes to retry, or deliberately adopt a baseline that lists the file. |
| The selected target is a theme collection. | Inspect the theme scope. The system library is outside its inventory. |

A first install or explicit re-adoption uses a baseline. Show its lots,
unmanaged names already present in managed directories, and differing local
files. The user can authorize replacing the listed paths. Back up the actual
originals. Preserve unrelated files, and retire a previously managed entry
only with valid history, matching bytes and an explicit removal in the plan.
A missing or stale history cannot justify removal. First-baseline undo restores
the original mixed contents and relinquishes ownership of adopted paths.

Each scope has one active target and a stack of receipts. Installing the next
audio revision naturally handles a reverted file. Its hash identifies the
needed bytes even when the build that immediately preceded it never shipped
those bytes. Accepting a merged userland baseline over a preview replaces the
active target and records the preview as its before-state. Undo restores that
preview. Repeating the same verified target can be reported as already installed.

**Switching experiments.** Installing Yonder over Audio is an ordinary install.
Compare Audio's receipt and actual files with Yonder's complete target. Include
Audio-only retirements in the preview. Stage the incoming files and verify the
backup before publishing. One completed switch adds one receipt to system
history. Undo Yonder and Audio is back. An interruption before durable completion
uses the same recovery procedure as any install and restores Audio. Each switch
to a different target grows the undo stack. Independent media histories keep
their own heads. To run both code changes together, publish a build containing both.

## Reproducible installable bytes

In the initial comparison, 229 of 235 differing ELF pairs from two revisions
became identical after removing DWARF from temporary copies. The debugging
requirement is named traces in normal images and matching host symbols for GDB.
Fable's proposed alternative to separate debug builds is to compile once with
DWARF, retain the exact host ELF and feed stripped-debug copies to QEMU, the
lifeboat and Porter. Decide the compile default after comparing build cost
and verifying matching symbols. Keep optimization independent of this choice.

Replace wall-clock inputs in the kernel link stamp and `webpbench` with stable
provenance where needed. Record human build times in the catalog. A revision
stamp in a shipped kernel changes its identity on unrelated commits, so define
it from kernel inputs if unchanged kernels should deduplicate across branches.
Prove zero payload differences for the same revision built in differently
named worktrees at different times. Examine residual differences individually.
[Kernel build](../../../kernel/GNUmakefile) · [Userland flags](../../../userland/GNUmakefile)
· [Benchmark](../../../userland/tests/webpbench/webpbench.c)

## Two configuration files and one server directory

`porter-sources.conf` belongs on the build side. Give each source a lot,
resolve project paths against the worktree being packaged, and allow local
entries for shared fonts, theme packs, audio and video. Host paths stay on
the publishing machine. An external collection can be packaged by itself
or published alongside a system build. Each independent collection has its own inventory and history. The Windows server needs access
to the shared package repository. The build host needs access to the selected
sources at publication time. A published build remains installable after its
worktree or media source goes away.

For example, the proposed source syntax below appends roots to a lot.
Built-in Make inventories supply the program and library lots.

```ini
[sources]
frames    += frames
pages     += userland/bin/pages
fonts     += etc/fonts
fonts     += userland/bin/fonts
icons     += assets/icons/default
```

The workstation's local overlay can add a `themepacks` root on a mounted
Windows drive or a WSL filesystem using the same syntax. In a guest
`[destinations]` section, a rule such as `downloads/*.mp4 = /home/videos`
overrides the default for that lot. Match suffixes on the basename, use the
most specific matching suffix, and keep the path below the source root.

`porter.conf` belongs on Os64. Ship defaults in `/etc/porter.conf` and overlay
individual keys from the user's configuration, normally `/home/porter.conf`. Read the effective
routes once per operation. The following are example destinations.

```ini
[destinations]
apps       = /bin
libs       = /lib
tests      = /tests
boot       = /fat/boot
config     = /etc
fonts      = /etc/fonts
frames     = /etc/frames
pages      = /tests/pages
icons      = /etc/icons/default
certs      = /etc/certs
themepacks = /var/themes
sounds     = /home/sounds
videos     = /home/videos

[backups]
directory   = /home/archive
compression = gzip
```

Directories are recursive. For example,
`themepacks/saved-customizations/Blue.frame` lands at
`/var/themes/saved-customizations/Blue.frame`. A PNG beside a test page stays
beside it. Keep the relative subpath when applying a lot-scoped file type
override for a mixed collection. Prefer distinct build lots for distinct
destinations, including libraries and kernel boot modules. Reserve the EFI loader and boot menu for explicit maintenance outside ordinary v1 packages. Require
an effective route for each selected lot and show the resolved paths in preview.

An explicitly registered nested source owns its subtree. Two sources may feed
the same lot, as with shared fonts and staged fonts. Coalesce entries with
the same logical name, kind and bytes. Report conflicting definitions with
both source locations. Use regular files and directories, skip traversal
through links or junctions, and report exclusions. On the guest check again
for destination collisions, including FAT aliases, file/directory conflicts,
reserved backup and receipt paths, and unavailable destination mounts.
Independent scopes may not claim the same resolved destination.
An upgrade encountering a changed route must show the recorded old location
and the proposed new one. V1 can refuse a move until the previous install is
undone. Receipts carry the actual volume and path used, so later config edits
cannot redirect undo.

The present valet scans one directory level, flattens by basename and looks
up the first match. Its line protocol and client limits do not describe a
recursive package. Add catalog, manifest and archive service alongside the
existing valet operations. Use immutable IDs in requests and length-delimited
or encoded names that handle spaces. Catalog browsing must not read every
movie into memory. [Current server](../../../tools/os64serve.py)
· [Current routing rules](../../../etc/os64get.conf)
· [Current limits](../completed/OS64GET.md#wire-protocol-and-limits)

## Compressed objects, transfer and backups

Store each distinct file once. Fable's proposed representation keys the object
by SHA-256 of the original bytes and stores a gzip member containing those
bytes padded to a 512-byte tar boundary. For a transfer, concatenate gzip
members for ustar headers, stored file data and the final zero blocks. Directory
entries have headers only. Keep original lengths in the manifest and normalize
archive metadata. A Windows server should be able to produce both a selected
transfer and a complete standalone `.tar.gz` without recompressing media or
keeping a second archive per package. Verify this on Os64 and with a host tar
reader, including empty files and sizes on both sides of a block boundary.
[Tar format](../../../userland/apps/tar/tar_format.h) · [Gzip interfaces](../../../userland/libgzip/include/gzip/gzip.h)

Use canonical target manifests with content IDs. Keep human labels and
publication records separate so repeated labels or identical inputs cannot
overwrite an older target. Verify immutable objects and their references before
atomically publishing a complete catalog entry. A directory scan must never
advertise a partly written entry. Retain objects reachable from published
targets. The guest validates manifest and archive bounds, names, kinds, sizes,
member coverage, digests and gzip completion while streaming to owned scratch
on the destination mounts. The initial service assumes a trusted LAN.

Back up actual replaced and retired bytes into one local `.tar.gz` per run.
Finalize, reopen and verify the original lengths and hashes by decompressing
before publishing any destination. A run containing only additions has a
receipt and no backup bytes. Reserve space per mount including the worst case
for already-compressed media. Keep backups and receipts together on persistent
storage, normally under `/home`, while entries are undoable.

## The writer and recovery

Serialize mutations across both front ends and all scopes. The operation record
must identify a boot and a live task incarnation. Exclusive creation is a
candidate primitive. The slice brief must cover a cut during acquisition,
a reused task ID and two attempts to recover an abandoned operation. Revalidate
the selected history heads, resolved volume identities and approved plan under
the writer. Resolve receipt paths as partition GUID plus path *inside* the
filesystem, including when booted from a different root. Keep a per-scope head
stamp on each managed volume. A rebuilt image may reuse the old partition GUID.
Missing or inconsistent stamps require reconciliation of a pending operation
first, otherwise explicit baseline adoption. Preserve the disowned history
for inspection and distinguish it from an undoable chain.

**An interrupted install is undone. An interrupted undo is finished.** The
operation record describes before/after identities, backup members, old and
intended scope heads, affected volume stamps and directories the operation
owns. Verify a complete unwind, including every earlier backup and each
intermediate identity, before its first write.

**The operator starts recovery in v1.** On launch, either front end checks
the local operation record. If the previous writer is gone and an operation
is pending, show the affected target and expected recovery outcome. At the
shell run `porter recover`; the desktop's **Recover** action invokes the same
engine. History remains readable. Further installs and undos wait for recovery
to finish. If the normal system cannot start Porter, boot the known-good
lifeboat and run the command there. Boot itself performs no Porter repair.

| State | Evidence and permitted writes | Restart action |
| --- | --- | --- |
| Prepared | Writer claimed. Stage incoming files and collect backups. Live destinations and heads are untouched. | Discard owned staging after establishing abandonment. |
| Backed up | Complete backup verified. Record rollback intent, then require a successful storage barrier for backup and record before allowing destination changes. | An intact intent is sufficient for rollback regardless of whether a first write occurred. |
| Committing | Apply approved changes from owned staging. Verify them, update intended volume stamps, then require a barrier covering every participating device. Keep original bytes and intent available. | In the absence of a valid completion record, restore the before-state and stamps. |
| Complete | Publish the completion record and new history head recoverably. Require a final successful barrier before reporting success or reclaiming recovery data. | Reconcile heads and stamps with the durable decision. A torn record cannot count as completion. |
| Undoing | Persist the intended unwind and its final head before changing files. Restore each receipt in reverse order, flushing restored state before durably advancing the head. | Finish the recorded unwind. Hashes and validated heads make repeating a step safe. |

Recovery reads back the actual identities. Leave a path already in the desired
older state alone. Restore a matching after-image from its verified backup,
or remove it if the install created it. Treat an absent intermediate name
according to the recorded operation. Preserve unexpected contents, including
local edits to a file the run created. Remove owned directories only when empty.
A failed barrier or an unavailable backup volume leaves an explicit incomplete
operation. The protocol must specify how a last valid decision survives a
torn metadata write before coding the writer.

The current whole-system sync calls VFS sync. Shutdown separately calls an
NVMe flush routine returning void. Porter needs a checked barrier that covers
the filesystems and their underlying devices and reports failure. Kernel
review must establish ordering and error propagation. QEMU termination
exercises the restart logic. It does not simulate losing a drive's volatile
cache. Filesystem damage outside the recorded before/after cases may require
offline repair. [System sync](../../../kernel/src/syscall.c) · [Shutdown](../../../kernel/src/shutdown.c)
· [NVMe flush](../../../kernel/src/driver/system/nvme.c)

## Bootable recovery and compatibility

The normal and FAT rescue entries in the checked-in HD menu name the same
kernel file. The first review records a hardware rescue boot that failed to
reach a shell. Before a kernel-changing install on hardware, establish a
separate known-good kernel, boot modules, FAT runtime and text-mode Porter
that have booted together. Exercise the actual rescue entry. Protect that
pair throughout ordinary installs and undos. A later kernel install cannot
rotate an untested kernel into the rescue slot. Keep EFI and menu changes
under separate maintenance. Preview any required manual menu change.
[HD boot menu](../../../limine-hd.conf) · [Hardware observation](INSTALLER_REVIEW.md)

On FAT use transaction-owned aside names for affected live files, check for
collisions and record recovery names before the first rename. A rename onto
an absent name avoids the existing remove-first replacement path. Prove restart
at each boundary. Reserve the bootloader's known-good rescue name separately.
Promotion of rescue files is an explicit operation from a working main system
after a successful boot. Its slice must show how bootstrap and promotion keep
a reachable recovery path through a cut, including when boot modules change.
Repair the main runtime from rescue. [FAT rename](../../../kernel/src/driver/filesystem/fat/fat_glue.c)

Record kernel ABI identity for system userland as a whole, including direct
syscall callers. A digest of the public ABI headers is a starting point.
Version consumed kernel report formats and explicitly identify coupled data
formats where relevant. A string scan can suggest dependencies for review.
Compare the target against both the staged and booted kernel. For a
kernel-coupled install or undo, the preview says **This operation will reboot
the machine**. Confirmation covers the reboot. After filesystem changes,
receipt and history head are durable, the running Porter process calls
`os64_shutdown(OS64_SHUTDOWN_REBOOT)` directly. A barrier failure leaves the
transaction in recovery. A cut after durable completion leaves the install
committed even if Porter never reached the reboot call.

Other tasks can launch programs while Porter is publishing files. For v1,
when the target runtime requires an ABI incompatible with the running main
kernel, perform the mutation from the known-good lifeboat against the
inactive main system, then reboot into the main system. A live update whose
runtime supports the running kernel can use the direct reboot path. This
uses the existing reboot interface and requires no per-program loader check.
[Userland reboot interface](../../../userland/libos64/include/os64/io.h)

## CLI, desktop and delivery

One executable, `porter`, owns catalog access, preview, install, history, undo
and offline recovery. The desktop app launches it with structured input,
progress and results. Commit decisions live in the engine. After commitment,
loss of a UI reader, including a closed progress pipe, must allow the operation
to finish. Reopening either front end reads the same outcome. Keep the engine
and its runtime independent of the desktop. Reuse the mount-aware staging,
path checks and verified publication already present in the tree.
[Existing installer code](../../../userland/apps/os64get/install.c)

Each CLI invocation records its argument vector, ordinary stdout and stderr in
order, and exit status in a separate `/var/porter/run-*.log` named by UTC time
and task ID. The CLI displays
the transcript path on stderr and reports logging failures there while keeping
the command's live output and exit status. An interrupted or incomplete log
can lack the exit footer. The transcript writer is available to the desktop
frontend. Preview writes this transcript and leaves managed files and install
history untouched. Porter reserves the log tree and excludes it from package
plans. Logs survive ordinary reboots and `os64get -a`. A full
`p5-refresh.sh` image refresh resets that part of the root filesystem.

| Proposed slice | Acceptance |
| --- | --- |
| 1. Freeze normal outputs | Repeat the two-worktree build at different times. Compare ELF and data bytes, trace names and GDB symbols. |
| 2. Publisher, catalog and Windows transfer | Concurrent publishers, complete scopes, bounded tar.gz export, large media, empty directories, source removal, missing roots and corrupted streams. |
| 3. CLI preview | Run the two-library example above both ways. Distinguish recognized downgrade from local drift, and detect collisions on actual mounted volumes. Verify transcripts on successful and failed commands, with concurrent invocations and a failed log open. |
| 4. Checked barrier and ext2 transactions | Kernel review, storage failure injection, interrupted install and single undo. Restart and use `porter recover` offline. Exercise a direct preview switch and its undo on a disposable disk. |
| 5. FAT, kernel and rescue promotion | Exercise absent-name and bad-kernel boots, a live compatible update, and an incompatible main-runtime update from rescue. Verify reboot follows durable completion and a failed flush leaves a recoverable operation. |
| 6. Multi-run undo | Verify older backups before starting, interrupt between receipts, preserve unrelated media and finish the unwind offline. |
| 7. Desktop and hardware | Compare CLI and GUI plans. Close the GUI mid-commit, exercise self-update, and repeat the accepted rehearsal with the CLI available from rescue. |

**End-to-end rehearsal.** Seed the disk with debug binaries, an edited config
and a personal file. Adopt a baseline. Independently install a theme and
recording. Install two audio revisions with one reverted file. Switch to
Yonder with one confirmation, then install a merged userland target. Undo the
merged target and recover Yonder's exact state. Undo Yonder and verify audio
revision 2 byte for byte. Undo both audio receipts to reach the baseline.
Repeat with an out-of-band library edit and with a recorded newer
userland library. Check the refusal and the explicit downgrade respectively.
Unwind through first adoption and verify the original debug binaries, edited
config and personal file byte for byte. The independent theme and recording
remain installed until their own histories are undone.
