# userland/GNUmakefile — builds the whole os64 userland: the launch stub,
# libos64, and every app under apps/. One makefile above them all (per the
# roadmap — build scale is a non-issue on the dev machines).
#
# Output: each app links to userland/bin/<app> as a dynamically-linked ring-3
# ELF (launch + the app, against /lib/libos64.so), ready for the disk image.

# THE CROSS TOOLCHAIN, AND THE TWO-FACED `?=` (fixed 2026-08-22).
#
# These two lines used to read `CC ?= x86_64-elf-gcc` / `LD ?= x86_64-elf-ld`,
# and whether that worked depended on WHICH DIRECTORY YOU TYPED MAKE IN:
#
#   make            (from the repo root)  -> cross toolchain, correct
#   make -C userland (here, directly)     -> HOST cc and HOST ld, silently
#
# Because `?=` assigns only to an UNDEFINED variable, and `CC`/`LD` are two of
# make's BUILT-IN variables (`cc`, `ld`) — defined, so `?=` skips them. What
# saved the root build is line 2 of the root GNUmakefile: `MAKEFLAGS += -rR`,
# where -R is --no-builtin-variables. That propagates into this sub-make and
# leaves CC/LD genuinely undefined, so `?=` fired and the cross tools won.
# Build straight from this directory, with no -rR in play, and the built-in
# defaults are back and `?=` goes quiet. Same tree, same makefile, different
# compiler — and no diagnostic either way.
#
# It matters because the two toolchains have different DEFAULTS. Host ld emits
# GNU-style hash tables; bare-metal binutils emits sysv — and os64's loader
# needs sysv, because ELF has no DT_SYMTABSZ and the kernel takes the dynamic
# symbol count from DT_HASH's nchain word. That one is pinned explicitly on
# both link lines now, but the next default to differ would have been found
# the hard way.
#
# So: assign when the variable is make's own (built-in default OR undefined),
# and never when a human meant something — an explicit `make CC=clang`, an
# environment variable, or a value passed down by a parent make all still win.
# (`ifeq ($(origin CC),default)` alone is NOT enough, and getting that wrong
# breaks the root build instead: under -rR the origin is `undefined`.)
ifeq ($(filter-out default undefined,$(origin CC)),)
CC := x86_64-elf-gcc
endif
ifeq ($(filter-out default undefined,$(origin LD)),)
LD := x86_64-elf-ld
endif

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
# NOT -static any more (2026-08-22): an app links against libos64.so, so it
# carries a PT_DYNAMIC with a DT_NEEDED and the kernel's loader resolves it.
# It is still ET_EXEC and still non-PIE — a fixed-base executable with dynamic
# dependencies, which is exactly what every non-PIE program on Unix was for
# twenty years before PIE became the default. No --dynamic-linker: there is no
# ld.so in os64 and no PT_INTERP; the KERNEL is the dynamic linker, so naming
# a userspace interpreter would be naming something that does not exist.
# --no-as-needed: record the DT_NEEDED unconditionally rather than depending
# on the linker's opinion of whether the library was referenced.
# --hash-style=sysv here for the same reason as the library (see
# LIBOS64_LDFLAGS): the kernel takes the dynamic symbol COUNT from DT_HASH's
# nchain word, and an app's own .dynsym is sized exactly the same way. The
# host toolchain this tree actually builds with defaults to gnu-only hashing,
# so this is load-bearing, not belt-and-braces.
# -z separate-code: keep CODE in a segment of its own, exactly as the library
# link does. app.ld's ALIGNs are necessary but not sufficient — ld will happily
# merge the read-only zone (dynamic tables, .rodata) into the executable one
# and emit a single R+E LOAD, and since os64 derives page protections straight
# from segment flags that would make every constant in every app executable,
# quietly undoing the NX arc. It became necessary the moment .plt got its own
# section (which the debugger needs); before that ld happened to split them.
# "Happened to" is not a guarantee, hence the flag.
LDFLAGS ?= -nostdlib --no-as-needed --hash-style=sysv -T $(CURDIR)/link/app.ld -z max-page-size=0x1000 -z noexecstack -z separate-code

