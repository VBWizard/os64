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
# -Werror in userland too (Chris, 2026-07-18): the const-discard that slipped
# into the first ls proved the workshop needs the same alarm as the cathedral —
# os64 does not repeat os32's three million warnings. The pressure valve for a
# deliberately half-built work-in-progress: `make NOWERROR=1` (or from the
# repo root: `make NOWERROR=1 run`) builds warnings-as-warnings for that run
# only. The default is strict; softness is a choice you make out loud.
WERROR_FLAG := $(if $(NOWERROR),,-Werror)
# -MMD -MP: header dependency tracking, same as the kernel makefile (and for
# the same reason, learned the same day it almost was learned twice): without
# it, growing a struct in args.h left every app's .o compiled against the OLD
# layout — ls read args.value 8 bytes off and got NULL, on 2026-07-19, hours
# after the kernel's identical lesson was already written down. The .d files
# land next to the .o's; the -include below pulls them in. In CFLAGS because
# CFLAGS is what the compile rules actually pass.
CFLAGS  ?= -g -m64 -ffreestanding -fno-pic -fno-pie -mno-red-zone \
           -mno-mmx -mno-sse -mno-sse2 -fno-stack-protector -fcf-protection=none \
           -nostdlib -nostartfiles -nodefaultlibs -masm=intel -Wall -Wextra \
           $(WERROR_FLAG) -MMD -MP \
           -I$(CURDIR)/../abi/include -I$(CURDIR)/libos64/include
# -z noexecstack: bare-metal objects carry no .note.GNU-stack section (that
# annotation is a Linux userspace convention), and without the flag ld warns
# about one arbitrary object per link — noise with no meaning here, since
# os64's loader, not ELF notes, decides page permissions.
LDFLAGS ?= -static -nostdlib -T $(CURDIR)/link/app.ld -z max-page-size=0x1000 -z noexecstack

BIN     := $(CURDIR)/bin
OBJ     := $(CURDIR)/obj

# libos64 objects + the launch stub link into every app.
RUNTIME_SRCS := start/launch.S libos64/io.c libos64/linereader.c libos64/proc.c libos64/fmt.c libos64/args.c libos64/mem.c libos64/env.c libos64/str.c libos64/date.c libos64/klog.c libos64/thread.c libos64/net.c
RUNTIME_OBJS := $(patsubst %,$(OBJ)/%.o,$(RUNTIME_SRCS))

# more and less are two policies over one file-pager engine.  Keeping that
# engine outside either app directory prevents the first useful slices from
# quietly becoming two implementations while TTY-backed stdin waits its turn.
PAGER_APPS := more less
PAGER_SRC  := common/file_pager.c
PAGER_OBJ  := $(OBJ)/$(PAGER_SRC).o

