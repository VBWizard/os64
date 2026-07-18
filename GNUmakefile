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
QEMU_BASE_FLAGS = -m 8g -no-reboot -smp 8 \
                  -serial file:qemu_com1.log \
                  -monitor $(shell echo telnet:127.0.0.1:55555,server,nowait) \
				  -d $(shell echo int,cpu_reset,pcall,guest_errors)
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


# Define drive/device flags
QEMU_DRIVE_FLAGS = \
                  -drive file=$(DISK_IMAGE),if=none,id=nvme1 \
                  -device nvme,drive=nvme1,serial=nvme1-serial \
                  #-drive file=/home/yogi/disk_images/sata.img,if=none,id=sata1 \
                  #-device ahci,id=ahci1 \
                  #-device ide-hd,drive=sata1,bus=ahci1.0

# Combine all flags
QEMUFLAGS_ADD = $(QEMU_BASE_FLAGS) $(QEMU_DRIVE_FLAGS)

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
USERLAND_APPS := $(notdir $(patsubst %/,%,$(dir $(wildcard userland/apps/*/*.c))))
USERLAND_BINS := $(addprefix userland/bin/,$(USERLAND_APPS))

# Kernel-side ring-3 test fixtures that also ride the image.
KERNEL_FIXTURES := $(addprefix kernel/bin/,test_elf arg_echo dyn_consumer \
                     syscall_smoke exit_by_return file_io redirect_io dir_list libtest.so)
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
$(USERLAND_BINS): userland ;

# Local qemu: ~/src/qemu-9.2.0-rc0/build/
.PHONY: run
run: $(IMAGE_NAME).iso
	qemu-system-x86_64 \
		-machine q35 \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		$(QEMUFLAGS)

debug: $(IMAGE_NAME).iso
	qemu-system-x86_64 \
		-machine q35 \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		$(QEMUFLAGS) $(QEMUDEBUGFLAGS)

.PHONY: debug-hdd-eufi
debug-hdd-eufi: ovmf/ovmf-code-x86_64.fd $(IMAGE_NAME).hdd $(DISK_IMAGE)
	qemu-system-x86_64 \
		-machine q35 \
		-drive if=pflash,unit=0,format=raw,file=ovmf/ovmf-code-x86_64.fd,readonly=on \
		-hda $(IMAGE_NAME).hdd \
		$(QEMUFLAGS) $(QEMUDEBUGFLAGS)

.PHONY: run-uefi
run-uefi: ovmf/ovmf-code-x86_64.fd $(IMAGE_NAME).iso
	qemu-system-x86_64 \
		-machine q35 \
		-drive if=pflash,unit=0,format=raw,file=ovmf/ovmf-code-x86_64.fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		$(QEMUFLAGS)

.PHONY: run-hdd
run-hdd: $(IMAGE_NAME).hdd $(DISK_IMAGE)
	qemu-system-x86_64 \
		-machine q35 \
		-hda $(IMAGE_NAME).hdd \
		$(QEMUFLAGS)

.PHONY: run-hdd-uefi
run-hdd-uefi: ovmf/ovmf-code-x86_64.fd $(IMAGE_NAME).hdd $(DISK_IMAGE)
	qemu-system-x86_64 \
		-machine q35 \
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
$(DISK_IMAGE): $(KERNEL_BIN) $(KERNEL_FIXTURES) $(USERLAND_BINS) kernel/test/partition_info.txt GNUmakefile
	@mkdir -p "$$(dirname $(DISK_IMAGE))"
	# rm + truncate instead of dd-from-/dev/zero: creates a sparse file, so
	# rebuilding the image doesn't write $(DISK_SIZE_MB)MB of zeros each time.
	# The rm matters: truncate alone would leave stale GPT/FAT bytes behind.
	rm -f $(DISK_IMAGE)
	truncate -s $(DISK_SIZE_MB)M $(DISK_IMAGE)
	# --new=1:2048:0 — end sector 0 means "largest possible": the partition
	# auto-fills the image (minus the backup GPT at the end), so DISK_SIZE_MB
	# is the single knob for image AND partition size.
	sgdisk $(DISK_IMAGE) --new=1:2048:0 --typecode=1:0700 --change-name=1:"os64" --partition-guid=1:$(DISK_PARTUUID)
	mformat -F -i $(DISK_IMAGE)@@$(DISK_OFFSET) ::
	-@mmd -i $(DISK_IMAGE)@@$(DISK_OFFSET) ::/bin > /dev/null 2>&1
	-@mmd -i $(DISK_IMAGE)@@$(DISK_OFFSET) ::/lib > /dev/null 2>&1
	$(foreach f,$(KERNEL_FIXTURES),$(if $(filter %libtest.so,$(f)),,\
	    mcopy -o -i $(DISK_IMAGE)@@$(DISK_OFFSET) $(f) ::/bin/$(notdir $(f));))
	# Every discovered userland app, automatically — no per-app line to forget.
	$(foreach b,$(USERLAND_BINS),\
	    mcopy -o -i $(DISK_IMAGE)@@$(DISK_OFFSET) $(b) ::/bin/$(notdir $(b));)
	mcopy -o -i $(DISK_IMAGE)@@$(DISK_OFFSET) kernel/bin/libtest.so ::/lib/libtest.so
	mcopy -o -i $(DISK_IMAGE)@@$(DISK_OFFSET) kernel/test/partition_info.txt ::/partition_info
	@echo "  disk image rebuilt: $(words $(USERLAND_APPS)) apps ($(USERLAND_APPS))"

# Compatibility aliases. disk-populate/disk-init now just mean "make sure the
# image is current"; `disk` FORCES a fresh one (the old always-rebuild behavior,
# for when you want to be certain).
.PHONY: disk-init disk-populate
disk-init disk-populate: $(DISK_IMAGE)

.PHONY: disk
disk:
	rm -f $(DISK_IMAGE)
	$(MAKE) $(DISK_IMAGE)

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
$(IMAGE_NAME).iso: limine/limine $(KERNEL_BIN) $(DISK_IMAGE) limine.conf
	@mkdir -p iso_root/boot
	cp kernel/bin/$(IMAGE_NAME) iso_root/boot/
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
	@rm -f /mnt/c/temp/os64_kernel.iso
	@cp -v os64_kernel.iso /mnt/c/temp

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
