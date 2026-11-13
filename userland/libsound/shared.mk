# shared.mk — how libsound.so is built.
#
# One upstream translation unit, speexdsp's resample.c, compiled as it came
# with the switch upstream provides for taking it alone (OUTSIDE_SPEEX), and
# os64's wrapper around it. It needs an allocator (libos64.so) and sin
# (libmath.so), and --no-undefined makes anything else a build failure.
include libsound/sources.mk

LIBSOUND_SO   := $(BIN)/libsound.so
LIBSOUND_BASE := $(patsubst libsound.so=%,%,$(filter libsound.so=%,$(LIB_BASE_PAIRS)))
SOUND_OBJS    := $(patsubst %,$(OBJ)/sound/%.o,$(SOUND_UPSTREAM_SRCS) $(SOUND_PORT_SRCS))

# FLOATING_POINT is upstream's reference build of the resampler; its 16-bit
# entry points convert at the edges. RANDOM_PREFIX is the name prefix its
# header insists on when it is built outside speexdsp. EXPORT is its
# visibility hook, emptied: exports.map decides what leaves the library.
SOUND_FLAGS := $(LIBOS64_CFLAGS) -O2 -std=c11 -fvisibility=hidden -fno-builtin \
    -mno-avx -mno-fma -DOUTSIDE_SPEEX -DRANDOM_PREFIX=os64_speex -DFLOATING_POINT -DEXPORT= \
    -Ilibsound/port/compat -Ilibsound/port -Ilibsound/upstream/include/speex \
    -Ilibsound/upstream/include -Ilibsound/upstream/libspeexdsp -Ilibsound/include

# UPSTREAM FILES ONLY, each flag for a diagnosed reason (libsound/README.md
# § Warnings). os64's own code keeps the full alarm.
SOUND_UPSTREAM_FLAGS := -Wno-sign-compare

$(OBJ)/sound/libsound/%.c.o: libsound/%.c libsound/shared.mk
	@mkdir -p "$(dir $@)"
	$(CC) $(SOUND_FLAGS) $(if $(findstring /upstream/,$<),$(SOUND_UPSTREAM_FLAGS)) -c $< -o $@

# sources.mk is on this line: removing a file from the list creates no newer
# object, so without it the old library would look up to date.
$(LIBSOUND_SO): $(SOUND_OBJS) $(LIBOS64_SO) $(LIBMATH_SO) libsound/exports.map link/lib.ld \
        tools/app_bases.py libsound/shared.mk libsound/sources.mk
	@mkdir -p "$(BIN)"
	$(LD) --defsym LIB_BASE=$(LIBSOUND_BASE) --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) \
	    $(SHARED_LIB_LDFLAGS) -soname libsound.so --no-undefined --no-as-needed \
	    --version-script=libsound/exports.map -Map=$(OBJ)/sound/link.map \
	    -o $@ $(SOUND_OBJS) $(LIBOS64_SO) $(LIBMATH_SO)

-include $(SOUND_OBJS:.o=.d)
