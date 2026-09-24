# shared.mk — how libfreetype.so is built.
#
# THE PROPERTY THIS FILE EXISTS TO PROTECT: libfreetype.so is a LEAF. It
# records no DT_NEEDED, it resolves every symbol it references from its own
# objects, and `--no-undefined` on the link is what turns that from an
# intention into a build failure. libos64 may depend on it; it depends on
# nothing. See FONT_CONTRACTS.md for why the graph runs that way.
include libfreetype/sources.mk

LIBFREETYPE_SO   := $(BIN)/libfreetype.so
LIBFREETYPE_BASE := $(patsubst libfreetype.so=%,%,$(filter libfreetype.so=%,$(LIB_BASE_PAIRS)))
FREETYPE_OBJS    := $(patsubst %,$(OBJ)/freetype/%.o,$(FREETYPE_UPSTREAM_SRCS) $(FREETYPE_PORT_SRCS))

# -O2, against userland's -O0 default: this is the arithmetic that turns an
# outline into pixels, and it runs per glyph on every repaint.
#
# Keep GCC's loop-to-libcall transformation disabled in the private runtime.
# Its memory aliases must not acquire recursive calls under another compiler
# version or flag set. tools/test_freetype_target_runtime.py checks runtime
# behavior separately from this build-policy requirement.
#
# -fno-builtin keeps the compiler from assuming the semantics of the other
# str/mem names the port defines, and -fvisibility=hidden makes every symbol
# local by default so exports.map has only to keep that true.
#
# -I ORDER IS LOAD-BEARING: port/ must precede upstream/include, so that
# port/ft2build.h is the copy every upstream source finds. That file names the
# three os64 configuration headers, so losing the ordering does not fail the
# build — it silently compiles upstream's defaults instead.
FREETYPE_FLAGS := $(LIBOS64_CFLAGS) -O2 -std=c11 -fvisibility=hidden \
    -fno-builtin -fno-tree-loop-distribute-patterns \
    -Ilibfreetype/port -Ilibfreetype/upstream/include

ifeq ($(filter -fno-tree-loop-distribute-patterns,$(FREETYPE_FLAGS)),)
$(error FreeType private runtime requires -fno-tree-loop-distribute-patterns)
endif

# Upstream's sources want FT2_BUILD_LIBRARY; the port must NOT have it — the
# adapter is a CONSUMER of the public headers, and building it as library
# internals would let it reach past the interface it is supposed to hide.
#
# The -Wno- pair applies to UPSTREAM FILES ONLY, and only because trimming the
# module set leaves locals behind that a fuller build uses: with the bytecode
# interpreter and embedded bitmaps compiled out, `TT_Load_Glyph` still declares
# the face it no longer consults. os64's own code keeps the full alarm.
FREETYPE_UPSTREAM_FLAGS := -DFT2_BUILD_LIBRARY \
    -Wno-unused-variable -Wno-unused-but-set-variable
$(OBJ)/freetype/libfreetype/%.c.o: libfreetype/%.c libfreetype/shared.mk \
        libfreetype/port/os64_ftoption.h libfreetype/port/os64_ftmodule.h \
        libfreetype/port/os64_ftstdlib.h libfreetype/port/ft2build.h
	@mkdir -p "$(dir $@)"
	$(CC) $(FREETYPE_FLAGS) $(if $(findstring /upstream/,$<),$(FREETYPE_UPSTREAM_FLAGS)) -c $< -o $@

$(OBJ)/freetype/libfreetype/%.S.o: libfreetype/%.S libfreetype/shared.mk
	@mkdir -p "$(dir $@)"
	$(CC) $(FREETYPE_FLAGS) -c $< -o $@

# No $(LIBOS64_SO) on this line, and that absence is the whole design: with
# --no-undefined, anything the port forgot to implement stops the build here
# instead of quietly acquiring libos64 as a dependency.
$(LIBFREETYPE_SO): $(FREETYPE_OBJS) libfreetype/exports.map link/lib.ld \
        tools/app_bases.py libfreetype/shared.mk
	@mkdir -p "$(BIN)"
	$(LD) --defsym LIB_BASE=$(LIBFREETYPE_BASE) --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) \
	    $(SHARED_LIB_LDFLAGS) -soname libfreetype.so --no-undefined --no-as-needed \
	    --version-script=libfreetype/exports.map -Map=$(OBJ)/freetype/link.map \
	    -o $@ $(FREETYPE_OBJS)

-include $(FREETYPE_OBJS:.o=.d)
