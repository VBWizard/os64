# userland/GNUmakefile — builds the whole os64 userland: the launch stub,
# libos64, every app under apps/, and every fixture under tests/. One makefile
# above them all (per the roadmap — build scale is a non-issue on the dev
# machines).
#
# Output: each program links to userland/bin/<app> or userland/bin/tests/<name>
# as a dynamically-linked ring-3 ELF (launch + the program, against
# /lib/libos64.so), ready for the disk image's /bin and /tests.

# Parallel when this is the top-level make (`make -C userland`); under the
# root build it inherits the root's jobserver instead — a -j of its own
# would drop that jobserver and run a full count beside the root's jobs.
ifeq ($(MAKELEVEL),0)
MAKEFLAGS += -j$(shell nproc)
endif

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
#   -msse2            : baseline x87/SSE state is saved per thread by the kernel
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
CFLAGS  ?= -g -m64 -ffreestanding -fno-pic -fno-pie -mno-red-zone -msse2 \
           -fno-stack-protector -fcf-protection=none \
           -nostdlib -nostartfiles -nodefaultlibs -masm=intel -Wall -Wextra \
           $(WERROR_FLAG) -MMD -MP \
           -I$(CURDIR)/../abi/include -I$(CURDIR)/libos64/include \
           -I$(CURDIR)/libgzip/include -I$(CURDIR)/libpng/include
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
# -rpath-link names the BUILD-TIME directory where ld can verify transitive
# shared dependencies. An app that needs libpng needs libgzip transitively;
# without this hint ld warns that libgzip cannot be found and then treats
# libpng's inflate calls as unresolved. This does not
# write an RPATH into the ELF — os64's runtime loader follows DT_NEEDED names
# in /lib, independent of the host build tree.
# -z separate-code: keep CODE in a segment of its own, exactly as the library
# link does. app.ld's ALIGNs are necessary but not sufficient — ld will happily
# merge the read-only zone (dynamic tables, .rodata) into the executable one
# and emit a single R+E LOAD, and since os64 derives page protections straight
# from segment flags that would make every constant in every app executable,
# quietly undoing the NX arc. It became necessary the moment .plt got its own
# section (which the debugger needs); before that ld happened to split them.
# "Happened to" is not a guarantee, hence the flag.
LDFLAGS ?= -nostdlib --no-as-needed --hash-style=sysv -rpath-link $(BIN) -T $(CURDIR)/link/app.ld -z max-page-size=0x1000 -z noexecstack -z separate-code

BIN     := $(CURDIR)/bin
OBJ     := $(CURDIR)/obj
# The fixtures land in their own output directory because they land in their
# own directory ON THE IMAGE (/tests, not /bin). Keeping the two apart here is
# what lets the image rules route them without a hand-maintained list of which
# names are tests — the SOURCE tree is the answer, and it cannot drift.
TESTBIN := $(BIN)/tests

# ── SHARED LIBRARIES AND THE STUB ──────────────────────────────────────────
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

# libos64's own sources. thread_trampoline.S moved here out of launch.S
# the same day: a shared object's relocations resolve against itself and its
# own dependencies (never against the executable that loaded it), so the
# library has to carry every symbol the library uses.
LIBOS64_SRCS := libos64/init.c libos64/io.c libos64/linereader.c libos64/proc.c libos64/procfs.c libos64/fmt.c libos64/args.c libos64/mem.c libos64/heap.c libos64/env.c libos64/str.c libos64/date.c libos64/klog.c libos64/thread.c libos64/net.c libos64/crc32.c libos64/draw.c libos64/ui.c libos64/ui_text.c libos64/clip.c libos64/conf.c libos64/resolve.c libos64/slurp.c libos64/image.c libos64/menu.c libos64/thread_trampoline.S
# PIC objects live under obj/pic/ — a separate tree, because these are
# compiled with different flags than everything else and an object built for
# one world must never be picked up by the other.
LIBOS64_OBJS := $(patsubst %,$(OBJ)/pic/%.o,$(LIBOS64_SRCS))
LIBOS64_SO   := $(BIN)/libos64.so

