# Porter P.4 ext2 transaction contract

Applies to the checked barrier, ext2 writer and single undo in
[the installer design](INSTALLER.md). A plan may read other volumes to verify
same-content dependencies, such as the staged FAT kernel. Changes, history,
backups and staging in this slice require writable ext2 on persistent NVMe.
FAT and kernel changes remain in P.5.

## Ownership and approval

The kernel grants one live Porter writer lease across scopes. A held lease
identifies a boot nonce and a monotonically issued acquisition token, along
with the task ID for diagnostics. The kernel ties the lease to the actual writer thread. That thread
performs all Porter mutation and releases the lease on return or at its final
exit boundary, after its syscall work has returned. A frontend must acquire,
perform mutation I/O and release on the same thread. Older kernels retain
read-only CLI support and refuse writer commands when the lease is unavailable. A task with a reused ID cannot inherit a lease. Install, undo and
recover all acquire the same lease. History can be read while a writer is
working. A reboot discards kernel leases.

The CLI renders the preview before explicit approval. Under the lease it
reopens the current configuration, resolves mounted partition identities and
rebuilds the plan. Approval covers the target, active head, resolved paths,
file identities, decisions and affected volumes. A changed plan requires
another preview. Existing operations block new changes. Recovery acquires
the lease before it examines or replays an abandoned operation. Competing
recoverers either see the live owner or acquire the lease after the first
recoverer and see its resulting durable state.

## Durable records

Each operation has a unique ID. Before preparing scratch, save the full
immutable intent and then publish the stable `/home/.porter/operation`
pointer binding its digest. The intent names the lease owner, kind, scope,
target, backup and scratch locations. It records path identities as volume
GUID plus filesystem-relative path, before and after kinds, lengths and
SHA-256, owned directories, backup member identities, and complete old and
intended heads and stamps. A missing or corrupt intent named by the stable
pointer blocks recovery. State snapshots identify absent files explicitly.

Install stages selected incoming bytes and verifies the full local backup.
A separate digest-bound armed record then permits live changes. Confirmed absence of
the armed record and the completion record permits cleanup of the operation's
private files. The copier can encounter ordinary local edits during staging.
Prepared cleanup leaves destinations, heads and stamps as it finds them. A
damaged or unreadable armed record blocks recovery. The
completion record binds the exact operation and intent digest. Torn or
unreadable records are handled separately from confirmed absence.

File sync and close precede publication by rename. A checked barrier
separates each phase. Immutable records use no-replace rename. Same-volume temporary names live
under the reserved state directory outside the active heads namespace. Active
heads and stamps use atomic ext2 replacement. Recovery accepts recorded
old or new values and refuses any unexpected value before modifying
metadata. As long as the pointer can survive, keep the intent, armed and
completion records, backup and receipt data needed to replay it. Remove the
resolved operation pointer and check its barrier before reclaiming
temporary files. A cut across that unlink leaves either replayable evidence
or a durable resolved state with no operation to run. Private stage files
may survive a cut after pointer retirement and can be left for inspection.

Keep the P.3 active inventory receipt and separate undo description keyed
by its receipt ID. Explicit adoption starts an active chain with no previous
receipt. Undo relinquishes ownership without reviving a disowned chain.
Disowned receipts remain available for history inspection.

## Write order

| Phase | Durable prerequisite and action | After interruption |
| --- | --- | --- |
| Prepared | Publish the stable operation record before staging. Stream selected package members to owned directories on the target volumes. Verify archive coverage, sizes, hashes, end markers and gzip completion. | Discard this operation's scratch without changing live files or active state. |
| Intent | Build one local backup of actual replaced and retired bytes. Finish it, reopen it and verify each member and the archive. Publish the armed record and require a checked barrier for the intent, archive and staged bytes. | An armed install supports rollback. An armed undo supports finishing the undo. |
| Apply | Recheck selected paths, including adopted and unchanged dependencies, before arming. Recheck actual files before writing. Create needed directories in parent order, rename staged files within the same ext2 volume, retire files and then empty owned directories. Verify resulting identities, publish intended volume stamps and require a checked barrier. | Inspect each path again and converge on the before-state. Restore or remove recorded after-images. Stop on unexpected contents and preserve unrelated entries. |
| Decide | Publish the separate completion bound to the intent and require a checked barrier. Publish the resulting head, reconcile stamps if needed and require another checked barrier. | A valid completion selects the new state. Reconcile the head and stamps with that decision. |
| Release | Once the resolved state is durable, remove the stable operation pointer and require a final checked barrier before reporting success. Preserve immutable receipts and backup data needed for undo. | A surviving pointer permits replay. An absent pointer with durable head and stamps is already settled. |

A failed barrier stops phase advancement. Retain the operation, backups and
other recovery evidence. A failure after writing completion leaves recovery
to check which decision survived. Single undo checks the active receipt and
all its backup data and current file identities before taking authority to
mutate. Undo and install rollback both converge on recorded before-images.
An interrupted undo finishes the same unwind and advances to the recorded
previous active head. First-adoption undo relinquishes ownership. Recovery
uses persistent local data only and issues no network requests.

The checked barrier runs filesystem sync before checked NVMe Flush, including
for writes made by already-closed handles. Preserve the existing shutdown
behavior while returning I/O failure to the Porter caller. A normal QEMU stop
can verify ordering and restart behavior. Device-cache durability requires
accurate flush failure propagation and cannot be inferred from an emulator
process stop alone.

The checked NVMe path consumes completed commands even when the controller
reports failure. Block-cache and ext2 errors from the failed write stop
Porter before the next barrier. A timed-out Flush keeps its completion slot
until drained. A timed-out ordinary DMA I/O command retains the driver's
fatal timeout policy.

## Running the disposable proof

Build the stock ISO and data disk, then run
`python3 tools/test_porter_transactions.py` for the direct-switch rehearsal.
The `write` and `flush` scenarios use [QEMU blkdebug](https://www.qemu.org/docs/master/devel/testing/blkdebug.html)
with the production binary. Cuts use a guest-only test binary. Build it separately
from the stock ISO used to boot.

```sh
make -C userland \
    PORTER_CFLAGS="-I$PWD/userland/libtls/port/include -I$PWD/userland/libtls/upstream/inc -DPORTER_TEST_INTERRUPTS" \
    "$PWD/userland/bin/porter"
cp userland/bin/porter /tmp/porter-instrumented
python3 tools/test_porter_transactions.py --scenario mixed-install --instrumented /tmp/porter-instrumented
python3 tools/test_porter_transactions.py --scenario mixed-undo --instrumented /tmp/porter-instrumented
```

The `lifeboat` case starts the stock UEFI FAT entry after the cut. It
resolves the persisted ext2 partition GUID at its different mount point and
replays with networking disabled. Keep the default build free of the test
flag when staging Porter for normal use.
