# INSTALLER.md review (Fable 5.2, 2026-11-24)

Read against `userland` at d001ae151: `userland/apps/os64get/install.c`,
OS64GET.md, `fat_glue.c` (`fat_rename`), `ext2_write.c` (rename, sync),
`nvme.c` and `shutdown.c` (Flush), `limine.conf`, `symbols.c`,
`elf_loader.c`, `libgzip/gzip.h`, both makefiles' flags and the kernel build
stamp, and the last 150 merges into `userland`.

## Verdict

The bones are right: a manifest of content identities, receipts that record
what was actually there, undo in stack order, one engine under two front
ends, and a recovery tool that needs neither the desktop nor the network.
Build it.

Two findings are hazards the design does not yet cover (1 and 2), and both
have small fixes. Three are simplifications (3, 4, 5) that I think remove
about half of what slices 3 and 4 would otherwise have to build and prove.
Finding 4 is the large one and it is a lean, not a defect: it trades a
capability the design promises for a much smaller machine, so it is Chris's
and Quinn's call.

The lineage, since it rides along: a store of immutable objects named by
their own hash, with small manifests that point into it, is Venti (Quinlan
and Dorward, Plan 9, 2002), which git borrowed three years later. And "the
machine holds one built state at a time, and history is a stack of them" is
what Nix calls generations. The design reaches both on its own.

## What I verified