# libgzip is a separate shared module: the raw DEFLATE engine serves gzip,
# libpng's zlib framing, and HTTP content decoding without becoming part of
# libos64's foundational C surface. Its gzip framing reuses libos64's
# established CRC32, heap, and memory primitives, so the object records a real
# DT_NEEDED on libos64.so instead of carrying private copies of them.
LIBGZIP_SRCS := libgzip/inflate.c libgzip/deflate.c libgzip/gzip.c
LIBGZIP_OBJS := $(patsubst %,$(OBJ)/pic/%.o,$(LIBGZIP_SRCS))
LIBGZIP_SO   := $(BIN)/libgzip.so

# libpng owns PNG chunks, zlib framing, filters and color conversion. It uses
# libgzip's raw DEFLATE door and libos64's heap/CRC primitives, but neither of
# those lower libraries knows an image format exists. That direction keeps a
# decoder out of every program's foundational runtime.
LIBPNG_SRCS := libpng/png.c
LIBPNG_OBJS := $(patsubst %,$(OBJ)/pic/%.o,$(LIBPNG_SRCS))
LIBPNG_SO   := $(BIN)/libpng.so

# Shared-library code is position-independent; the apps are not. That is the
# whole design (see kernel/include/shared_object.h): apps keep their fixed
# per-app link bases — which is what lets a host debugger tell two running
# programs apart — while a PIC library can live outside the crowded low 2GB.
LIBOS64_CFLAGS := $(filter-out -fno-pic -fno-pie,$(CFLAGS)) -fPIC
LIBGZIP_CFLAGS := $(LIBOS64_CFLAGS)
LIBPNG_CFLAGS  := $(LIBOS64_CFLAGS)

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
# page protections straight from segment flags, would make the library's
# constants executable. lib.ld's ALIGNs are necessary but NOT sufficient; the
# flag is what makes ld honour them as segment boundaries.
SHARED_LIB_LDFLAGS := -shared --hash-style=sysv -Bsymbolic-functions \
                      -z max-page-size=0x1000 -z noexecstack -z separate-code \
                      -T $(CURDIR)/link/lib.ld
LIBOS64_LDFLAGS := $(SHARED_LIB_LDFLAGS) -soname libos64.so
LIBGZIP_LDFLAGS := $(SHARED_LIB_LDFLAGS) -soname libgzip.so
LIBPNG_LDFLAGS  := $(SHARED_LIB_LDFLAGS) -soname libpng.so

# Each library's PRELINKED base, from the same assigner that places the apps
# (--libs switches it to the shared-library window). Libraries are placed at
# BUILD time, not by the kernel's bump allocator, so the address is a stable
# function of the library's name — which is what lets the GDB map below name
# it and `step` into a library call actually step in. See link/lib.ld.
# Library bases must be assigned as one population: calculating each name in
# isolation would miss a hash collision and prelink two objects into the same
# slot. app_bases.py sorts before probing, so adding a library gives the build
# one deterministic map independent of link order.
LIB_BASE_PAIRS := $(shell $(CURDIR)/tools/app_bases.py --libs libos64.so libgzip.so libpng.so)
LIB_SLOT_SIZE  := $(shell $(CURDIR)/tools/app_bases.py --lib-slot-size)
LIBOS64_BASE   := $(patsubst libos64.so=%,%,$(filter libos64.so=%,$(LIB_BASE_PAIRS)))
LIBGZIP_BASE   := $(patsubst libgzip.so=%,%,$(filter libgzip.so=%,$(LIB_BASE_PAIRS)))
LIBPNG_BASE    := $(patsubst libpng.so=%,%,$(filter libpng.so=%,$(LIB_BASE_PAIRS)))

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

# tar's header codec is pure byte work. The app uses it to handle archives;
# tartest uses the same object to construct malformed inputs at the format
# boundary instead of carrying a private almost-tar encoder in the fixture.
TAR_FORMAT_PROGRAMS := tartest
TAR_FORMAT_OBJ      := $(OBJ)/apps/tar/tar_format.c.o

