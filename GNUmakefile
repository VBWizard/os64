# Nuke built-in rules and variables.
MAKEFLAGS += -rR
.SUFFIXES:
QEMUDEBUGFLAGS = -S -s
# Convenience macro to reliably declare user overridable variables.
override USER_VARIABLE = $(if $(filter $(origin $(1)),default undefined),$(eval override $(1) := $(2)))

# Default user QEMU flags. These are appended to the QEMU command calls.
# Send COM1 port output to the console with: -serial mon:stdio 
# Send debug logging to file with: -D qemu_debug.log 
# debug hardware ints and such: -d $(shell echo int,cpu_reset,pcall,guest_errors) \
# $(call USER_VARIABLE,QEMUFLAGS,-m 8g -smp 2 -no-reboot -serial file:qemu_com1.log -monitor "$(shell echo telnet:127.0.0.1:55555,server,nowait)" -d "$(shell echo int,cpu_reset,pcall,guest_errors)")

# Define the base QEMU flags
# -smp 2
# -machine q35 lives HERE, not per-target, since 2026-08-18: it is the single
# most load-bearing flag in the set (os64's PCI config path is ECAM-only, and
# QEMU's default i440FX board has no MCFG — a boot without q35 triple-faults
# seconds in, silently if LOGD= is holding serial). It was spelled in every
# target individually, so anyone rebuilding an invocation from these
# variables — as the headless harness does — inherited everything EXCEPT the
# flag that mattered. See VERIFICATION.md's headless section for the scar.
QEMU_BASE_FLAGS = -machine q35 -m 8g -no-reboot -smp 8 \
                  -serial file:qemu_com1.log \
                  -monitor $(shell echo telnet:127.0.0.1:55555,server,nowait) \
				 # -d $(shell echo int,cpu_reset,pcall,guest_errors)
#				  -D qemu_debug.log 

# Disk image configuration.
#
# ** DISK_SIZE_MB is the ONE knob for the ramdisk size. ** It sizes both the
# image file AND the partition inside it (the `disk` target passes end-sector
# 0 to sgdisk = "fill the image"). Change this number, run `make`, done.
#
# The full size is what Limine loads into RAM on a ramdisk boot AND what the
# ISO carries — so it directly costs boot time (~1s/32MB) and build-copy time.
# The image is sparse on the build HOST (rm+truncate in the disk target), so
# the on-disk build file stays tiny regardless; only the ISO/RAM cost scales.
#
# ** HARD FLOOR: ~40MB, use 64MB. ** The disk target runs `mformat -F`, which
# FORCES FAT32 — and FAT32 is only legal at >= 65525 clusters (~34MB of data
# area at 1 sector/cluster). Below that, mformat emits a malformed hybrid
# (FAT32 layout but a FAT16-range cluster count and the 16-bit size field set)
# that the os64 FAT driver misclassifies as FAT16 and can't read — every file
# open fails ("Failed to open /partition_info" panic, seen 2026-07-12 at
# 32MB). 64MB gives a clean FAT32 (small-size=0, ~128k clusters). Do NOT drop
# below 64 without also dropping `-F` / teaching the driver FAT16 (see DEBTS).
#
# Sized to the FILES on it (~57KB of apps+fixtures) plus headroom. History:
# 64MB originally, briefly 512MB to reserve space for a log-to-disk sink that
# doesn't exist yet ("an hour of heavy logs" is 100s of MB regardless — a
# log-sink-design problem, not a boot-image one), so 512MB was 99.99% empty
# dead weight and cost ~30s of boot load. Back to the proven 64MB floor.
#
# The full size is what Limine loads into RAM on a ramdisk boot AND what the
# ISO carries, so it costs boot + build-copy time. The image is sparse on the
# build HOST (rm+truncate), so only the ISO/RAM cost scales, not build disk.
DISK_IMAGE ?= $(CURDIR)/disk/os64.img
DISK_OFFSET ?= 1048576
DISK_SIZE_MB ?= 64
DISK_PARTUUID ?= 2f4fd02e-68b4-4c82-98bc-72467529b3fc

# ---- The ext2 test partition (partition 2) --------------------------------
# Formatted by REAL e2fsprogs on the build host and populated by debugfs —
# os64 never writes a byte of it. That's the point: the kernel's ext2 driver
# must parse a filesystem someone ELSE made (mkfs.ext2), with content written
# OUTSIDE the OS. Block size pinned to 1024 so pattern.bin's 1.5MB reliably
# exercises direct, single-indirect AND double-indirect block maps (see
# tools/gen_ext2_testdata.py for the math).
# The partition rides the same image (and therefore the ISO/ramdisk boots
# too), right after the FAT partition. The FAT partition keeps DISK_SIZE_MB
# and its GUID exactly as before — root mounting is untouched.
# 256, ruled 2026-08-07 with the writable-root ratification: half a gig was
# headroom nobody used, and the WHOLE image is what Limine copies to RAM on a
# ramdisk boot (~1s/32MB — the 30-second boot Chris timed was this number).
# 256 still quadruples the old 64MB FAT ceiling that made "test something
# BIG" a struggle, and halves the ramdisk toll.
EXT2_SIZE_MB ?= 256
EXT2_PARTUUID ?= 1ec5f5ab-71b7-45cd-a7a4-05646e878e57
EXT2_TEST_IMAGE ?= $(CURDIR)/disk/ext2_test.img
EXT2_STAGING ?= $(CURDIR)/disk/ext2_staging

