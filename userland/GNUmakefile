# userland/GNUmakefile — builds the whole os64 userland: the launch stub,
# libos64, and every app under apps/. One makefile above them all (per the
# roadmap — build scale is a non-issue on the dev machines).
#
# Output: each app links to userland/bin/<app> as a static ring-3 ELF
# (launch + libos64 + the app), ready for the disk image.

CC ?= x86_64-elf-gcc
LD ?= x86_64-elf-ld

# Ring-3 freestanding, same hardening the fixtures earned the hard way:
#   -mno-red-zone : an interrupt firing mid-function (these run at CPL 3 with
#                   IF=1 and WILL be preempted) must not clobber red-zone locals
#   -mno-mmx/sse/sse2 : SIMD is #UD until CR4.OSFXSR + FPU context switching
#   -masm=intel   : project-wide asm syntax
#   -fno-pic/-fno-pie : static, fixed-base for now (PIE is a later item)
CFLAGS  ?= -g -m64 -ffreestanding -fno-pic -fno-pie -mno-red-zone \
           -mno-mmx -mno-sse -mno-sse2 -fno-stack-protector -fcf-protection=none \
           -nostdlib -nostartfiles -nodefaultlibs -masm=intel -Wall -Wextra \
           -I$(CURDIR)/../abi/include -I$(CURDIR)/libos64/include
LDFLAGS ?= -static -nostdlib -T $(CURDIR)/link/app.ld -z max-page-size=0x1000

BIN     := $(CURDIR)/bin
OBJ     := $(CURDIR)/obj

# libos64 objects + the launch stub link into every app.
RUNTIME_SRCS := start/launch.S libos64/io.c libos64/proc.c
RUNTIME_OBJS := $(patsubst %,$(OBJ)/%.o,$(RUNTIME_SRCS))

# Every apps/<name>/<name>.c is an app named <name>.
APPS := $(notdir $(patsubst %/,%,$(dir $(wildcard apps/*/*.c))))

.PHONY: all
all: $(addprefix $(BIN)/,$(APPS))

# Per-app link rule, generated per app: the app name appears in BOTH the
# directory and the filename, and a make pattern prerequisite allows only one
# `%` stem — so a single `$(BIN)/%: $(OBJ)/apps/%/%.c.o` rule can't express it.
# eval writes an explicit rule for each app instead (also the natural place to
# add per-app extras later).
define APP_RULE
$(BIN)/$(1): $(OBJ)/apps/$(1)/$(1).c.o $(RUNTIME_OBJS)
	@mkdir -p "$(BIN)"
	$(LD) $(LDFLAGS) -o $$@ $$< $(RUNTIME_OBJS)
endef
$(foreach app,$(APPS),$(eval $(call APP_RULE,$(app))))

$(OBJ)/%.c.o: %.c
	@mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/%.S.o: %.S
	@mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -rf $(OBJ) $(BIN)