# These programs consume the standalone gzip module. Keep the dependency
# selective: linking libgzip into every app would turn an optional format
# engine into part of the foundational runtime by accident.
LIBGZIP_PROGRAMS := gzip gunzip gziptest os64get

# gview consumes PNG in the GUI; pngtest proves the same dependency in the
# guest without requiring a GUI. libpng carries its own DT_NEEDED edge on
# libgzip, so consumers name the codec they asked for rather than its engine.
LIBPNG_PROGRAMS := gview pngtest

# Every apps/<name>/ directory holding .c files is an app named <name>.
# $(sort) both orders and DEDUPES — essential since apps went multi-file
# (top was the first: top.c + topMain.c): the wildcard yields the directory
# once PER .c FILE, so without sort a two-file app appears twice in APPS
# and its link rule gets eval'd twice.
APPS := $(sort $(notdir $(patsubst %/,%,$(dir $(wildcard apps/*/*.c)))))

# And every tests/<name>/ directory is a FIXTURE named <name>, built exactly
# like an app and installed to /tests instead of /bin.
#
# The split is curatorial, not technical: /bin is the programs and tools a
# person runs, /tests is the proof harness — the badge-code fixtures testrun
# spawns, and the acceptance probes you drive by hand when a feature is on
# trial. Two directories also make a NAME COLLISION IMPOSSIBLE TO MISS, which
# is why the split was worth doing on its own: `hog` existed twice, once here
# and once under kernel/test/elf, both aimed at /bin/hog, and the image kept
# the one this tree built. So test_backstop_preemption — whose entire premise
# is "a thread that never enters the kernel" — was spawning the measuring
# instrument, which calls os64_ticks() as it spins.
TESTS := $(sort $(notdir $(patsubst %/,%,$(dir $(wildcard tests/*/*.c)))))

# AND THE TRIPWIRE, because this slice exists to replace vigilance with a
# mechanism. Every per-program make variable below (APP_OBJS_<name>,
# APP_BASE_<name>, the base stamp) is keyed on the NAME alone, so one name in
# both trees would have the second eval quietly overwrite the first and build
# one of them from the other's sources. The two directories cannot collide on
# the IMAGE any more; this is what stops them colliding in the BUILD.
APP_TEST_NAME_CLASH := $(filter $(APPS),$(TESTS))
ifneq ($(APP_TEST_NAME_CLASH),)
$(error name used by both an app and a fixture: $(APP_TEST_NAME_CLASH) — \
        apps/ and tests/ share one link-base pool and one set of make \
        variables, so a name must belong to exactly one of them)
endif

