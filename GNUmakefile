# Nuke built-in rules and variables.
MAKEFLAGS += -rR
.SUFFIXES:
QEMUDEBUGFLAGS = -S -s
# Convenience macro to reliably declare user overridable variables.
override USER_VARIABLE = $(if $(filter $(origin $(1)),default undefined),$(eval override $(1) := $(2)))

# Default user QEMU flags. These are appended to the QEMU command calls.
# Send COM1 port output to the console with: -serial mon:stdio 
# Send debug logging to file with: -D qemu_debug.log 
# $(call USER_VARIABLE,QEMUFLAGS,-m 8g -smp 2 -no-reboot -serial file:qemu_com1.log -monitor "$(shell echo telnet:127.0.0.1:55555,server,nowait)" -d "$(shell echo int,cpu_reset,pcall,guest_errors)")

# Define the base QEMU flags
QEMU_BASE_FLAGS = -m 8g -smp 4 -no-reboot \
                  -serial file:qemu_com1.log \
                  -monitor $(shell echo telnet:127.0.0.1:55555,server,nowait) \
                  -d $(shell echo int,cpu_reset,pcall,guest_errors)
				  
# Define drive/device flags
QEMU_DRIVE_FLAGS = \
                  -drive file=/home/yogi/disk_images/nvme.img,if=none,id=nvme1 \
                  -device nvme,drive=nvme1,serial=nvme1-serial \
                  #-drive file=/home/yogi/disk_images/sata.img,if=none,id=sata1 \
                  #-device ahci,id=ahci1 \
                  #-device ide-hd,drive=sata1,bus=ahci1.0

# Combine all flags
QEMUFLAGS_ADD = $(QEMU_BASE_FLAGS) $(QEMU_DRIVE_FLAGS)

# Use the combined flags in your target
$(call USER_VARIABLE,QEMUFLAGS,$(QEMUFLAGS_ADD))


override IMAGE_NAME := os64_kernel

.PHONY: all
all: $(IMAGE_NAME).iso

.PHONY: all-hdd
all-hdd: $(IMAGE_NAME).hdd

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
debug-hdd-eufi: ovmf/ovmf-code-x86_64.fd $(IMAGE_NAME).hdd
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
run-hdd: $(IMAGE_NAME).hdd
	qemu-system-x86_64 \
		-machine q35 \
		-hda $(IMAGE_NAME).hdd \
		$(QEMUFLAGS)

.PHONY: run-hdd-uefi
run-hdd-uefi: ovmf/ovmf-code-x86_64.fd $(IMAGE_NAME).hdd
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

# Removed this from both top and bottom of the next section
#	rm -rf iso_root
$(IMAGE_NAME).iso: limine/limine kernel
	@mkdir -p iso_root/boot
	cp kernel/bin/$(IMAGE_NAME) iso_root/boot/
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
