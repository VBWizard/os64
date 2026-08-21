#!/bin/bash
# p5-refresh.sh — mirror the os64 system payload onto a real machine's own
# partitions, repeatably. Runs on the target machine's Linux (the P5's
# Ubuntu), reads the payload out of the very ISO you booted from, and never
# needs editing between refreshes.
#
# THE PERSISTENCE DOCTRINE (ruled 2026-08-07): the ROOT and FAT partitions
# are the SYSTEM's — this script mirrors them exactly (rsync --delete) and
# anything you wrote there dies, same as a QEMU image rebuild. The /home
# partition is YOURS — this script does not know its device name on purpose
# and will never touch it.
#
# ── ONE-TIME SETUP (do this once; refreshes need none of it) ────────────────
#
# 1. Carve three partitions in the space set aside for os64 (gdisk/parted):
#      big   ext2  — the system root (256MB+ — as big as you like)
#      small FAT   — the lifeboat  (>= 64MB — mformat's FAT32 floor)
#      yours ext2  — /home         (sized to taste; NEVER touched here)
#
# 2. Give them the CANONICAL GUIDs and GPT names. The kernel auto-mounts
#    only partitions whose GUIDs it recognizes (vfs.c kKnownPartGUIDs), and
#    mounts each at its GPT NAME — so the same three GUIDs the build stamps
#    on every QEMU image make the P5's partitions native citizens, and every
#    existing Limine entry's ROOT= works unchanged. (Twin GUIDs at BOOT are
#    handled by the OS64 KERNEL — its mount table dedupes by GUID, built for
#    the RAMDisk's twin. Twin GUIDs while THIS SCRIPT runs under Linux are a
#    different animal entirely: udev lets the newest twin steal the
#    by-partuuid symlink, which is why find_real_part below never trusts it
#    — the lesson of the first bare-metal run, 2026-08-08.)
#      sgdisk /dev/nvme0n1 \
#        --partition-guid=<rootN>:1ec5f5ab-71b7-45cd-a7a4-05646e878e57 --change-name=<rootN>:ext2 \
#        --partition-guid=<fatN>:2f4fd02e-68b4-4c82-98bc-72467529b3fc  --change-name=<fatN>:fat \
#        --partition-guid=<homeN>:7a3c1d90-4e62-4f3b-9a55-0c6f2b8e41d7 --change-name=<homeN>:home
#
# 3. Make the filesystems, in the exact shape the drivers grew up on:
#      mkfs.ext2 -b 1024 -O ^dir_index -L OS64ROOT /dev/<root-partition>
#      mkfs.fat  -F 32                 -n OS64FAT  /dev/<fat-partition>
#      mkfs.ext2 -b 1024 -O ^dir_index -L OS64HOME /dev/<home-partition>
#
# 4. Run this script (it lives at the ISO root):
#      sudo bash /media/<you>/<iso>/p5-refresh.sh /media/<you>/<iso>
#    ...and thereafter, after every new burn: same command. That's the
#    whole refresh.
#
# ── HD BOOT, one-time (2026-08-21) — retire the thumb drive ─────────────────
#
# The mirror above already delivers a COMPLETE boot volume: since 2026-08-21
# the FAT lifeboat carries /boot/os64_kernel, the font/pci modules, the
# HD-boot menu at /boot/limine/limine.conf (limine-hd.conf in the repo), and
# Limine's own EFI app at /EFI/BOOT/BOOTX64.EFI. Limine IS the bootloader —
# no GRUB anywhere, and the Windows ESP is NEVER touched: UEFI boots an EFI
# app from any FAT partition an NVRAM entry names ("ESP" is a convention,
# not a law), and Limine 8.7 reads its conf from its own boot volume — which
# is now the lifeboat. (Rehearsed end-to-end in QEMU/OVMF, including the
# instructive failure: parking Limine on a conf-less ESP and hoping for a
# cross-volume conf search earns "[config file not found]" — that search
# does not exist. Everything on one volume is the design, not a shortcut.)
#
# 5. Retype the lifeboat as an EFI System Partition. This is the step that
#    matters, and the 2026-08-21 install on the P5 proved every part of it
#    the hard way:
#      sudo sgdisk -t <lifeboat-#>:EF00 /dev/<internal-disk>
#    Only the TYPE GUID changes — the PARTUUID the kernel mounts by, the
#    GPT name, and the filesystem are untouched (so step 2's canonical GUID
#    and this script's find_real_part still work, and Windows ignores the
#    extra ESP entirely). The old typecode 0700 was fine for the OS and
#    fatal for booting: the P5's AMI firmware VALIDATES NVRAM boot entries
#    against partition type and silently DELETES, at every boot, any entry
#    pointing at a non-ESP partition. Three perfectly-formed efibootmgr
#    entries died that way before the pattern was clear.
#
# 6. Do NOT bother with efibootmgr at all — the same firmware that launders
#    foreign entries AUTO-DISCOVERS \EFI\BOOT\BOOTX64.EFI on any ESP-typed
#    partition and mints its own entry for it (label "UEFI OS"), which,
#    being its own child, it never deletes. Make it the default in the
#    firmware's OWN setup UI (F2/Del): Boot tab -> the nested
#    "UEFI Hard Disk / NVME BBS Priorities" submenu (the top-level Boot
#    Option list shows device CLASSES; the submenu picks each class's
#    representative) -> put "UEFI OS" at #1, then the NVMe class itself at
#    Boot Option #1. Settings made in the firmware's own UI are the one
#    thing it respects. (The one-shot boot-menu key — F7-ish — lists
#    "UEFI OS" too, for trying before committing.)
#    CAUTION: if Windows uses BitLocker/device encryption (check with
#    `manage-bde -status` in Windows), chainloading changes the measured
#    boot path and Windows may ask for its recovery key once — have it
#    handy before the first try, or boot Windows only via the firmware menu.
#    (limine-hd.conf's /Windows entry chainloads bootmgfw.efi by the ESP's
#    explicit PARTUUID — chainload_next walks DEVICES, not partitions, and
#    could not see a Windows ESP on the same disk; learned live.)
#
# 7. Thereafter a kernel update needs no Linux and no stick at all: from a
#    RUNNING os64, fetch the new kernel over the NIC onto /fat/boot/
#    (os64get), reboot. The OS updates itself — kernel, menu, even the
#    bootloader, all through /fat. This script remains the full-payload
#    refresh (new apps, /bin, fixtures) and the recovery path.
#
# The Limine entry: "/Bosgame GUI" (desktop) and "/Bosgame Boot - no GUI"
# (text) in limine.conf both boot the P5 from its own disk, skipping the
# RAMDisk module load entirely. They use the canonical ROOT GUID above, so
# they need no editing. (Both were once one commented-out "/Bosgame Boot
# (disk root)" template you had to uncomment; the surgery below is what made
# them real, and the 2026-08-20 limine.conf walk gave them their names.)