# `[` is the traditional second name for test, not a second program. Keep one
# linked image and copy it under the punctuation name so both entry points have
# identical expression semantics and fixes cannot drift between twins.
APP_ALIASES := [

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
# FIXTURES DRAW FROM THE SAME POOL AS APPS, and must: a fixture is an ordinary
# non-PIE ET_EXEC, so two programs sharing a base is the same debugger ambiguity
# whichever directory they came from — and testrun runs a fixture right next to
# the app it is testing. Because app_bases.py keys on the NAME and sorts before
# probing, feeding it both lists yields exactly the map it produced when they
# were one list: moving a program between apps/ and tests/ does not move it in
# memory.
APP_BASE_PAIRS := $(shell $(CURDIR)/tools/app_bases.py $(APPS) $(TESTS))
# The slot size comes from the same script that assigns the bases, and is
# handed to ld so app.ld can ASSERT the app actually fits in its slot. One
# definition of the number; see app.ld's slot-check comment.
APP_SLOT_SIZE  := $(shell $(CURDIR)/tools/app_bases.py --slot-size)
$(foreach p,$(APP_BASE_PAIRS),\
  $(eval APP_BASE_$(word 1,$(subst =, ,$(p))) := $(word 2,$(subst =, ,$(p)))))

.PHONY: all
all: $(LIBOS64_SO) $(LIBGZIP_SO) $(LIBPNG_SO) $(addprefix $(BIN)/,$(APPS)) $(addprefix $(TESTBIN)/,$(TESTS)) \
     $(addprefix $(BIN)/,$(APP_ALIASES)) $(BIN)/app_bases.gdb

$(BIN)/[: $(BIN)/test
	cp $< $@

# The shared library itself. ET_DYN, PRELINKED: app_bases.py --libs picks its
# slot in the kernel's prelink region (the first 4GB of the shared window)
# and lib.ld links it there, so the address is the build's to know and the
# debugger's to use. The kernel honours it (load_bias 0) rather than choosing
# one — the bump allocator in shared_object.c serves only images that arrive
# WITHOUT a base. (This comment said the opposite until 2026-08-23, in the
# same commit that made it false.)
# Both --defsym values precede -T (which lives in LIBOS64_LDFLAGS): ld reads
# options left to right, and lib.ld must see the assigned base and the slot
# bound before it evaluates either one. LIB_SLOT_SIZE comes from the assigner
# that spaces the bases, so its ASSERT checks against the build's actual map.
$(LIBOS64_SO): $(LIBOS64_OBJS) $(CURDIR)/link/lib.ld $(CURDIR)/tools/app_bases.py
	@mkdir -p "$(BIN)"
	$(LD) --defsym LIB_BASE=$(LIBOS64_BASE) \
	      --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) $(LIBOS64_LDFLAGS) \
	      -o $@ $(LIBOS64_OBJS)
	@printf '  %-10s prelinked at %s\n' "libos64.so" "$(LIBOS64_BASE)"

# --no-as-needed makes the dependency an explicit property of libgzip.so.
# The kernel recursively loads DT_NEEDED objects and resolves this library's
# libos64 references within that dependency closure.
$(LIBGZIP_SO): $(LIBGZIP_OBJS) $(LIBOS64_SO) $(CURDIR)/link/lib.ld $(CURDIR)/tools/app_bases.py
	@mkdir -p "$(BIN)"
	$(LD) --defsym LIB_BASE=$(LIBGZIP_BASE) \
	      --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) $(LIBGZIP_LDFLAGS) --no-as-needed \
	      -o $@ $(LIBGZIP_OBJS) $(LIBOS64_SO)
	@printf '  %-10s prelinked at %s\n' "libgzip.so" "$(LIBGZIP_BASE)"

# libpng calls both dependencies directly: libgzip for raw inflate and
# libos64 for allocation and PNG chunk CRCs. --no-as-needed records both edges
# even if a future optimizer changes which calls remain visible to the linker.
$(LIBPNG_SO): $(LIBPNG_OBJS) $(LIBGZIP_SO) $(LIBOS64_SO) $(CURDIR)/link/lib.ld $(CURDIR)/tools/app_bases.py
	@mkdir -p "$(BIN)"
	$(LD) --defsym LIB_BASE=$(LIBPNG_BASE) \
	      --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) $(LIBPNG_LDFLAGS) --no-as-needed \
	      -o $@ $(LIBPNG_OBJS) $(LIBGZIP_SO) $(LIBOS64_SO)
	@printf '  %-10s prelinked at %s\n' "libpng.so" "$(LIBPNG_BASE)"

