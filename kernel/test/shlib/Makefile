OUTPUT ?= ../../bin/libtest.so

CC ?= x86_64-elf-gcc
LD ?= x86_64-elf-ld

# -mno-red-zone is essential here, not just style: without it, GCC may keep
# leaf-ish locals in the 128-byte SysV red zone below RSP without adjusting
# RSP at all. That's safe for user-mode code where the OS/runtime respects
# the red zone, but an interrupt/exception firing while this code runs pushes
# its frame starting AT the current RSP — directly into those red-zone
# locals — silently corrupting them. Matches the kernel's own CFLAGS for
# exactly this reason.
CFLAGS ?= -g -m64 -ffreestanding -fPIC -mno-red-zone -fno-stack-protector -fcf-protection=none -nostdlib -nostartfiles -nodefaultlibs -Wall -Wextra
# NOTE: -shared is NOT passed to $(CC) here — this cross toolchain's specs
# were built without shared-library support and silently drop it before the
# link step ever happens (confirmed via `gcc -### -shared ...`). Compiling
# with -fPIC still works fine; the final link just has to go through $(LD)
# directly instead of through the gcc driver.
# --hash-style=sysv: the kernel derives the dynamic symbol count from
# DT_HASH's nchain, so the SysV hash table must exist. It's the bare-metal
# binutils default today; pinning it guards against a toolchain upgrade
# switching to gnu-only hashing (which would break symbol lookup at runtime,
# far from the real cause).
LDFLAGS ?= -shared -z now -soname libtest.so --hash-style=sysv

OBJS := libtest.o

.PHONY: all
all: $(OUTPUT)

$(OUTPUT): $(OBJS)
	@mkdir -p "$$(dirname $@)"
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

libtest.o: libtest.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f $(OBJS) $(OUTPUT)