set -euo pipefail

# The canonical partition GUIDs (GNUmakefile constants, unchanged since the
# disk image was born — and now also the P5's, per the surgery above).
ROOT_UUID=1ec5f5ab-71b7-45cd-a7a4-05646e878e57
FAT_UUID=2f4fd02e-68b4-4c82-98bc-72467529b3fc

usage() { echo "usage: sudo $0 /path/to/mounted/iso (or the .iso file itself)"; exit 2; }

[ "$(id -u)" = 0 ] || { echo "p5-refresh: needs root (sudo) for mounts and rsync"; usage; }
SRC="${1:-}"; [ -n "$SRC" ] || usage

# Resolve each GUID to the one REAL partition that carries it — never via
# /dev/disk/by-partuuid. That symlink CANNOT be trusted in this script: the
# payload image carries the SAME canonical GUIDs as the destination (the
# whole design), and the moment losetup maps it, udev repoints the symlink
# at the newest twin — the READ-ONLY loop. The first bare-metal run
# (2026-08-08) did exactly that: mounted the source image AS the
# destination ("write protected" warnings), rsync'd a filesystem onto
# itself (zero diffs, zero errors, zero refresh), and printed "done."
# Twin-GUID dedupe is an os64-KERNEL virtue; Linux udev lets the newest
# twin steal the name. So: enumerate real partitions, exclude loops (a
# stale loop from a crashed run must not win either), refuse ambiguity.
find_real_part() {
	local uuid="$1" what="$2" hits
	hits=$(blkid -o device -t PARTUUID="$uuid" 2>/dev/null | grep -v '^/dev/loop' || true)
	if [ -z "$hits" ]; then
		echo "p5-refresh: no real partition carries PARTUUID $uuid ($what) — do the one-time setup in this script's header" >&2
		exit 1
	fi
	if [ "$(echo "$hits" | wc -l)" != 1 ]; then
		echo "p5-refresh: MULTIPLE real partitions carry PARTUUID $uuid ($what):" >&2
		echo "$hits" >&2
		echo "p5-refresh: detach the twin (a dd'd stick or second disk?) and rerun" >&2
		exit 1
	fi
	echo "$hits"
}
ROOT_DEV=$(find_real_part "$ROOT_UUID" "system root")
FAT_DEV=$(find_real_part "$FAT_UUID" "lifeboat")
echo "p5-refresh: destinations: root=$ROOT_DEV lifeboat=$FAT_DEV"

