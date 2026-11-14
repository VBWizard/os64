# shared.mk — how libav.so is built.
#
# FFmpeg's libavutil, libavcodec and libavformat, in the configuration
# manifest.json's configure_args names (sources.mk is generated from it by
# tools/import_av.py), as ONE shared library of its own. It is LGPL, and a
# separate library the user can replace is how os64 meets that licence:
# never link these objects into anything else (README.md § The licence).
# It needs an allocator and strings (libos64.so) and double-precision maths
# (libmath.so); --no-undefined makes anything else a build failure.
include libav/sources.mk

LIBAV_SO   := $(BIN)/libav.so
LIBAV_BASE := $(patsubst libav.so=%,%,$(filter libav.so=%,$(LIB_BASE_PAIRS)))
AV_OBJS    := $(patsubst %,$(OBJ)/av/%.o,$(AV_UPSTREAM_SRCS) $(AV_PORT_SRCS))

# Both halves see the shims first (port/compat, the C library FFmpeg's
# sources call), then configure's generated headers, then FFmpeg's tree,
# and all of them AHEAD of userland's own -I list, which names libmath's
# <math.h> that compat/math.h wraps.
AV_INCLUDES := -Ilibav/port/compat -Ilibav/port -Ilibav/port/config -Ilibav/upstream

# FFmpeg's sources build with FFmpeg's own flags (its configure's CFLAGS for
# this target, ffbuild/config.mak): -O3, the maths it is written for, and its
# own warning set, not os64's -Wextra -Werror, which upstream code was never
# written against (the SBC port's rule). os64's code generation is kept: PIC,
# no red zone, SSE2 and no AVX, the stack-clash probes. FFmpeg's
# -Wredundant-decls is left out: the shims declare what FFmpeg's internal
# headers declare again (av_sscanf), by design. And -masm=intel is left out:
# FFmpeg is written for GCC's default syntax, and in Intel syntax a symbol
# that is spelled like a register (FFmpeg has them) does not assemble.
AV_UPSTREAM_FLAGS := $(AV_INCLUDES) $(filter-out -O2 -Wextra -Werror -masm=intel,$(LIBOS64_CFLAGS)) \
    -std=c17 -O3 -fno-math-errno -fno-signed-zeros -mno-avx -mno-fma -fno-builtin \
    -D_ISOC11_SOURCE -DHAVE_AV_CONFIG_H \
    -Wdisabled-optimization -Wpointer-arith -Wwrite-strings -Wtype-limits \
    -Wundef -Wempty-body -Wmissing-prototypes -Wstrict-prototypes \
    -Wno-parentheses -Wno-switch -Wno-format-zero-length -Wno-pointer-sign \
    -Wno-unused-const-variable -Wno-bool-operation -Wno-char-subscripts -Wno-maybe-uninitialized \
    -Werror=implicit-function-declaration -Werror=return-type

# os64's own code in the port keeps the full alarm.
AV_PORT_FLAGS := $(AV_INCLUDES) $(LIBOS64_CFLAGS) -std=c17 -mno-avx -mno-fma -fno-builtin

$(OBJ)/av/upstream/%.c.o: libav/upstream/%.c libav/shared.mk
	@mkdir -p "$(dir $@)"
	$(CC) $(AV_UPSTREAM_FLAGS) -c $< -o $@

$(OBJ)/av/port/%.c.o: libav/port/%.c libav/shared.mk
	@mkdir -p "$(dir $@)"
	$(CC) $(AV_PORT_FLAGS) -c $< -o $@

# sources.mk is on this line: removing a file from the list creates no newer
# object, so without it the old library would look up to date.
$(LIBAV_SO): $(AV_OBJS) $(LIBOS64_SO) $(LIBMATH_SO) libav/exports.map link/lib.ld \
        tools/app_bases.py libav/shared.mk libav/sources.mk
	@mkdir -p "$(BIN)"
	$(LD) --defsym LIB_BASE=$(LIBAV_BASE) --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) \
	    $(SHARED_LIB_LDFLAGS) -soname libav.so --no-undefined --no-as-needed --gc-sections \
	    --version-script=libav/exports.map -Map=$(OBJ)/av/link.map \
	    -o $@ $(AV_OBJS) $(LIBOS64_SO) $(LIBMATH_SO)

-include $(AV_OBJS:.o=.d)

# What a program that calls FFmpeg compiles with, and who those programs
# are. FFmpeg's public headers include C-library headers, so a caller sees
# the shims too; it never sees HAVE_AV_CONFIG_H, which is FFmpeg's own.
LIBAV_CONSUMER_CFLAGS := -I$(CURDIR)/libav/port/compat -I$(CURDIR)/libav/port \
    -I$(CURDIR)/libav/port/config -I$(CURDIR)/libav/upstream
LIBAV_PROGRAMS := avtest
