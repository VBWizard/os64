# shared.mk — how libav.so is built.
#
# FFmpeg's libavutil, libavcodec, libavformat and libswscale, in the configuration
# manifest.json's configure_args names (sources.mk is generated from it by
# tools/import_av.py), as ONE shared library of its own. It is LGPL, and a
# separate library the user can replace is how os64 meets that licence:
# never link these objects into anything else (README.md § The licence).
# It needs an allocator and strings (libos64.so) and double-precision maths
# (libmath.so); --no-undefined makes anything else a build failure.
include libav/sources.mk

LIBAV_SO   := $(BIN)/libav.so
LIBAV_BASE := $(patsubst libav.so=%,%,$(filter libav.so=%,$(LIB_BASE_PAIRS)))
AV_OBJS    := $(patsubst %,$(OBJ)/av/%.o,$(AV_UPSTREAM_SRCS) $(AV_UPSTREAM_ASM) $(AV_PORT_SRCS))

# Both halves see the shims first (port/compat, the C library FFmpeg's
# sources call), then configure's generated headers, then FFmpeg's tree,
# and all of them AHEAD of userland's own -I list, which names libmath's
# <math.h> that compat/math.h wraps.
AV_INCLUDES := -Ilibav/port/compat -Ilibav/port -Ilibav/port/config -Ilibav/upstream

# FFmpeg's sources build with FFmpeg's own flags (its configure's CFLAGS for
# this target, ffbuild/config.mak): the maths it is written for, its
# warning set and the warnings it makes errors, not os64's -Wextra -Werror,
# which upstream code was never written against (the SBC port's rule).
# Two of FFmpeg's guards matter more here than on Linux, because a thread's
# stack is fixed: -Werror=vla and -Wstack-usage. os64's code generation is
# kept: PIC, no red zone, SSE2 and no AVX, the stack-clash probes (so the
# only AVX in the library is FFmpeg's assembly, chosen at run time). Taken
# explicitly: -fomit-frame-pointer, because FFmpeg's inline assembly (H.264's
# CABAC reader, x86/cabac.h) uses RBP as a register, which a function that
# keeps a frame pointer refuses to compile; and -DPIC, configure's own for
# --enable-pic, without which that inline assembly addresses its tables
# absolutely (libavutil/x86/asm.h), which a shared library cannot link
# (-z text says so by name). Left out: -Wredundant-decls (the
# shims declare what FFmpeg's internal headers declare again, av_sscanf, by
# design) and userland's -masm=intel (FFmpeg is written for GCC's default
# syntax, and in Intel syntax a symbol that is spelled like a register,
# which FFmpeg has, does not assemble). One of its flags is changed: -O2
# where configure says -O3, the level os64 builds the library at now that it
# decodes video (Chris's ruling).
AV_UPSTREAM_FLAGS := $(AV_INCLUDES) $(filter-out -O2 -Wextra -Werror -masm=intel,$(LIBOS64_CFLAGS)) \
    -std=c17 -O2 -fomit-frame-pointer -fno-math-errno -fno-signed-zeros -mno-avx -mno-fma \
    -D_ISOC11_SOURCE -DHAVE_AV_CONFIG_H -DPIC \
    -Wdisabled-optimization -Wpointer-arith -Wwrite-strings -Wtype-limits \
    -Wundef -Wempty-body -Wstack-usage=122880 -Wimplicit-fallthrough=5 \
    -Wmissing-prototypes -Wstrict-prototypes \
    -Wno-parentheses -Wno-switch -Wno-format-zero-length -Wno-pointer-sign \
    -Wno-unused-const-variable -Wno-bool-operation -Wno-char-subscripts -Wno-maybe-uninitialized \
    -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=format-security \
    -Werror=return-type -Werror=vla -Wformat

# os64's own code in the port keeps the full alarm. It is libav's build
# too, so it sees the shims' libav-only names (compat/string.h).
AV_PORT_FLAGS := $(AV_INCLUDES) $(LIBOS64_CFLAGS) -std=c17 -mno-avx -mno-fma -fno-builtin -DHAVE_AV_CONFIG_H

$(OBJ)/av/upstream/%.c.o: libav/upstream/%.c libav/shared.mk
	@mkdir -p "$(dir $@)"
	$(CC) $(AV_UPSTREAM_FLAGS) -c $< -o $@

$(OBJ)/av/port/%.c.o: libav/port/%.c libav/shared.mk
	@mkdir -p "$(dir $@)"
	$(CC) $(AV_PORT_FLAGS) -c $< -o $@

# FFmpeg's x86 assembly, assembled the way its own makefiles do (common.mak:
# -I. -I$(SRC_PATH) -I$(<D)/ -Pconfig.asm, and configure's -f elf64 for
# this target); x86inc.asm makes every routine position-independent on
# x86-64. nasm is a prerequisite of the build, and its absence is said by
# name before any assembly is attempted.
NASM := $(shell command -v nasm 2>/dev/null)
$(OBJ)/av/.nasm:
	@if [ -z "$(NASM)" ]; then \
	    echo "libav: nasm is required to build FFmpeg's x86 assembly (install nasm)" >&2; exit 1; fi
	@mkdir -p "$(dir $@)" && touch $@

$(OBJ)/av/upstream/%.asm.o: libav/upstream/%.asm libav/shared.mk | $(OBJ)/av/.nasm
	@mkdir -p "$(dir $@)"
	$(NASM) -f elf64 -Ilibav/port/config/ -Ilibav/upstream/ -I$(dir $<) \
	    -Plibav/port/config/config.asm -MD $(@:.o=.d) -MT $@ -o $@ $<

# sources.mk is on this line: removing a file from the list creates no newer
# object, so without it the old library would look up to date.
$(LIBAV_SO): $(AV_OBJS) $(LIBOS64_SO) $(LIBMATH_SO) libav/exports.map link/lib.ld \
        tools/app_bases.py libav/shared.mk libav/sources.mk
	@mkdir -p "$(BIN)"
	$(LD) --defsym LIB_BASE=$(LIBAV_BASE) --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) \
	    $(SHARED_LIB_LDFLAGS) -soname libav.so --no-undefined --no-as-needed --gc-sections -z text \
	    --version-script=libav/exports.map -Map=$(OBJ)/av/link.map \
	    -o $@ $(AV_OBJS) $(LIBOS64_SO) $(LIBMATH_SO)

-include $(AV_OBJS:.o=.d)

# What a program that calls FFmpeg compiles with, and who those programs
# are. FFmpeg's public headers include C-library headers, so a caller sees
# the shims too; it never sees HAVE_AV_CONFIG_H, which is FFmpeg's own.
LIBAV_CONSUMER_CFLAGS := -I$(CURDIR)/libav/port/compat -I$(CURDIR)/libav/port \
    -I$(CURDIR)/libav/port/config -I$(CURDIR)/libav/upstream
LIBAV_PROGRAMS := avtest avbench