# The link rule, generated once per program — apps and fixtures alike: the
# name appears in BOTH the directory and the filename, and a make pattern
# prerequisite allows only one `%` stem, so a single
# `$(BIN)/%: $(OBJ)/apps/%/%.c.o` rule can't express it. eval writes an
# explicit rule for each instead (also the natural place to add per-app
# extras later).
# NOTE the argument order: --defsym MUST come BEFORE -T (which lives in LDFLAGS).
# ld processes options left to right, and the script's DEFINED(APP_BASE) test can
# only see symbols defined by the time the script is read — a --defsym placed
# after -T is silently ignored and every app quietly links at the 0x400000
# fallback again (which builds, runs, and fails only in the debugger — the worst
# kind of bug). Verified: defsym-after-T -> 0x400000, defsym-before-T -> 0xe400000.
# MULTI-FILE APPS (since top, 2026-07-27): EVERY .c in the program's directory
# compiles and links into it — before this, the rule named exactly <name>.c.o, so
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
#
# THE THREE ARGUMENTS are (name, source directory, output directory) — a
# fixture builds by exactly the same recipe as an app and differs only in
# where it comes from and where it goes, so it gets the same rule rather than
# a parallel copy that could drift.
define APP_RULE
APP_OBJS_$(1) := $(patsubst %.c,$(OBJ)/%.c.o,$(wildcard $(2)/$(1)/*.c))
APP_EXTRA_OBJS_$(1) := $(if $(filter $(1),$(PAGER_APPS)),$(PAGER_OBJ)) \
                       $(if $(filter $(1),$(HEX_BUFFER_APPS)),$(HEX_BUFFER_OBJ)) \
                       $(if $(filter $(1),$(TAR_FORMAT_PROGRAMS)),$(TAR_FORMAT_OBJ))
APP_EXTRA_LIBS_$(1) := $(if $(filter $(1),$(LIBGZIP_PROGRAMS)),$(LIBGZIP_SO)) \
                       $(if $(filter $(1),$(LIBPNG_PROGRAMS)),$(LIBPNG_SO))
APP_BASE_STAMP_$(1) := $(OBJ)/$(2)/$(1)/.base-$(APP_BASE_$(1))
$$(APP_BASE_STAMP_$(1)):
	@mkdir -p "$$(dir $$@)"
	@touch "$$@"
$(3)/$(1): $$(APP_OBJS_$(1)) $$(APP_EXTRA_OBJS_$(1)) $(LAUNCH_OBJS) $$(APP_EXTRA_LIBS_$(1)) $(LIBOS64_SO) $$(APP_BASE_STAMP_$(1))
	@mkdir -p "$(3)"
	$(LD) --defsym APP_BASE=$(APP_BASE_$(1)) --defsym APP_SLOT_SIZE=$(APP_SLOT_SIZE) $(LDFLAGS) -o $$@ $$(APP_OBJS_$(1)) $$(APP_EXTRA_OBJS_$(1)) $(LAUNCH_OBJS) $$(APP_EXTRA_LIBS_$(1)) $(LIBOS64_SO)
	@printf '  %-10s base %s\n' "$(1)" "$(APP_BASE_$(1))"
endef
$(foreach app,$(APPS),$(eval $(call APP_RULE,$(app),apps,$(BIN))))
$(foreach t,$(TESTS),$(eval $(call APP_RULE,$(t),tests,$(TESTBIN))))

# The GDB symbol map: one add-symbol-file per program, at its own base. Sourced
# by ./.gdbinit, so EVERY program's symbols are loaded before anything runs —
# symbolic breakpoints (`hbreak upper.c:47`) resolve immediately instead of going
# pending, and they resolve UNAMBIGUOUSLY because no two share an address.
# FIXTURES ARE IN HERE TOO, and that is the point of the line rather than an
# afterthought: a fixture is the program you are most likely to be single-
# stepping, because it is what you reach for when something is already broken.
$(BIN)/app_bases.gdb: $(addprefix $(BIN)/,$(APPS)) $(addprefix $(TESTBIN)/,$(TESTS)) $(LIBOS64_SO) $(LIBGZIP_SO) $(LIBPNG_SO)
	@mkdir -p "$(BIN)"
	@printf '# GENERATED by userland/GNUmakefile — do not edit.\n' > $@
	@printf '# Every program at its own base, so GDB can hold all their symbols at once.\n' >> $@
	@$(foreach app,$(APPS),\
	   printf 'add-symbol-file %s/%s %s\n' "$(BIN)" "$(app)" "$(APP_BASE_$(app))" >> $@;)
	@printf '\n# The /tests fixtures, from the same base pool.\n' >> $@
	@$(foreach t,$(TESTS),\
	   printf 'add-symbol-file %s/%s %s\n' "$(TESTBIN)" "$(t)" "$(APP_BASE_$(t))" >> $@;)
	@printf '\n' >> $@
	@printf '# THE SHARED LIBRARIES. Without these lines GDB has no line info in\n' >> $@
	@printf '# libos64, libgzip or libpng, so `step` at a library call silently behaves like\n' >> $@
	@printf '# `next` — it cannot step into code it has no symbols for. This is also\n' >> $@
	@printf '# why each library is PRELINKED at a build-time address (link/lib.ld):\n' >> $@
	@printf '# when the kernel chose the address by load order, the build had nothing\n' >> $@
	@printf '# truthful to write here.\n' >> $@
	@printf '# Their section addresses are already absolute in each prelinked ELF, so\n' >> $@
	@printf '# bare add-symbol-file reads the truth without duplicating it by regex.\n' >> $@
	@printf 'add-symbol-file %s\n' "$(LIBOS64_SO)" >> $@
	@printf 'add-symbol-file %s\n' "$(LIBGZIP_SO)" >> $@
	@printf 'add-symbol-file %s\n' "$(LIBPNG_SO)" >> $@
	@printf '# (symbols for %s + libos64.so + libgzip.so + libpng.so)\n' "$(APPS) $(TESTS)" >> $@

# Every .d gcc emitted for the runtime and each app — self-maintaining: each
# compile regenerates its .d with the current include list, so it cannot drift.
# Enumerated per .c FILE (not per app) since apps went multi-file: the old
# per-app form covered only <name>.c.d, which would have left topMain.c.o
# stale against topMain.h edits — the EXACT bug class this makefile's -MMD
# comment block memorializes, sneaking back through a side door.
HEADER_DEPS := $(LAUNCH_OBJS:.o=.d) $(LIBOS64_OBJS:.o=.d) $(LIBGZIP_OBJS:.o=.d) $(LIBPNG_OBJS:.o=.d) \
               $(patsubst %.c,$(OBJ)/%.c.d,$(wildcard apps/*/*.c tests/*/*.c)) \
               $(PAGER_OBJ:.o=.d) $(HEX_BUFFER_OBJ:.o=.d)
-include $(HEADER_DEPS)

# The PIC rules come FIRST: make picks the pattern rule with the shortest
# stem, and obj/pic/%.c.o would otherwise also match the plain obj/%.c.o rule
# (with a stem of "pic/...") and get compiled without -fPIC. Ordering alone
# does not decide that — the stems differ in length, and the more specific
# prefix wins — but keeping them adjacent and commented means nobody has to
# re-derive it.
$(OBJ)/pic/libgzip/%.c.o: libgzip/%.c
	@mkdir -p "$(dir $@)"
	$(CC) $(LIBGZIP_CFLAGS) -c $< -o $@

$(OBJ)/pic/libpng/%.c.o: libpng/%.c
	@mkdir -p "$(dir $@)"
	$(CC) $(LIBPNG_CFLAGS) -c $< -o $@

$(OBJ)/pic/%.c.o: %.c
	@mkdir -p "$(dir $@)"
	$(CC) $(LIBOS64_CFLAGS) -c $< -o $@

$(OBJ)/pic/%.S.o: %.S
	@mkdir -p "$(dir $@)"
	$(CC) $(LIBOS64_CFLAGS) -c $< -o $@

# Userland builds -O0 so a debugger sees every local — but a program whose
# whole job is arithmetic pays for that at retail: at -O0 the Mandelbrot
# inner loop spills four doubles to the stack every iteration. Apps named
# here compile at -O2 (a pattern-specific variable, so nothing else
# changes). Add one only when it has been MEASURED; a demo that feels
# slow is usually doing more math, not less optimizing.
# The source directory is DERIVED rather than spelled, because a name here is
# a pattern prefix: `$(OBJ)/apps/<name>/%.c.o` matches nothing at all once that
# program lives under tests/, and a pattern-specific variable that matches
# nothing is silent — the -O2 would simply stop applying with no diagnostic.
# fpu_demo made exactly that move on the day /tests was created.
OPTIMIZED_APPS := fpu_demo
$(foreach a,$(OPTIMIZED_APPS),\
  $(eval $(OBJ)/$(if $(filter $(a),$(TESTS)),tests,apps)/$(a)/%.c.o: CFLAGS += -O2))

$(OBJ)/%.c.o: %.c
	@mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ)/%.S.o: %.S
	@mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -rf $(OBJ) $(BIN)