BIN     := $(CURDIR)/bin
OBJ     := $(CURDIR)/obj

# ── THE LIBRARY AND THE STUB (split 2026-08-22, when libos64 became a .so) ──
#
# Until today every one of these objects was linked into every single app:
# 64 private copies of the same ~81KB of library code, on disk and again in
# RAM for every running program. libos64 is now built ONCE as a real shared
# object, mapped at one fixed address, and the kernel's per-page cache
# (kernel/src/shared_object.c) hands every task the same physical pages.
#
# What is left in each app is `launch` — the entry stub. It stays statically
# linked and it has to: it IS the ELF entry point, it runs before any library
# is standing, and it is twenty instructions. Its one call into the library
# (os64_runtime_init) goes through the app's PLT like any other.
LAUNCH_SRCS := start/launch.S
LAUNCH_OBJS := $(patsubst %,$(OBJ)/%.o,$(LAUNCH_SRCS))

# The library's own sources. thread_trampoline.S moved here out of launch.S
# the same day: a shared object's relocations resolve against itself and its
# own dependencies (never against the executable that loaded it), so the
# library has to carry every symbol the library uses.
LIBOS64_SRCS := libos64/init.c libos64/io.c libos64/linereader.c libos64/proc.c libos64/procfs.c libos64/fmt.c libos64/args.c libos64/mem.c libos64/heap.c libos64/env.c libos64/str.c libos64/date.c libos64/klog.c libos64/thread.c libos64/net.c libos64/crc32.c libos64/draw.c libos64/ui.c libos64/ui_text.c libos64/clip.c libos64/conf.c libos64/resolve.c libos64/thread_trampoline.S
# PIC objects live under obj/pic/ — a separate tree, because these are
# compiled with different flags than everything else and an object built for
# one world must never be picked up by the other.
LIBOS64_OBJS := $(patsubst %,$(OBJ)/pic/%.o,$(LIBOS64_SRCS))
LIBOS64_SO   := $(BIN)/libos64.so

# The library is position-independent; the apps are not. That asymmetry is the
# whole design (see kernel/include/shared_object.h): apps keep their fixed
# per-app link bases — which is what lets a host debugger tell two running
# programs apart — while the LIBRARY, being PIC, can be placed anywhere and is
# therefore free to live outside the crowded low 2GB entirely.
LIBOS64_CFLAGS := $(filter-out -fno-pic -fno-pie,$(CFLAGS)) -fPIC

# --hash-style=sysv: the kernel derives the dynamic symbol COUNT from
#   DT_HASH's nchain word (there is no DT_SYMTABSZ in ELF), so a GNU-only
#   hash table would leave it unable to size the symbol table at all. Pinned
#   rather than trusted: bare-metal binutils defaults to sysv today, hosted
#   toolchains default to gnu, and the failure mode is a relocation error
#   nowhere near the cause.
# -Bsymbolic-functions: bind the library's calls to its OWN functions
#   directly instead of through the PLT. This is exactly what libChrisOS
#   achieved by hand in the 32-bit OS by writing every function twice —
#   `strcmpI` (internal) and `strcmp` (exported) — so the library's own use
#   of strcmp could never be hijacked by an app that defined its own, and
#   never went near the lazy-binding resolver. One linker flag now buys the
#   same property with no duplicated source (LIBOS64.md's hidden-alias plan,
#   arrived at from the other direction).
# -z separate-code: keep CODE in a segment of its own. Without it ld happily
# merges the read-only zone (symbol tables, relocations, .rodata) into the
# executable one and hands back a single R+E LOAD — which, since os64 derives
# page protections straight from segment flags, would make every constant in
# the library executable and undo the NX arc for the one object every program
# maps. lib.ld's ALIGNs are necessary but NOT sufficient; the flag is what
# makes ld honour them as segment boundaries.
LIBOS64_LDFLAGS := -shared -soname libos64.so --hash-style=sysv -Bsymbolic-functions \
                   -z max-page-size=0x1000 -z noexecstack -z separate-code \
                   -T $(CURDIR)/link/lib.ld