# Every apps/<name>/ directory holding .c files is an app named <name>.
# $(sort) both orders and DEDUPES — essential since apps went multi-file
# (top was the first: top.c + topMain.c): the wildcard yields the directory
# once PER .c FILE, so without sort a two-file app appears twice in APPS
# and its link rule gets eval'd twice.
APPS := $(sort $(notdir $(patsubst %/,%,$(dir $(wildcard apps/*/*.c)))))

# EVERY APP LINKS AT ITS OWN BASE ADDRESS.
#
# They all used to link at 0x400000 — which was fine while only one program ran
# at a time, and broke the instant pipelines arrived: `hello | upper` puts two
# programs at the SAME linear addresses, and GDB (one global symbol table, keyed
# by address) then cannot tell them apart. Unique bases mean GDB can hold every
# app's symbols at once and a breakpoint lands in exactly one program.
#
# app_bases.py derives each base from a hash of the app's NAME (not build order,
# not a hand-maintained table), so an app's address is stable across rebuilds and
# adding a new app doesn't shove the others around — and "drop a directory and it
# builds" still holds. See that file for the full rationale and the window map.
APP_BASE_PAIRS := $(shell $(CURDIR)/tools/app_bases.py $(APPS))
$(foreach p,$(APP_BASE_PAIRS),\
  $(eval APP_BASE_$(word 1,$(subst =, ,$(p))) := $(word 2,$(subst =, ,$(p)))))

.PHONY: all
all: $(addprefix $(BIN)/,$(APPS)) $(BIN)/app_bases.gdb

# Per-app link rule, generated per app: the app name appears in BOTH the
# directory and the filename, and a make pattern prerequisite allows only one
# `%` stem — so a single `$(BIN)/%: $(OBJ)/apps/%/%.c.o` rule can't express it.
# eval writes an explicit rule for each app instead (also the natural place to
# add per-app extras later).
# NOTE the argument order: --defsym MUST come BEFORE -T (which lives in LDFLAGS).
# ld processes options left to right, and the script's DEFINED(APP_BASE) test can
# only see symbols defined by the time the script is read — a --defsym placed
# after -T is silently ignored and every app quietly links at the 0x400000
# fallback again (which builds, runs, and fails only in the debugger — the worst
# kind of bug). Verified: defsym-after-T -> 0x400000, defsym-before-T -> 0xe400000.
# MULTI-FILE APPS (since top, 2026-07-27): EVERY .c in apps/<name>/ compiles
# and links into <name> — before this, the rule named exactly <name>.c.o, so
# a second source file wasn't "compiled but not linked", it was never even
# compiled ("undefined reference to `topMain'" was the symptom). The wildcard
# expands at eval time, so dropping another .c into the folder is picked up
# by the next make with no registration step — the same drop-it-and-it-builds
# contract the app DIRECTORIES follow, now honored one level down.
#
# BASE ADDRESS IS A REAL BUILD INPUT. A newly added app can hash into an
# occupied slot, making the allocator linear-probe one of the existing apps
# to a new base. Make cannot see a changed command-line variable by itself:
# without an explicit prerequisite it considers that existing binary current
# and leaves two ELFs linked at the old address. Each app therefore depends on
# a stamp whose NAME contains its assigned base. If probing changes the base,
# the required filename changes, its new stamp is created, and only that app
# relinks with the new --defsym. Old stamps are harmless historical crumbs and
# disappear with `make clean` alongside the rest of OBJ.
define APP_RULE
APP_OBJS_$(1) := $(patsubst %.c,$(OBJ)/%.c.o,$(wildcard apps/$(1)/*.c))
APP_EXTRA_OBJS_$(1) := $(if $(filter $(1),$(PAGER_APPS)),$(PAGER_OBJ))
APP_BASE_STAMP_$(1) := $(OBJ)/apps/$(1)/.base-$(APP_BASE_$(1))
$$(APP_BASE_STAMP_$(1)):
	@mkdir -p "$$(dir $$@)"
	@touch "$$@"
$(BIN)/$(1): $$(APP_OBJS_$(1)) $$(APP_EXTRA_OBJS_$(1)) $(RUNTIME_OBJS) $$(APP_BASE_STAMP_$(1))
	@mkdir -p "$(BIN)"
	$(LD) --defsym APP_BASE=$(APP_BASE_$(1)) $(LDFLAGS) -o $$@ $$(APP_OBJS_$(1)) $$(APP_EXTRA_OBJS_$(1)) $(RUNTIME_OBJS)
	@printf '  %-10s base %s\n' "$(1)" "$(APP_BASE_$(1))"
endef
$(foreach app,$(APPS),$(eval $(call APP_RULE,$(app))))

# The GDB symbol map: one add-symbol-file per app, at its own base. Sourced by
# ./.gdbinit, so EVERY app's symbols are loaded before anything runs — symbolic
# breakpoints (`hbreak upper.c:47`) resolve immediately instead of going pending,
# and they resolve UNAMBIGUOUSLY because no two apps share an address anymore.
$(BIN)/app_bases.gdb: $(addprefix $(BIN)/,$(APPS))
	@mkdir -p "$(BIN)"
	@printf '# GENERATED by userland/GNUmakefile — do not edit.\n' > $@
	@printf '# Every app at its own base, so GDB can hold all their symbols at once.\n' >> $@
	@$(foreach app,$(APPS),\
	   printf 'add-symbol-file %s/%s %s\n' "$(BIN)" "$(app)" "$(APP_BASE_$(app))" >> $@;)
	@printf '# (symbols for %s)\n' "$(APPS)" >> $@

# Every .d gcc emitted for the runtime and each app — self-maintaining: each
# compile regenerates its .d with the current include list, so it cannot drift.
# Enumerated per .c FILE (not per app) since apps went multi-file: the old
# per-app form covered only <name>.c.d, which would have left topMain.c.o
# stale against topMain.h edits — the EXACT bug class this makefile's -MMD
# comment block memorializes, sneaking back through a side door.
HEADER_DEPS := $(RUNTIME_OBJS:.o=.d) \
               $(patsubst %.c,$(OBJ)/%.c.d,$(wildcard apps/*/*.c)) \
               $(PAGER_OBJ:.o=.d)
-include $(HEADER_DEPS)

$(OBJ)/%.c.o: %.c
	@mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/%.S.o: %.S
	@mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -rf $(OBJ) $(BIN)
