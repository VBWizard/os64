OUTPUT ?= ../../bin/test_elf
DYN_OUTPUT ?= ../../bin/dyn_consumer
ARG_OUTPUT ?= ../../bin/arg_echo
SMOKE_OUTPUT ?= ../../bin/syscall_smoke
RETEXIT_OUTPUT ?= ../../bin/exit_by_return
FILEIO_OUTPUT ?= ../../bin/file_io
REDIR_OUTPUT ?= ../../bin/redirect_io
DIRLIST_OUTPUT ?= ../../bin/dir_list
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
# -mno-mmx/-mno-sse/-mno-sse2: SIMD is #UD until CR4.OSFXSR + FPU context
# switching exist (see ARG_CFLAGS note).
DYN_CFLAGS ?= -g -m64 -ffreestanding -fpie -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -fno-stack-protector -fcf-protection=none -nostdlib -nostartfiles -nodefaultlibs -masm=intel -Wall -Wextra
# --hash-style=sysv pins the DT_HASH table the kernel's loader derives the
# dynamic symbol count from (nchain). Bare-metal binutils defaults to sysv
# today, but hosted targets default to gnu-only — pinning it costs nothing
# and keeps a future toolchain upgrade from silently producing binaries
# whose symbols the kernel can't count (which would surface as a relocation
# panic at first page fault, far from the real cause).
DYN_LDFLAGS ?= -pie -z now -e _start --no-allow-shlib-undefined --hash-style=sysv

# arg_echo is a static, freestanding C fixture. It gets the same preemption
# hardening as dyn_consumer (-mno-red-zone etc.) because it runs with interrupts
# enabled and can be preempted mid-function, and -masm=intel to match the
# project-wide asm syntax convention.
# -mno-mmx/-mno-sse/-mno-sse2: CR4.OSFXSR is off, so SIMD is #UD even at
# ring 3 — same rule as the kernel until the FPU-state slice enables it.
# -I../../../abi/include: fixtures take syscall numbers AND raw stubs from the
# abi contract like every other ring-3 binary — a fixture with hand-copied
# numbers is one renumbering away from testing the wrong syscall (the exact
# drift class the abi/ split exists to kill; older fixtures predate the path
# and still carry local copies — migrate them as they're touched).
ARG_CFLAGS ?= -g -m64 -ffreestanding -fno-pic -fno-pie -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -fno-stack-protector -fcf-protection=none -nostdlib -nostartfiles -nodefaultlibs -masm=intel -Wall -Wextra -I../../../abi/include

OBJS := serial_ping.o

.PHONY: all
all: $(OUTPUT) $(DYN_OUTPUT) $(ARG_OUTPUT) $(SMOKE_OUTPUT) $(RETEXIT_OUTPUT) $(FILEIO_OUTPUT) $(REDIR_OUTPUT) $(DIRLIST_OUTPUT)

$(OUTPUT): $(OBJS)
	@mkdir -p "$$(dirname $@)"
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

serial_ping.o: serial_ping.S
	$(CC) $(CFLAGS) -c $< -o $@

$(ARG_OUTPUT): arg_echo.o
	@mkdir -p "$$(dirname $@)"
	$(LD) $(LDFLAGS) -o $@ arg_echo.o

arg_echo.o: arg_echo.c
	$(CC) $(ARG_CFLAGS) -c $< -o $@

# The ring-3 syscall fixtures are static freestanding C like arg_echo, so they
# share ARG_CFLAGS (-mno-red-zone matters even more here: these actually run
# at CPL 3 with interrupts enabled and WILL be preempted).
$(SMOKE_OUTPUT): syscall_smoke.o
	@mkdir -p "$$(dirname $@)"
	$(LD) $(LDFLAGS) -o $@ syscall_smoke.o

syscall_smoke.o: syscall_smoke.c
	$(CC) $(ARG_CFLAGS) -c $< -o $@

$(RETEXIT_OUTPUT): exit_by_return.o
	@mkdir -p "$$(dirname $@)"
	$(LD) $(LDFLAGS) -o $@ exit_by_return.o

exit_by_return.o: exit_by_return.c
	$(CC) $(ARG_CFLAGS) -c $< -o $@

$(FILEIO_OUTPUT): file_io.o
	@mkdir -p "$$(dirname $@)"
	$(LD) $(LDFLAGS) -o $@ file_io.o

file_io.o: file_io.c
	$(CC) $(ARG_CFLAGS) -c $< -o $@

$(REDIR_OUTPUT): redirect_io.o
	@mkdir -p "$$(dirname $@)"
	$(LD) $(LDFLAGS) -o $@ redirect_io.o

redirect_io.o: redirect_io.c
	$(CC) $(ARG_CFLAGS) -c $< -o $@

$(DIRLIST_OUTPUT): dir_list.o
	@mkdir -p "$$(dirname $@)"
	$(LD) $(LDFLAGS) -o $@ dir_list.o

dir_list.o: dir_list.c
	$(CC) $(ARG_CFLAGS) -c $< -o $@

$(DYN_OUTPUT): dyn_consumer.o $(LIBTEST)
	@mkdir -p "$$(dirname $@)"
	$(LD) $(DYN_LDFLAGS) -o $@ dyn_consumer.o $(LIBTEST)

dyn_consumer.o: dyn_consumer.c
	$(CC) $(DYN_CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f $(OBJS) $(OUTPUT) dyn_consumer.o $(DYN_OUTPUT) arg_echo.o $(ARG_OUTPUT) syscall_smoke.o $(SMOKE_OUTPUT) exit_by_return.o $(RETEXIT_OUTPUT) file_io.o $(FILEIO_OUTPUT) redirect_io.o $(REDIR_OUTPUT) dir_list.o $(DIRLIST_OUTPUT)