# Each library's PRELINKED base, from the same assigner that places the apps
# (--libs switches it to the shared-library window). Libraries are placed at
# BUILD time, not by the kernel's bump allocator, so the address is a stable
# function of the library's name — which is what lets the GDB map below name
# it and `step` into a library call actually step in. See link/lib.ld.
LIBOS64_BASE   := $(shell $(CURDIR)/tools/app_bases.py --libs libos64.so | cut -d= -f2)

# more and less are two policies over one file-pager engine.  Keeping that
# engine outside either app directory prevents the first useful slices from
# quietly becoming two implementations while TTY-backed stdin waits its turn.
PAGER_APPS := more less
PAGER_SRC  := common/file_pager.c
PAGER_OBJ  := $(OBJ)/$(PAGER_SRC).o

# The hex editor's window/cache/undo engine is presentation-independent. A
# future GUI links this same object and supplies pixels where hexedit supplies
# terminal rows.
HEX_BUFFER_APPS := hexedit
HEX_BUFFER_SRC  := common/hex_buffer.c
HEX_BUFFER_OBJ  := $(OBJ)/$(HEX_BUFFER_SRC).o

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
# The slot size comes from the same script that assigns the bases, and is
# handed to ld so app.ld can ASSERT the app actually fits in its slot. One
# definition of the number; see app.ld's slot-check comment.
APP_SLOT_SIZE  := $(shell $(CURDIR)/tools/app_bases.py --slot-size)
$(foreach p,$(APP_BASE_PAIRS),\
  $(eval APP_BASE_$(word 1,$(subst =, ,$(p))) := $(word 2,$(subst =, ,$(p)))))

.PHONY: all
all: $(LIBOS64_SO) $(addprefix $(BIN)/,$(APPS)) $(BIN)/app_bases.gdb

# The shared library itself. ET_DYN, no fixed address — the kernel's
# shared-object registry places it in the TASK_SHLIB_VIRT_BASE window and
# every task maps it there.
# --defsym BEFORE -T (which lives in LIBOS64_LDFLAGS): ld reads options left
# to right and lib.ld's DEFINED(LIB_BASE) test can only see symbols defined by
# the time the script is read. The same trap app.ld's comment memorializes.
$(LIBOS64_SO): $(LIBOS64_OBJS) $(CURDIR)/link/lib.ld
	@mkdir -p "$(BIN)"
	$(LD) --defsym LIB_BASE=$(LIBOS64_BASE) $(LIBOS64_LDFLAGS) -o $@ $(LIBOS64_OBJS)
	@printf '  %-10s prelinked at %s\n' "libos64.so" "$(LIBOS64_BASE)"

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
APP_EXTRA_OBJS_$(1) := $(if $(filter $(1),$(PAGER_APPS)),$(PAGER_OBJ)) \
                       $(if $(filter $(1),$(HEX_BUFFER_APPS)),$(HEX_BUFFER_OBJ))
APP_BASE_STAMP_$(1) := $(OBJ)/apps/$(1)/.base-$(APP_BASE_$(1))
$$(APP_BASE_STAMP_$(1)):
	@mkdir -p "$$(dir $$@)"
	@touch "$$@"
$(BIN)/$(1): $$(APP_OBJS_$(1)) $$(APP_EXTRA_OBJS_$(1)) $(LAUNCH_OBJS) $(LIBOS64_SO) $$(APP_BASE_STAMP_$(1))
	@mkdir -p "$(BIN)"
	$(LD) --defsym APP_BASE=$(APP_BASE_$(1)) --defsym APP_SLOT_SIZE=$(APP_SLOT_SIZE) $(LDFLAGS) -o $$@ $$(APP_OBJS_$(1)) $$(APP_EXTRA_OBJS_$(1)) $(LAUNCH_OBJS) $(LIBOS64_SO)
	@printf '  %-10s base %s\n' "$(1)" "$(APP_BASE_$(1))"