# ---- The data disk: /home ------------------------------------------------
# A SEPARATE IMAGE FILE, and that is the entire point. Everything on
# $(DISK_IMAGE) is rewritten from scratch on every build (rm + sgdisk +
# mformat), so nothing written from INSIDE os64 can survive there — and it
# can't simply be made persistent, because the binaries on it must be rebuilt.
# A file the build creates ONCE and never touches again is the only shape that
# is both writable from the OS and durable across `make`. It is wired in as an
# ORDER-ONLY prerequisite (the `|` in the run targets below), which is exactly
# make's word for "ensure it exists, never rebuild it".
#
# It costs the P5 nothing: only $(DISK_IMAGE) rides the ISO as a Limine
# module, so this image adds zero bytes to the ISO and zero seconds to a
# ramdisk boot.
#
# SIZE: 1GB since 2026-08-18, and the old 64MB needs explaining because its
# reason evaporated twice over. 64MB was the FAT32 FLOOR — the smallest
# filesystem mkfs would make — chosen when /home held a husk.rc and nothing
# else. Then /home became where LOGD= writes, and a single os64.log had
# already reached 16MB; then /home became ext2, which has no floor at all.
# So the size is now chosen by what the partition is FOR: holding logs across
# many boots without the operator thinking about it. The file is sparse
# (truncate + mkfs writes metadata only), so an empty 1GB /home costs a few
# tens of MB of real disk and nothing on the ISO.
#
# 'home' is the GPT PARTITION NAME, and the kernel mounts partitions at
# /<partition name> (see vfs_mount_secondary_partitions). No /etc/fstab to
# invent: the partition says what it is called, and the mount believes it.
DATA_IMAGE ?= $(CURDIR)/disk/os64_data.img
DATA_SIZE_MB ?= 1024
DATA_PARTUUID ?= 7a3c1d90-4e62-4f3b-9a55-0c6f2b8e41d7
# NVMe, and the reason is a bug this disk found on its very first boot:
# **THE AHCI DRIVER CANNOT WRITE.** ahci.c installs ops->read and never
# ops->write, so /home on a SATA disk mounts, lists and reads perfectly — and
# the first `echo > /home/x` dispatches through a NULL function pointer into
# mapped page zero, executes it, and takes the machine down with RIP=0x3.
# (fat_glue.c's disk_write now refuses cleanly instead, but refusing is not
# the same as having a home you can write to.)
#
# Switch this back to `ahci` the day the AHCI write path exists — the coverage
# argument stands, and this disk is still the right way to get it: it was
# genuinely useful that one boot found a hole years of NVMe-only testing never
# went near.
DATA_DISK_IF ?= nvme
# Partition 2 starts right after partition 1: 2048 boot-gap sectors + the FAT
# partition. Total image adds 2MB slack for the GPT structures.
EXT2_START_SECTOR := $(shell echo $$((2048 + $(DISK_SIZE_MB) * 2048)))
TOTAL_DISK_MB := $(shell echo $$(($(DISK_SIZE_MB) + $(EXT2_SIZE_MB) + 2)))


# Define drive/device flags
QEMU_DRIVE_FLAGS = \
                  -drive file=$(DISK_IMAGE),if=none,id=nvme1 \
                  -device nvme,drive=nvme1,serial=nvme1-serial \
                  #-drive file=/home/yogi/disk_images/sata.img,if=none,id=sata1 \
                  #-device ahci,id=ahci1 \
                  #-device ide-hd,drive=sata1,bus=ahci1.0

# The data disk. Second controller, second disk — see DATA_DISK_IF above.
# NOTE: this is deliberately NOT folded into QEMU_DRIVE_FLAGS, whose last live
# line ends in a backslash followed by commented-out lines — anything appended
# there is swallowed by the continuation.
ifeq ($(DATA_DISK_IF),ahci)
QEMU_DATA_FLAGS = -drive file=$(DATA_IMAGE),if=none,id=data1 \
                  -device ahci,id=ahci0 \
                  -device ide-hd,drive=data1,bus=ahci0.0
else
QEMU_DATA_FLAGS = -drive file=$(DATA_IMAGE),if=none,id=data1 \
                  -device nvme,drive=data1,serial=data1-serial
endif

# Combine all flags
QEMUFLAGS_ADD = $(QEMU_BASE_FLAGS) $(QEMU_DRIVE_FLAGS) $(QEMU_DATA_FLAGS)

# Use the combined flags in your target
$(call USER_VARIABLE,QEMUFLAGS,$(QEMUFLAGS_ADD))


override IMAGE_NAME := os64_kernel

# Say it out loud: a rule accidentally placed above `all` would otherwise
# silently become the default goal (make picks the FIRST target in the file),
# and `make` would quietly build one binary instead of the OS.
.DEFAULT_GOAL := all