- **The lifeboat boots the same kernel file as everything else.** Every
  entry in the repo's `limine.conf` (the ISO's menu), the FAT lifeboat's
  included, has `kernel_path: boot():/boot/os64_kernel`, and all three os64
  entries of `limine-hd.conf` (the hard-disk menu, the P5's kind) name
  `/boot/os64_kernel` on the FAT partition by GUID. (The P5's own copy is
  kept by hand; check that it reads the same.)
- **FAT replacement removes the destination first** (`fat_rename`,
  fat_glue.c:777-791), as the design says. A rename onto an ABSENT name has
  no such window.
- **Nothing flushes the drive until shutdown.** `nvme_flush_all` has one
  caller, shutdown.c:171. `ext2_sync` is a no-op because ext2 is
  write-through to the device, and the whole-system sync (syscall.c:1314,
  `vfs_sync_all`) does not issue Flush either.
- **Stripping DWARF keeps the names traces need.** Ring 0 reads `.symtab`
  from the kernel file Limine kept (symbols.c); ring 3 reads the image's own
  tables (elf_loader.c). `objcopy --strip-debug` keeps both, and the root
  makefile already ships the lifeboat's `/lib` that way (GNUmakefile:674).
- **Receipts can name a volume.** `/sys/mounts` carries each mount's
  partition GUID, and `install.c` already resolves paths through it.
- **libgzip reads concatenated members** (gzip.h: the limit "applies across
  every member in a concatenated gzip stream"), and GNU tar reads an archive
  assembled from per-file gzip members: I built one on the host, a header
  member then a stored data member per file, one object reused under two
  paths, and `tar xzf` returned the right bytes. Finding 6 uses this.
- **How often a branch changes a shared library.** Of the last 150 branches
  merged into `userland`, 121 change at least one shared library (libdom 51,
  libjs 40, libflow 23, libgarb 18, libmedia 15, libav 6; libos64 only 2),
  and 16 touch `kernel/`. Finding 4 rests on this.
- Syscalls are inline in `abi/include/os64/syscall.h` and several programs
  call them directly, so a kernel requirement belongs to every ELF, not to
  libos64.so alone.

## Findings

### 1. The kernel's name on FAT is the one file whose loss stops both boots

The design asks what the lifeboat can recover when FAT loses a name. If the
name is `/fat/boot/os64_kernel`, the answer is nothing: the lifeboat entry
loads that same file, so a power cut inside FAT's remove-first window leaves
no entry that boots, and the recovery tool never runs. A likelier failure
has the same shape: a new kernel that panics at boot. The receipts and the
backup are intact and nothing can reach them.

Fix, in two parts.

**Never replace on FAT; go around.** For a file on FAT that already exists:
remove last time's `NAME.prev`, rename the live file to `NAME.prev` (a
rename onto an absent name: no window), then rename the staged file to
`NAME`. At every instant one of the two names holds a complete, verified
file. This is the general FAT recipe, and recovery knows to look for
`.prev`.

**One hand-added Limine entry**, "previous kernel, lifeboat root", with
`kernel_path: boot():/boot/os64_kernel.prev` and the lifeboat's `ROOT=`. The
menu does not travel to the P5, so Chris adds it once. It is the rescue for
a lost name and for a bad kernel alike.

That entry is only as good as the pair it boots, which gives the rule that
answers "the recovery tool must work with the booted ABI":

> **The previous kernel and the lifeboat's userland are always a pair that
> has booted together.** One operation never changes both root's runtime and
> the lifeboat's copies. The lifeboat's `/bin` and `/lib`, and `.prev`, are
> brought forward by a separate, explicit step after the new system has
> booted and run.

Two things follow. Booted from the lifeboat, FAT cannot replace its own open
files, so the lifeboat is only ever updated from the main system and the
main system is only ever repaired from the lifeboat: each maintains the
other. And Limine's own EFI file has no aside name that firmware would
find, so I would keep the EFI lot and the menu out of ordinary packages in
v1 entirely.

**Tried the same day, and the P5's lifeboat does not reach a shell.** Chris
booted it: `/bin/vtfont` is absent, six post-boot tests that load programs
fail (elf_loader, dynamic_linking, task_args, env_growth,
backstop_preemption, task_teardown_leak), and husk is announced and never
starts on any terminal. The build is not at fault: the same entry on today's
image in QEMU passes 38 + 34 + 8 tests and seats husk on both terminals. The
cause is delivery. `etc/os64get.conf` routes the kernel, the boot modules
and Limine to `/fat/boot`, and routes nothing to the lifeboat's own `/bin`,
`/lib` or `/tests`; those were last written by the thumb drive. So the P5's
lifeboat is today's kernel over a userland frozen on the last sneakernet
day, which is exactly the untested pair the rule above forbids.

For the design this is a requirement, not a note: the lifeboat's runtime is
a set of lots with destinations of their own (the stripped libraries the
makefile already stages, the programs, the fixtures), and bringing it
forward is an operation the installer offers after a successful boot.

### 2. Durable ordering ends at the drive's cache

The commit boundary depends on one ordering: the backup and the operation
record are safe before the first destination changes. The design's proof is
"finalize, sync, reopen, decompress and verify". On ext2 the sync is a
no-op, and the read-back is answered from the same drive cache it is meant
to check, so it proves the bytes were written correctly and says nothing
about a power cut. An NVMe drive with a volatile cache may lose the backup
and keep the later renames.

This is question 3's "durable progress ordering", and the answer is that no
existing guest primitive gives it. It needs one small kernel change: a flush
barrier ring 3 can ask for. My lean is that the whole-system sync ends with
`nvme_flush_all`, as shutdown already does, rather than a new verb. The
installer needs three per operation: after the backup and record, after the
last destination change, and after the completion record.

Two honest limits to write down. QEMU cannot demonstrate this: killing QEMU
does not lose the host's page cache, so the kill tests in slice 4 prove the
ordering LOGIC and the flush is correct by construction, not by experiment.
And ext2 has no journal and os64 has no in-OS fsck, so after a real power
cut recovery must trust file CONTENTS (hashes) and never directory state it
has not re-read. The design already leans that way; finding 3 takes it the
rest of the way.

This is the piece that is kernel work and earns the kernel review tier.

### 3. Recovery in one direction: always toward the older state

The design journals progress per destination, then has to handle a cut
between a rename and its progress record, recovery of an install, recovery
of an undo, and a cut between two receipts of a multi-run undo. All of it
collapses under one rule:

> **An interrupted install is undone. An interrupted undo is finished.**

Both are the same procedure: for each path in the operation record, hash
what is there. Equal to the before-identity: nothing to do. Equal to the
after-identity, or absent (FAT's window, or a `.prev`), or a path this run
created: restore from the backup, or remove. Anything else: stop and ask.
It is idempotent, so a cut during recovery is answered by running it again.

Everything that procedure needs is the backup archive and the record, both
made durable before the first change (finding 2) and both on one volume.
It does not depend on scratch surviving on three filesystems, on the server
being reachable, or on any progress record, so the per-destination progress
records can go, and with them the class of bug where the record and the disk
disagree. A multi-run undo is the same procedure once per receipt, newest
first, with the history head moved after each.

The cost: an install cut at 95% is rolled back and you install again. For a
machine on a desk beside its build host I think that is cheap.

### 4. Build v1 for one preview at a time, and compute the delta on the guest

The compatibility table's hardest rows (requirements retained by installed
packages, the same conflict in either order, shared ownership of identical
bytes, superseding a predecessor) all serve one capability: two independent
previews installed side by side. With exact identities, two previews coexist
only when neither changes a library the other's changed programs load. The
tree's history says how often that is: 121 of the last 150 branches change a
shared library, and yonder loads nearly all of them. Side-by-side previews
would be refused far more often than allowed, and the union of two previews
is a system nobody ever built or tested.

So my lean for v1:

- **A package is a full target inventory**, every file and digest in its
  lots, not a delta against a pinned base. A baseline and a preview are the
  same kind of object with different labels.
- **The guest computes the delta**, against what is actually on the machine:
  plan = difference between current state and the target inventory, fetch
  only the objects whose digests it lacks.
- **Within a set of lots the machine holds one built state at a time.**
  History is a stack of them. Switching previews is "undo to the baseline,
  install the other", which the installer can offer as one confirmed
  operation. Media lots (themes, video) are separate lots and so separate
  chains.

What this removes from the design: pinning the merge base and building or
caching its inventory; the special case for packaging on `userland` itself;
predecessor declarations; the dependency check between programs and
libraries (the installed set is one build, so it is consistent by
construction); retained requirements; and "wrote nothing" receipts. A file
reverted to its base costs nothing special, because objects are fetched by
digest. What remains is what must remain: local-edit detection (the bytes at
a path are not what the newest receipt says the installer left there),
retirement (a path the previous state had and the target lacks), and the
kernel requirement (finding 8).

What it gives up: two unmerged previews on the machine at once. Full
inventories keep that door open, since everything needed to add a merge
later is in the manifests.

It also answers question 1. Superseding one exact predecessor cannot express
a merged baseline over two active previews, because that baseline has two
predecessors. Under this model nothing needs to: the walk is baseline, then
preview A, then merged baseline B1. B1's receipt records the bytes it
replaced (A's and the old baseline's) and owns its lots outright. Undo B1
and the state is A's again; undo A and it is the baseline. At every step the
active requirements and ownership come from exactly one receipt, the newest.
If you want the preview to say "B1 includes Audio preview 2" rather than
"replaces", the publisher can record which catalog entries' revisions are
ancestors of the one being packaged. That is information for the person, not
a rule for the engine.

### 5. Strip at freeze; do not split the build

The design proposes two build profiles, normal and debug, with separate
object, binary and image outputs. Its own measurement argues for something
cheaper: stripping DWARF from the existing `-g` binaries made 229 of 235
pairs identical. So build once, as today, and let the frozen bytes be
`objcopy --strip-debug` copies in one staging tree that feeds the disk
image, the lifeboat and the package. The debugger's symbols are the
unstripped files of the same build, so they match by construction, where two
profiles match by convention and cost a second compile of quickjs and
FFmpeg. QEMU then runs the same bytes the P5 will.

Three details. Name the six pairs that still differed and why, since they
are either real changes or the remaining leaks. The kernel's build stamp can
stay useful and become stable: short revision, commit date, and for a dirty
tree a short digest of the diff, which answers "which kernel is this" better
than a clock did. And if a checkout path turns up in a string, the cure is
`-ffile-prefix-map`.

### 6. One store, and archives are a way of sending it (question 2)

Do not keep both objects and ready-made archives. Store each distinct file
once, as its own gzip member of the data padded to a 512-byte boundary. A
`.tar.gz` of any set of files is then a concatenation: a small gzip member
holding the tar header, then the stored member, per file, then a member of
zero blocks. No recompression, no duplicate storage, bounded server memory,
and byte-for-byte reproducible with the gzip time field at zero. I checked
that GNU tar reads it, and libgzip's decoder already accepts concatenated
members.

The same request serves every case in the design: a whole baseline for the
workstation, exactly the digests a guest's plan lacks, or one large media
file. The measurements question 2 asks for become unnecessary, because
there is nothing to compare.

Two smaller choices that fall out. Make the package ID the SHA-256 of the
canonical manifest, so identity is content, publishing the same build twice
is idempotent, and a person who confirms an ID has confirmed everything
under it; the label and time live in the catalog entry. And make the catalog
a directory with one exclusively-created file per ID, so concurrent
publishers have no shared file to clobber. The same per-file-member form is
right for backups too: damage to one member costs one file, not everything
after it.

### 7. Where history lives, and how a file is named in it

Receipts and backups live on `/home`, which is never rebuilt. The files they
describe live on root, which `make` rewrites in QEMU on every build and
which is RAM under `RAMDISK`, and on FAT. The partition GUIDs are pinned in
`limine.conf`, so they are the same before and after a rebuild, and volume
identity by GUID cannot see that the volume under the history is a new one.

Add a small stamp file on each managed volume, written at commit, holding
the history head. At the start of any operation a missing or different stamp
means "history does not describe this volume", and the only offer for it is
a baseline. That is one read, where discovering it file by file is a
screenful of "changed by another tool".

And say outright that a receipt names a file as partition GUID plus the path
INSIDE that filesystem, resolved through `/sys/mounts` when used. The same
root is `/` on a normal boot and somewhere else from the lifeboat, and FAT
is `/fat` normally and `/` on the lifeboat. A receipt holding `/bin/yonder`
cannot be undone from the lifeboat; one holding (GUID, `bin/yonder`) can.

Decide too what `os64get -a` is once a machine has a baseline. My lean: it
sees the stamp and refuses without `-f`.

### 8. Kernel and data requirements (question 4)

With one-build packages, a package that selects the kernel lot is coupled
correctly by construction. The requirement only bites when the kernel lot is
left out, and it can be mechanical: an ABI identity that is the hash of the
`abi/include` tree, recorded in every manifest for the userland and for the
kernel file. The guest compares the target's identity with the staged
kernel's to allow the install, and with the booted kernel's to decide
"reboot before these programs may run". The booted kernel has to say what
its identity is, which is one line in `/sys`, and it can ride with the flush
change. The publisher's check for an incomplete declaration is then "the
`abi/` tree differs from the previous package's and the kernel lot is not
selected": refuse to publish.

What that cannot see is the ABI that is not in `abi/`: the text of `/sys`
and `/proc`. `install.c` today refuses a `/sys/mounts` row that does not
have exactly twelve columns, so a kernel that adds a thirteenth breaks an
older installer, and the older installer is precisely the lifeboat's. For
this and for data files (fonts, certificates, frames, config formats), a
solver is too much and silence is too little: have the publisher list the
`/sys/`, `/proc/` and `/etc/` paths found in each changed ELF's strings, so
a missing declaration is at least visible at publish time.

### 9. The engine is a program, and the desktop is its second caller

"One engine behind a common guest interface" reads as a library linked into
two programs. Make it one executable with a machine-readable mode, and have
the desktop installer run it and read a pipe. Then "both submit the same
plan" is literally true, the comparison in slice 5 cannot fail, a crash in
the window cannot take a commit with it (once committing, the engine treats
a closed output pipe as "carry on"), and the engine's own libraries are
libos64, libgzip and a hash, none of them the desktop's. That last part is
what makes "update the installer and its own libraries" a small test.

For the hash: SHA-256 exists three times in the tree (BearSSL, libdom's Web
Crypto, FFmpeg) and none is right to link from here. One copy in libos64
beside CRC-32, with this as the consumer that asked.

For the single writer (the rest of question 3): exclusive create already
exists (`os64_open(path, "x")`, which os64get uses). The operation record
can be the lock, holding the task ID and which boot it was written in.
Abandoned means "a different boot" or "that task is gone from `/proc`". I
found nothing that identifies a boot today, so that is a second line for
`/sys`.

## Smaller notes

- `installer.conf`'s key-by-key overlay is the right call and is
  `bootenv.conf`'s merge kind. It cures a trap OS64GET.md has to warn
  about for each new lot, where a `/home` copy silently lacks a route added
  since.
- A first baseline cannot retire what it never knew about. Have its preview
  list the files in `/bin`, `/lib` and `/tests` that are not in the build,
  so stale programs are at least seen.
- A package that needs a boot-menu change cannot deliver it. Let a manifest
  carry a note that preview shows ("add `NEWTOKEN` to the P5's entry").
- Slice 4 is three slices: install, undo and recovery on ext2 with the
  flush barrier; then FAT, the kernel's aside name and the lifeboat step;
  then multi-run undo. The first of those is the first point where the P5
  can use it.

## What I would ask of the next draft

1. Findings 1 and 2 adopted, or a reason they do not apply.
2. A ruling on finding 4. Everything in the compatibility table and in
   "Where the delta comes from" changes with it, so it should be settled
   before any other section is revised.
3. The commit and recovery section rewritten as a state table for one
   operation (prepared, backed up, committing, complete, undoing), with what
   is durable in each state and what recovery does on finding it.