endef
$(foreach app,$(APPS),$(eval $(call APP_RULE,$(app))))

# The GDB symbol map: one add-symbol-file per app, at its own base. Sourced by
# ./.gdbinit, so EVERY app's symbols are loaded before anything runs — symbolic
# breakpoints (`hbreak upper.c:47`) resolve immediately instead of going pending,
# and they resolve UNAMBIGUOUSLY because no two apps share an address anymore.
$(BIN)/app_bases.gdb: $(addprefix $(BIN)/,$(APPS)) $(LIBOS64_SO)
	@mkdir -p "$(BIN)"
	@printf '# GENERATED by userland/GNUmakefile — do not edit.\n' > $@
	@printf '# Every app at its own base, so GDB can hold all their symbols at once.\n' >> $@
	@$(foreach app,$(APPS),\
	   printf 'add-symbol-file %s/%s %s\n' "$(BIN)" "$(app)" "$(APP_BASE_$(app))" >> $@;)
	@printf '\n' >> $@
	@printf '# THE SHARED LIBRARY (2026-08-22). Without this line GDB has no line\n' >> $@
	@printf '# info anywhere in libos64, so `step` at a library call silently behaves\n' >> $@
	@printf '# like `next` — it cannot step into code it has no symbols for. This is\n' >> $@
	@printf '# also why the library is PRELINKED at a build-time address (link/lib.ld):\n' >> $@
	@printf '# when the kernel chose the address by load order, the build had nothing\n' >> $@
	@printf '# truthful to write here.\n' >> $@
	@printf '# The address is .text, which is what add-symbol-file wants — NOT the\n' >> $@
	@printf '# image base, since lib.ld puts the read-only zone first.\n' >> $@
	@printf 'add-symbol-file %s 0x%s\n' "$(LIBOS64_SO)" \
	   "$$(x86_64-elf-readelf -SW $(LIBOS64_SO) | grep -E '\.text +PROGBITS' | grep -oE '[0-9a-f]{16}' | head -1)" >> $@
	@printf '# (symbols for %s + libos64.so)\n' "$(APPS)" >> $@

# Every .d gcc emitted for the runtime and each app — self-maintaining: each
# compile regenerates its .d with the current include list, so it cannot drift.
# Enumerated per .c FILE (not per app) since apps went multi-file: the old
# per-app form covered only <name>.c.d, which would have left topMain.c.o
# stale against topMain.h edits — the EXACT bug class this makefile's -MMD
# comment block memorializes, sneaking back through a side door.
HEADER_DEPS := $(LAUNCH_OBJS:.o=.d) $(LIBOS64_OBJS:.o=.d) \
               $(patsubst %.c,$(OBJ)/%.c.d,$(wildcard apps/*/*.c)) \
               $(PAGER_OBJ:.o=.d) $(HEX_BUFFER_OBJ:.o=.d)
-include $(HEADER_DEPS)

# The PIC rules come FIRST: make picks the pattern rule with the shortest
# stem, and obj/pic/%.c.o would otherwise also match the plain obj/%.c.o rule
# (with a stem of "pic/...") and get compiled without -fPIC. Ordering alone
# does not decide that — the stems differ in length, and the more specific
# prefix wins — but keeping them adjacent and commented means nobody has to
# re-derive it.
$(OBJ)/pic/%.c.o: %.c
	@mkdir -p "$(dir $@)"
	$(CC) $(LIBOS64_CFLAGS) -c $< -o $@

$(OBJ)/pic/%.S.o: %.S
	@mkdir -p "$(dir $@)"
	$(CC) $(LIBOS64_CFLAGS) -c $< -o $@

$(OBJ)/%.c.o: %.c
	@mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/%.S.o: %.S
	@mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -rf $(OBJ) $(BIN)