# ---- What lands on the disk image -----------------------------------------
# The userland apps are DISCOVERED, exactly the way userland/GNUmakefile does
# it (apps/<name>/<name>.c => an app called <name>). So "drop a directory and
# it builds" now also means "drop a directory and it lands on the image" — no
# mcopy line to add, ever. (It used to need both, and a forgotten mcopy meant
# an app that built perfectly and simply wasn't there at boot.)
# $(sort) dedupes: multi-file apps (top.c + topMain.c) list their directory
# once per .c file — without it, top appears twice in every list built here.
USERLAND_APPS := $(sort $(notdir $(patsubst %/,%,$(dir $(wildcard userland/apps/*/*.c)))))
USERLAND_BINS := $(addprefix userland/bin/,$(USERLAND_APPS))

# The shared libraries every one of those apps now needs to run at all
# (2026-08-22). This is not optional cargo: since libos64 became a .so, an
# image with /bin but no /lib/libos64.so boots to a husk that cannot start —
# and cannot start anything else either. It goes on EVERY volume that carries
# a /bin, which today means the ext2 root AND the FAT lifeboat. The lifeboat
# gets its own copy on its own partition on purpose: the whole point of a
# lifeboat is that it does not share machinery with the thing it exists to
# repair, so a stray write that eats the root's library leaves the lifeboat's
# untouched.
USERLAND_LIBS := userland/bin/libos64.so

# Kernel-side ring-3 test fixtures that also ride the image.
KERNEL_FIXTURES := $(addprefix kernel/bin/,test_elf arg_echo dyn_consumer \
                     syscall_smoke exit_by_return file_io redirect_io dir_list map_unmap cwd_test stat_test sleep_test memory_test hog env_fill glutton libtest.so)
KERNEL_BIN      := kernel/bin/$(IMAGE_NAME)


.PHONY: all
all: $(IMAGE_NAME).iso

.PHONY: all-hdd
all-hdd: $(IMAGE_NAME).hdd

# The sub-makes are the authority on whether these need rebuilding, so recurse
# unconditionally — but let the FILE TIMESTAMPS decide what happens downstream.
# The empty recipe (`;`) is the point: the sub-make either updates the binary
# (new mtime -> the disk image and ISO rebuild) or leaves it alone (old mtime ->
# they don't). That is what makes `make run` cheap when nothing changed.
$(KERNEL_BIN) $(KERNEL_FIXTURES): kernel ;
$(USERLAND_BINS) $(USERLAND_LIBS): userland ;

# Local qemu: ~/src/qemu-9.2.0-rc0/build/
.PHONY: run
run: $(IMAGE_NAME).iso | $(DATA_IMAGE)
	qemu-system-x86_64 \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		$(QEMUFLAGS)

debug: $(IMAGE_NAME).iso | $(DATA_IMAGE)
	qemu-system-x86_64 \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		$(QEMUFLAGS) $(QEMUDEBUGFLAGS)

# ── Network-attached variants ────────────────────────────────────────────────
# Same run/debug, plus a virtio NIC on QEMU's user-mode NAT (guest 10.0.2.15,
# gateway .2, DNS .3 — the convention the whole net stack's defaults match)
# and a pcap of everything on the virtual wire (net_capture.pcap — the serial
# log of the networking arc; read it with tcpdump/wireshark, or ask Fable).
#
# The net tests SKIP without a NIC, so anything network-shaped — debugging
# /bin/dialtest, watching DHCP lease, chasing a checksum — MUST boot one of
# THESE: under plain `make debug` the OS is netless, dialtest never spawns,
# and a breakpoint in its main() waits forever for a program that never runs
# (a lesson with a specific 2026-08-01 morning attached to it).
QEMU_NET_FLAGS = -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
                 -object filter-dump,id=dump0,netdev=n0,file=net_capture.pcap

.PHONY: run-net
run-net: $(IMAGE_NAME).iso
	qemu-system-x86_64 \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		$(QEMUFLAGS) $(QEMU_NET_FLAGS)

# Same slirp harness, e1000 instead of virtio — the INTx doorbell's test
# bench (2026-08-06). Boot this to see "e1000: interrupts live — INTx
# GSI <n> -> vector 0x45 (probe-confirmed)" on the glass; run-net stays
# virtio on purpose, as the polled-path regression twin. Same 10.0.2.x
# world, same pcap.
QEMU_E1000_FLAGS = -netdev user,id=n0 -device e1000,netdev=n0 \
                   -object filter-dump,id=dump0,netdev=n0,file=net_capture.pcap

.PHONY: run-e1000
run-e1000: $(IMAGE_NAME).iso
	qemu-system-x86_64 \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		$(QEMUFLAGS) $(QEMU_E1000_FLAGS)

# The REAL-internet variant: a host tap device instead of slirp, because
# slirp cannot relay ICMP to the outside world (ping 8.8.8.8 dies in the
# NAT). Boot the "/QEMU Boot (TAP net)" Limine entry — it carries the
# static IP=10.0.3.15 GW=10.0.3.1 the tap subnet expects.
#
# ONE-TIME HOST SETUP (needs sudo; survives until WSL restarts):
#   sudo bash -c 'ip tuntap add dev tap0 mode tap user $(USER) && \
#     ip addr add 10.0.3.1/24 dev tap0 && ip link set tap0 up && \
#     sysctl -w net.ipv4.ip_forward=1 && \
#     iptables -t nat -A POSTROUTING -s 10.0.3.0/24 -o eth0 -j MASQUERADE && \
#     iptables -P FORWARD ACCEPT'
#   (iptables: `sudo apt-get install -y iptables` if the distro lacks it)
QEMU_TAP_FLAGS = -netdev tap,id=n0,ifname=tap0,script=no,downscript=no \
                 -device virtio-net-pci,netdev=n0 \
                 -object filter-dump,id=dump0,netdev=n0,file=net_capture.pcap

.PHONY: run-tap
run-tap: $(IMAGE_NAME).iso
	qemu-system-x86_64 \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		$(QEMUFLAGS) $(QEMU_TAP_FLAGS)

.PHONY: debug-net
debug-net: $(IMAGE_NAME).iso
	qemu-system-x86_64 \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		$(QEMUFLAGS) $(QEMUDEBUGFLAGS) $(QEMU_NET_FLAGS)

.PHONY: debug-hdd-eufi
debug-hdd-eufi: ovmf/ovmf-code-x86_64.fd $(IMAGE_NAME).hdd $(DISK_IMAGE) | $(DATA_IMAGE)
	qemu-system-x86_64 \
		-drive if=pflash,unit=0,format=raw,file=ovmf/ovmf-code-x86_64.fd,readonly=on \
		-hda $(IMAGE_NAME).hdd \
		$(QEMUFLAGS) $(QEMUDEBUGFLAGS)

.PHONY: run-uefi
run-uefi: ovmf/ovmf-code-x86_64.fd $(IMAGE_NAME).iso | $(DATA_IMAGE)
	qemu-system-x86_64 \
		-drive if=pflash,unit=0,format=raw,file=ovmf/ovmf-code-x86_64.fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		$(QEMUFLAGS)

.PHONY: run-hdd
run-hdd: $(IMAGE_NAME).hdd $(DISK_IMAGE) | $(DATA_IMAGE)
	qemu-system-x86_64 \
		-hda $(IMAGE_NAME).hdd \
		$(QEMUFLAGS)

.PHONY: run-hdd-uefi
run-hdd-uefi: ovmf/ovmf-code-x86_64.fd $(IMAGE_NAME).hdd $(DISK_IMAGE) | $(DATA_IMAGE)
	qemu-system-x86_64 \
		-drive if=pflash,unit=0,format=raw,file=ovmf/ovmf-code-x86_64.fd,readonly=on \
		-hda $(IMAGE_NAME).hdd \
		$(QEMUFLAGS)

ovmf/ovmf-code-x86_64.fd:
	mkdir -p ovmf
	curl -Lo $@ https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/ovmf-code-x86_64.fd

limine/limine:
	rm -rf limine
	git clone https://github.com/limine-bootloader/limine.git --branch=v8.x-binary --depth=1
	$(MAKE) -C limine

kernel-deps:
	./kernel/get-deps
	touch kernel-deps

.PHONY: kernel
kernel: kernel-deps
	$(MAKE) -C kernel
	$(MAKE) -C kernel test-elf

# The os64 userland (launch + libos64 + apps). Separate tree, its own
# makefile; produces ring-3 ELFs under userland/bin/ that disk-populate drops
# onto the image. Depends on nothing in the kernel build — it links only
# against the abi/ contract.
.PHONY: userland
userland:
	$(MAKE) -C userland

# The disk image is a REAL FILE TARGET, not .PHONY.
#
# It used to be phony, which meant every single `make run` destroyed the image
# (rm + sgdisk + mformat), re-mcopy'd all eleven files, and — because the ISO
# carries this image as a Limine module — forced xorriso to repack the whole
# 77MB ISO. Nothing downstream could EVER be up to date, because a phony
# prerequisite is by definition always out of date. Fixing the kernel's header
# dependencies bought nothing until this was fixed too.
#
# Now it rebuilds only when something that goes ON it actually changed. The
# recipe still recreates the image from scratch (that is the honest thing to do
# — it guarantees no stale files linger from a previous layout); what changed is
# WHEN it runs.
# The ext2 test image, built entirely with host tools (mkfs.ext2 + debugfs —
# no loop devices, no sudo). Since the mount table landed it is no longer
# test-data-only: it carries /bin, /lib and /partition_info too, so an
# ext2-ROOT boot (see the "/QEMU Boot (ext2 root)" Limine entry) finds the
# same programs a FAT boot does. Rebuilds when the generator OR any binary
# that rides it changes.
$(EXT2_TEST_IMAGE): tools/gen_ext2_testdata.py $(USERLAND_BINS) $(USERLAND_LIBS) $(KERNEL_FIXTURES) kernel/test/partition_info.txt etc/husk.rc etc/logd.conf etc/os64get.conf etc/hosts etc/net.conf GNUmakefile
	@mkdir -p "$$(dirname $(EXT2_TEST_IMAGE))"
	python3 tools/gen_ext2_testdata.py $(EXT2_STAGING)
	rm -f $(EXT2_TEST_IMAGE)
	truncate -s $(EXT2_SIZE_MB)M $(EXT2_TEST_IMAGE)
	# -b 1024 pins the block size the test math depends on; -O ^dir_index
	# keeps directories plain linear lists (dir_index is COMPAT — a linear
	# reader works either way — but pinning it keeps the on-disk layout the
	# driver's simplest case while the driver IS its simplest case).
	mkfs.ext2 -F -q -b 1024 -L OS64EXT2 -O ^dir_index $(EXT2_TEST_IMAGE)
	debugfs -w -f $(EXT2_STAGING)/debugfs.cmds $(EXT2_TEST_IMAGE) > /dev/null 2>&1
	# Second debugfs pass: the program payload. Generated here (not in the
	# python script) because the bin list is THIS makefile's knowledge — same
	# discovery as the FAT mcopy loop below, no per-app line to forget.
	# /etc and /tmp join /bin and /lib: the curated tree, now that root is
	# writable (ratified 2026-08-07). /etc/husk.rc is the SYSTEM's rc —
	# /home/husk.rc (the user's, on its own partition) still wins the
	# search; /fat/husk.rc remains the lifeboat's copy.
	printf 'mkdir /bin\nmkdir /lib\nmkdir /etc\nmkdir /tmp\ncd /etc\nwrite etc/husk.rc husk.rc\nwrite etc/logd.conf logd.conf\nwrite etc/os64get.conf os64get.conf\nwrite etc/hosts hosts\nwrite etc/net.conf net.conf\ncd /bin\n' > $(EXT2_STAGING)/debugfs_bins.cmds
	$(foreach b,$(USERLAND_BINS),printf 'write %s %s\n' "$(b)" "$(notdir $(b))" >> $(EXT2_STAGING)/debugfs_bins.cmds;)
	$(foreach f,$(KERNEL_FIXTURES),$(if $(filter %libtest.so,$(f)),,printf 'write %s %s\n' "$(f)" "$(notdir $(f))" >> $(EXT2_STAGING)/debugfs_bins.cmds;))
	# The ext2 partition introduces ITSELF (Chris caught it claiming to be
	# FAT — the one file that must never lie about which filesystem it's on).
	printf 'Ima ext2 partition on the NVME disk\n' > $(EXT2_STAGING)/partition_info.txt
	# /lib: libos64.so (what every app in /bin above is dynamically linked
	# against — without it nothing on this volume runs) and libtest.so (the
	# dynamic-linking regression fixture).
	printf 'cd /lib\nwrite userland/bin/libos64.so libos64.so\nwrite kernel/bin/libtest.so libtest.so\ncd /\nwrite %s partition_info\n' "$(EXT2_STAGING)/partition_info.txt" >> $(EXT2_STAGING)/debugfs_bins.cmds
	debugfs -w -f $(EXT2_STAGING)/debugfs_bins.cmds $(EXT2_TEST_IMAGE) > /dev/null 2>&1
	@echo "  ext2 test image rebuilt (mkfs.ext2 + debugfs, host-authored content + $(words $(USERLAND_APPS)) apps)"

$(DISK_IMAGE): $(KERNEL_BIN) $(KERNEL_FIXTURES) $(USERLAND_BINS) $(USERLAND_LIBS) kernel/test/partition_info.txt etc/husk.rc limine-hd.conf $(wildcard external/*) $(EXT2_TEST_IMAGE) GNUmakefile
	@mkdir -p "$$(dirname $(DISK_IMAGE))"
	# rm + truncate instead of dd-from-/dev/zero: creates a sparse file, so
	# rebuilding the image doesn't write $(DISK_SIZE_MB)MB of zeros each time.
	# The rm matters: truncate alone would leave stale GPT/FAT bytes behind.
	rm -f $(DISK_IMAGE)
	truncate -s $(TOTAL_DISK_MB)M $(DISK_IMAGE)
	# Partition 1 (FAT root): same start, same size, same GUID as always —
	# now explicitly bounded (+$(DISK_SIZE_MB)M) instead of "fill the image",
	# because partition 2 lives after it.
	# The GPT NAMES ("fat", "ext2") are now load-bearing: os64 mounts each
	# partition at /<partition name>. They were "os64" and "ext2test", which
	# would have moved the mounts to /os64 and /ext2test — these spellings
	# keep every path exactly where it has always been. (A VBox VDI that
	# hasn't had `make vbox-sync` run against it still carries the old names
	# and will mount at /os64 until it does.)
	sgdisk $(DISK_IMAGE) --new=1:2048:+$(DISK_SIZE_MB)M --typecode=1:0700 --change-name=1:"fat" --partition-guid=1:$(DISK_PARTUUID)
	# Partition 2 (ext2 test): 0:0 = next free spot, fill the remainder.
	sgdisk $(DISK_IMAGE) --new=2:0:0 --typecode=2:8300 --change-name=2:"ext2" --partition-guid=2:$(EXT2_PARTUUID)
	# -T bounds the FAT filesystem to partition 1's sectors — mformat cannot
	# be allowed to size itself from the file, which now extends past the
	# partition and into ext2 territory.
	mformat -F -T $(shell echo $$(($(DISK_SIZE_MB) * 2048))) -i $(DISK_IMAGE)@@$(DISK_OFFSET) ::
	# Drop the host-authored ext2 filesystem into partition 2, byte for byte.
	dd if=$(EXT2_TEST_IMAGE) of=$(DISK_IMAGE) bs=512 seek=$(EXT2_START_SECTOR) conv=notrunc,sparse status=none
	-@mmd -i $(DISK_IMAGE)@@$(DISK_OFFSET) ::/bin > /dev/null 2>&1
	-@mmd -i $(DISK_IMAGE)@@$(DISK_OFFSET) ::/lib > /dev/null 2>&1
	$(foreach f,$(KERNEL_FIXTURES),$(if $(filter %libtest.so,$(f)),,\
	    mcopy -o -i $(DISK_IMAGE)@@$(DISK_OFFSET) $(f) ::/bin/$(notdir $(f));))
	# Every discovered userland app, automatically — no per-app line to forget.
	$(foreach b,$(USERLAND_BINS),\
	    mcopy -o -i $(DISK_IMAGE)@@$(DISK_OFFSET) $(b) ::/bin/$(notdir $(b));)
	# The lifeboat's own /lib. Its /bin is dynamically linked exactly like
	# root's, so this file is what makes the lifeboat a working system rather
	# than a partition full of programs that cannot start.
	mcopy -o -i $(DISK_IMAGE)@@$(DISK_OFFSET) userland/bin/libos64.so ::/lib/libos64.so
	mcopy -o -i $(DISK_IMAGE)@@$(DISK_OFFSET) kernel/bin/libtest.so ::/lib/libtest.so
	mcopy -o -i $(DISK_IMAGE)@@$(DISK_OFFSET) kernel/test/partition_info.txt ::/partition_info
	# husk's startup script. It rides the FAT partition rather than root
	# because the DEFAULT boot entry mounts ext2 as root, and ext2 is
	# read-only by design here — /etc/husk.rc would not merely be awkward to
	# update from inside the OS, it would be unwritable. On this boot it
	# appears as /fat/husk.rc; on a FAT-root boot the same file is /husk.rc,
	# which is why husk looks in both places.
	mcopy -o -i $(DISK_IMAGE)@@$(DISK_OFFSET) etc/husk.rc ::/husk.rc
	# ── The HD-boot payload (2026-08-21) ─────────────────────────────────
	# Kernel + boot modules + the HD-boot menu ride the FAT partition at
	# /boot, so p5-refresh.sh's ordinary mirror makes the P5's lifeboat a
	# complete boot volume — Limine installed on the machine's ESP (one-time
	# setup in p5-refresh.sh's header) finds this conf and boots this
	# kernel, and a running os64 can update BOTH through /fat. ISO and QEMU
	# boots never see any of it: Limine prefers its own boot volume's conf
	# (proven by decoy in both firmwares — see limine-hd.conf's header).
	# ~4.3MB against the partition's ~54MB of headroom.
	-@mmd -i $(DISK_IMAGE)@@$(DISK_OFFSET) ::/boot > /dev/null 2>&1
	-@mmd -i $(DISK_IMAGE)@@$(DISK_OFFSET) ::/boot/limine > /dev/null 2>&1
	-@mmd -i $(DISK_IMAGE)@@$(DISK_OFFSET) ::/EFI > /dev/null 2>&1
	-@mmd -i $(DISK_IMAGE)@@$(DISK_OFFSET) ::/EFI/BOOT > /dev/null 2>&1
	mcopy -o -i $(DISK_IMAGE)@@$(DISK_OFFSET) kernel/bin/$(IMAGE_NAME) ::/boot/os64_kernel
	$(foreach e,$(wildcard external/*),\
	    mcopy -o -i $(DISK_IMAGE)@@$(DISK_OFFSET) $(e) ::/boot/$(notdir $(e));)
	mcopy -o -i $(DISK_IMAGE)@@$(DISK_OFFSET) limine-hd.conf ::/boot/limine/limine.conf
	# Limine's own EFI app rides too — the lifeboat is a complete BOOT
	# VOLUME: one firmware NVRAM entry pointing here ("ESP" is a convention,
	# not a law — UEFI boots an EFI app from any FAT partition an entry
	# names) and Limine boots from this partition, finds the conf above ON
	# its own volume (the only place Limine 8.7 looks — the cross-volume
	# search the first design assumed does not exist; found by rehearsal,
	# 2026-08-21), and loads the kernel beside it. The Windows ESP is never
	# touched, and a running os64 can update kernel, menu, AND bootloader
	# through /fat.
	mcopy -o -i $(DISK_IMAGE)@@$(DISK_OFFSET) limine/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	@echo "  disk image rebuilt: $(words $(USERLAND_APPS)) apps ($(USERLAND_APPS))"

# The data disk. NO PREREQUISITES, DELIBERATELY: this rule fires exactly once,
# when the file does not exist, and never again — not when the kernel changes,
# not when an app changes, not when this makefile changes. That is what makes
# /home a place you can keep things.
#
# It is seeded with a husk.rc so there is something to edit on first boot; from
# then on YOUR copy is the one that runs, because nothing here will overwrite
# it. (etc/husk.rc is still copied fresh onto the FAT partition every build as
# the fallback default — see the mcopy above.)
$(DATA_IMAGE):
	@mkdir -p "$$(dirname $(DATA_IMAGE))"
	truncate -s $(shell echo $$(($(DATA_SIZE_MB) + 2)))M $(DATA_IMAGE)
	# --change-name IS the mount point: os64 mounts this partition at /home.
	# typecode 8300 (Linux filesystem) since /home became ext2 — the mount
	# router recognizes filesystems by probing, not by GPT type, but a
	# partition table that lies about its contents is a trap for host tools.
	sgdisk $(DATA_IMAGE) --new=1:2048:+$(DATA_SIZE_MB)M --typecode=1:8300 --change-name=1:"home" --partition-guid=1:$(DATA_PARTUUID)
	# EXT2, NOT FAT (converted 2026-08-18). The P5 has run both root and
	# /home on ext2 for a while; QEMU was still handing /home a FAT32
	# partition, which meant the test rig could reproduce an entire class of
	# failure the real machine cannot have — and did, twice in one evening:
	#
	#   * FatFs's FF_FS_LOCK makes a write-open EXCLUSIVE, so logd holding
	#     the log file starved `tail`, which reported "follow failed (-2)";
	#   * and when tail won the race instead, logd could not reopen for five
	#     seconds, gave up, and released the log sink.
	#
	# (see DEBTS: "tail -f killed logd via FF_FS_LOCK write-exclusion")
	#
	# ext2 has no such exclusion and is full write-through, so an appended
	# file reads at true length immediately — which is what makes tailing a
	# live log work at all. Same geometry as the root image (-b 1024,
	# ^dir_index) so the driver meets one layout everywhere.
	mkfs.ext2 -F -q -b 1024 -L OS64HOME -O ^dir_index -E offset=$(DISK_OFFSET) $(DATA_IMAGE) $(DATA_SIZE_MB)M
	@mkdir -p $(EXT2_STAGING)
	printf 'cd /\nwrite etc/husk.rc husk.rc\n' > $(EXT2_STAGING)/debugfs_home.cmds
	debugfs -w -f $(EXT2_STAGING)/debugfs_home.cmds "$(DATA_IMAGE)?offset=$(DISK_OFFSET)" > /dev/null 2>&1
	@echo "  data disk CREATED at $(DATA_IMAGE) ($(DATA_SIZE_MB)MB, /home, ext2) — this happens once; only 'make distclean' removes it"

# Compatibility aliases. disk-populate/disk-init now just mean "make sure the
# image is current"; `disk` FORCES a fresh one (the old always-rebuild behavior,
# for when you want to be certain).
.PHONY: disk-init disk-populate
disk-init disk-populate: $(DISK_IMAGE)

.PHONY: disk
disk:
	rm -f $(DISK_IMAGE)
	$(MAKE) $(DISK_IMAGE)

# Judge what os64 wrote (the ext2 write driver's outside authority, since
# 2026-08-04): pull the ext2 partition out of the disk image and run the
# host's e2fsck over it, read-only, forced full pass. Run AFTER a QEMU
# session that exercised ext2 writes — e2fsck independently recomputes every
# structure the driver maintains (bitmaps, free counts, i_blocks including
# indirect blocks, links_count, bg_used_dirs_count), so a green exit here is
# the strongest single verdict the write path can earn.
# The dd copy means the image itself is never touched; on a FAILING check
# the extracted copy is left at /tmp/os64_fsck_ext2.img for the autopsy.
.PHONY: fsck-ext2
# /home joined the constitution 2026-08-18, the day it became ext2: a green
# e2fsck on root alone stopped being the whole verdict the moment a second
# ext2 filesystem started taking the write driver's traffic (and logd makes
# /home the BUSIER of the two). The data image is checked in place via
# e2fsprogs' ?offset= syntax — no gigabyte copy, and -fn never writes.
fsck-ext2: | $(DATA_IMAGE)
	@dd if=$(DISK_IMAGE) of=/tmp/os64_fsck_ext2.img bs=512 \
	    skip=$(EXT2_START_SECTOR) count=$$(($(EXT2_SIZE_MB) * 2048)) status=none
	e2fsck -fn /tmp/os64_fsck_ext2.img
	@rm -f /tmp/os64_fsck_ext2.img
	e2fsck -fn "$(DATA_IMAGE)?offset=$(DISK_OFFSET)"

# Refresh compile_commands.json — the per-file compiler flags VSCode's
# IntelliSense runs on (see .vscode/c_cpp_properties.json). bear records the
# real invocations, so it needs them to RUN: this is a full rebuild. Rerun
# after adding new source files; nothing else needs it.
# -k (keep going): a half-written app mustn't truncate the database — bear
# records every compile it SEES, so the more of the build that runs past a
# broken file, the more complete the record. The build failing is fine here;
# compdb's product is the json, not the ISO.
.PHONY: compdb
compdb:
	-bear -- $(MAKE) -B -k

# ---- VirtualBox disk sync -------------------------------------------------
# The VBox VM boots the DVD from C:/temp/os64_kernel.iso (refreshed by the
# ISO rule above) and uses a FIXED-format VDI as its NVMe hard disk. A fixed
# VDI is just a small header + block map + the raw disk bytes laid out flat,
# so syncing it is one dd of $(DISK_IMAGE) into the data area — no VBoxManage
# needed, and the medium UUID VBox has registered is untouched.
#
# THE VM MUST BE POWERED OFF when running this.
#
# One-time setup (already done 2026-07-03; repeat only if DISK_SIZE_MB
# changes or the VDI is recreated):
#   1. qemu-img convert -f raw -O vdi -o static=on $(DISK_IMAGE) new.vdi
#   2. Preserve the registered medium UUID (16 bytes at offset 0x188):
#      dd if=old.vdi of=new.vdi bs=1 skip=392 seek=392 count=16 conv=notrunc
#   3. Move new.vdi into place at $(VBOX_VDI).
VBOX_VDI ?= /mnt/i/Virtualbox_VMs/OS64/OS64/OS64_NVME.vdi

.PHONY: vbox-sync
vbox-sync: $(DISK_IMAGE)
	@if [ ! -f "$(VBOX_VDI)" ]; then echo "vbox-sync: VDI not found: $(VBOX_VDI)"; exit 1; fi
	@TYPE=$$(od -An -t u4 -j 76 -N 4 "$(VBOX_VDI)" | tr -d ' '); \
	if [ "$$TYPE" != "2" ]; then \
		echo "vbox-sync: ERROR: $(VBOX_VDI) is not a FIXED VDI (type=$$TYPE) — see one-time setup comments above this target"; exit 1; \
	fi
	@CAP=$$(od -An -t u8 -j 368 -N 8 "$(VBOX_VDI)" | tr -d ' '); \
	IMG=$$(stat -c %s "$(DISK_IMAGE)"); \
	if [ "$$CAP" != "$$IMG" ]; then \
		echo "vbox-sync: ERROR: VDI capacity ($$CAP) != $(DISK_IMAGE) size ($$IMG) — recreate the VDI (one-time setup)"; exit 1; \
	fi
	@OFF=$$(od -An -t u4 -j 344 -N 4 "$(VBOX_VDI)" | tr -d ' '); \
	echo "vbox-sync: writing $(DISK_IMAGE) into $(VBOX_VDI) at data offset $$OFF (VM must be POWERED OFF)"; \
	dd if="$(DISK_IMAGE)" of="$(VBOX_VDI)" bs=1M oflag=seek_bytes seek=$$OFF conv=notrunc status=none
	@echo "vbox-sync: done"

# Removed this from both top and bottom of the next section
#	rm -rf iso_root
# Real file prerequisites, so xorriso only repacks the 77MB ISO when something
# on it actually changed (it used to depend on the phony disk-populate, which
# guaranteed a full repack on every single make).
$(IMAGE_NAME).iso: limine/limine $(KERNEL_BIN) $(DISK_IMAGE) limine.conf tools/p5-refresh.sh
	@mkdir -p iso_root/boot
	cp kernel/bin/$(IMAGE_NAME) iso_root/boot/
	# The P5 refresh script rides at the ISO root: boot the target machine's
	# Linux, mount the stick, `sudo bash <stick>/p5-refresh.sh <stick>` — the
	# ISO carries its own installer (one-time setup lives in its header).
	cp tools/p5-refresh.sh iso_root/
	# The QEMU NVMe disk image rides along on the ISO as a Limine module so
	# RAMDISK boot entries can mount it as root with no physical disk prep
	# (see /RAMDisk Boot in limine.conf). Copied after disk-populate so the
	# module always carries the freshly built test binaries.
	cp $(DISK_IMAGE) iso_root/boot/os64_disk.img
	@cp external/* iso_root/boot/
	@cp external/* iso_root/
	@mkdir -p iso_root/boot/limine
	@cp limine.conf limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/boot/limine/
	@mkdir -p iso_root/EFI/BOOT
	@cp limine/BOOTX64.EFI iso_root/EFI/BOOT/
	@cp limine/BOOTIA32.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(IMAGE_NAME).iso > /dev/null
	./limine/limine bios-install $(IMAGE_NAME).iso > /dev/null
# The c:/temp export feeds Chris's VBox/P5 boots — a WSL2 convenience, not
# part of the build contract. Gated on the directory existing (PR #26): an
# unconditional cp made `make` FAIL on any machine without /mnt/c/temp,
# which is every machine that isn't this one.
	@if [ -d /mnt/c/temp ]; then \
		rm -f /mnt/c/temp/os64_kernel.iso; \
		cp -v os64_kernel.iso /mnt/c/temp; \
	fi

$(IMAGE_NAME).hdd: limine/limine kernel
#	@rm -f $(IMAGE_NAME).hdd
#	@dd if=/dev/zero bs=1M count=0 seek=1024 of=$(IMAGE_NAME).hdd
#	# Create the EFI partition
#	@sgdisk $(IMAGE_NAME).hdd --new=1:2048:+64M --typecode=1:EF00 --change-name=1:"EFI System Partition"
#	# Create the second partition (480 MB)
#	@sgdisk $(IMAGE_NAME).hdd --new=2:133120:+480M --typecode=2:8300 --change-name=2:"ext2_part"
#	# Create the third partition (480 MB)
#	@sgdisk $(IMAGE_NAME).hdd  --new=3:1116160:+400M --typecode=3:0700 --change-name=3:"win_part"
#	sudo losetup /dev/loop2 -P $(IMAGE_NAME).hdd
#	sudo mkfs.fat -F32 /dev/loop2p2
#	sudo mkfs.ext2 /dev/loop2p3
#	sudo losetup -d /dev/loop2
#	@./limine/limine bios-install $(IMAGE_NAME).hdd
#	@mformat -i $(IMAGE_NAME).hdd@@1048576 ::
#	# Create directories on the EFI partition
#	@mmd -i $(IMAGE_NAME).hdd@@1048576 ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
	# Copy bootloader and kernel files to the EFI partition
	@mcopy -i $(IMAGE_NAME).hdd@@1048576 kernel/bin/os64_kernel ::/boot
#	@mcopy -i $(IMAGE_NAME).hdd@@1048576 limine.conf limine/limine-bios.sys ::/boot/limine
#	@mcopy -i $(IMAGE_NAME).hdd@@1048576 limine/BOOTX64.EFI ::/EFI/BOOT
#	@mcopy -i $(IMAGE_NAME).hdd@@1048576 limine/BOOTIA32.EFI ::/EFI/BOOT
#	@mcopy -i $(IMAGE_NAME).hdd@@1048576 limine/limine-bios.sys  ::/EFI/BOOT
#	@mcopy -i $(IMAGE_NAME).hdd@@1M external/* ::/boot

.PHONY: clean
clean:
	$(MAKE) -C kernel clean
#	rm -rf iso_root $(IMAGE_NAME).iso $(IMAGE_NAME).hdd

.PHONY: distclean
distclean: clean
	$(MAKE) -C kernel distclean
	rm -rf kernel-deps limine ovmf
	# The data disk dies HERE and nowhere else. It is the only file in the tree
	# holding things written from inside os64 — /home — and no ordinary build
	# step may remove it. distclean says "back to a fresh clone", and that
	# includes losing your home directory, so it is spelled out rather than
	# folded into the rm above.
	rm -f $(DATA_IMAGE)