# ...and refuse if the filesystems aren't what we expect to overwrite.
[ "$(blkid -o value -s TYPE "$ROOT_DEV")" = "ext2" ] || { echo "p5-refresh: $ROOT_DEV is not ext2 — refusing"; exit 1; }
case "$(blkid -o value -s TYPE "$FAT_DEV")" in vfat) ;; *) echo "p5-refresh: $FAT_DEV is not vfat — refusing"; exit 1;; esac

WORK=$(mktemp -d)
LOOP=""
ISO_MNT=""
cleanup() {
	set +e
	for d in "$WORK"/root-dst "$WORK"/fat-dst "$WORK"/ext2-src "$WORK"/fat-src; do
		mountpoint -q "$d" && umount "$d"
	done
	[ -n "$LOOP" ] && losetup -d "$LOOP"
	[ -n "$ISO_MNT" ] && umount "$WORK/iso"
	rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$WORK"/{iso,ext2-src,fat-src,root-dst,fat-dst}

# Accept either the mounted ISO directory or the .iso file itself.
if [ -d "$SRC" ]; then
	IMG="$SRC/boot/os64_disk.img"
else
	mount -o loop,ro "$SRC" "$WORK/iso"
	ISO_MNT=yes
	IMG="$WORK/iso/boot/os64_disk.img"
fi
[ -r "$IMG" ] || { echo "p5-refresh: cannot find boot/os64_disk.img under $SRC"; exit 1; }

# The payload image is GPT: p1 = FAT (lifeboat), p2 = ext2 (system root).
LOOP=$(losetup --show -rfP "$IMG")
mount -o ro "${LOOP}p2" "$WORK/ext2-src"
mount -o ro "${LOOP}p1" "$WORK/fat-src"
mount "$ROOT_DEV" "$WORK/root-dst"
mount "$FAT_DEV"  "$WORK/fat-dst"

# A destination that fell back to read-only is a refresh that will not
# happen — and mount only WARNS on that fallback and exits 0, which is the
# silent half of the 2026-08-08 failure. Fail loudly instead.
for d in "$WORK/root-dst" "$WORK/fat-dst"; do
	case ",$(findmnt -n -o OPTIONS "$d")," in
		*,ro,*) echo "p5-refresh: $d ($(findmnt -n -o SOURCE "$d")) mounted READ-ONLY — refusing to pretend"; exit 1;;
	esac
done

echo "p5-refresh: mirroring system root (ext2)..."
rsync -a --delete "$WORK/ext2-src/" "$WORK/root-dst/"
echo "p5-refresh: mirroring lifeboat (FAT)..."
rsync -a --delete "$WORK/fat-src/" "$WORK/fat-dst/"
sync

# The receipt names the actual devices written — if either says "loop",
# something above has failed in a new and interesting way.
echo "p5-refresh: done. $(findmnt -n -o SOURCE "$WORK/root-dst") and $(findmnt -n -o SOURCE "$WORK/fat-dst") mirror the build; /home untouched, as ruled."
