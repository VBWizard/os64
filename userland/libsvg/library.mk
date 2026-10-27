# SVG owns no system allocator or I/O; libmath supplies arc arithmetic.
LIBSVG_SRCS := libsvg/core.c libsvg/path.c libsvg/geometry.c libsvg/bounds.c libsvg/stroke.c libsvg/scene.c libsvg/paint.c libsvg/gradient.c
LIBSVG_OBJS := $(patsubst %,$(OBJ)/pic/%.o,$(LIBSVG_SRCS))
LIBSVG_SO := $(BIN)/libsvg.so
LIBSVG_BASE = $(patsubst libsvg.so=%,%,$(filter libsvg.so=%,$(LIB_BASE_PAIRS)))
LIBSVG_CFLAGS = $(LIBOS64_CFLAGS) -O2 -fvisibility=hidden -fno-builtin \
    -fno-tree-loop-distribute-patterns -fno-fast-math -ffp-contract=off

$(LIBSVG_SO): $(LIBSVG_OBJS) $(LIBMATH_SO) link/lib.ld tools/app_bases.py libsvg/library.mk
	@mkdir -p "$(BIN)"
	$(LD) --defsym LIB_BASE=$(LIBSVG_BASE) --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) \
	    $(SHARED_LIB_LDFLAGS) -soname libsvg.so --no-undefined --no-as-needed \
	    -o $@ $(LIBSVG_OBJS) $(LIBMATH_SO)

$(OBJ)/pic/libsvg/%.c.o: libsvg/%.c libsvg/library.mk GNUmakefile
	@mkdir -p "$(dir $@)"
	$(CC) $(LIBSVG_CFLAGS) -c $< -o $@

-include $(LIBSVG_OBJS:.o=.d)
.PHONY: svg-library
svg-library: $(LIBSVG_SO)
