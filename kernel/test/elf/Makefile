OUTPUT ?= ../../bin/test_elf
DYN_OUTPUT ?= ../../bin/dyn_consumer
LIBTEST ?= ../../bin/libtest.so

CC ?= x86_64-elf-gcc
LD ?= x86_64-elf-ld

CFLAGS ?= -g -m64 -ffreestanding -fno-pic -fno-pie -nostdlib -nostartfiles -nodefaultlibs -Wall -Wextra
LDFLAGS ?= -static -nostdlib -T linker.ld -z max-page-size=0x1000

# dyn_consumer is a PIE (position-independent) executable that dynamically
# links against libtest.so — a different code-generation model from the
# static test_elf binary above, so it gets its own flags.
# -mno-red-zone matters here for the same reason it does in test/shlib's
# Makefile: without it, an interrupt firing mid-function (e.g. the CoW page
# fault this binary is specifically built to exercise) corrupts red-zone
# locals by pushing its exception frame right on top of them.
# -masm=intel: all asm in this project — kernel and fixtures alike — is
# Intel syntax (see outb in dyn_consumer.c).
DYN_CFLAGS ?= -g -m64 -ffreestanding -fpie -mno-red-zone -fno-stack-protector -fcf-protection=none -nostdlib -nostartfiles -nodefaultlibs -masm=intel -Wall -Wextra
# --hash-style=sysv pins the DT_HASH table the kernel's loader derives the
# dynamic symbol count from (nchain). Bare-metal binutils defaults to sysv
# today, but hosted targets default to gnu-only — pinning it costs nothing
# and keeps a future toolchain upgrade from silently producing binaries
# whose symbols the kernel can't count (which would surface as a relocation
# panic at first page fault, far from the real cause).
DYN_LDFLAGS ?= -pie -z now -e _start --no-allow-shlib-undefined --hash-style=sysv

OBJS := serial_ping.o

.PHONY: all
all: $(OUTPUT) $(DYN_OUTPUT)

$(OUTPUT): $(OBJS)
	@mkdir -p "$$(dirname $@)"
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

serial_ping.o: serial_ping.S
	$(CC) $(CFLAGS) -c $< -o $@

$(DYN_OUTPUT): dyn_consumer.o $(LIBTEST)
	@mkdir -p "$$(dirname $@)"
	$(LD) $(DYN_LDFLAGS) -o $@ dyn_consumer.o $(LIBTEST)

dyn_consumer.o: dyn_consumer.c
	$(CC) $(DYN_CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f $(OBJS) $(OUTPUT) dyn_consumer.o $(DYN_OUTPUT)
