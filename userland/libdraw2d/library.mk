# Native draw2d: allocation and scheduling belong to the caller.
LIBDRAW2D_SRCS := libdraw2d/gradient.c libdraw2d/paint.c libdraw2d/stroke.c
LIBDRAW2D_OBJS := $(patsubst %,$(OBJ)/pic/%.o,$(LIBDRAW2D_SRCS))
LIBDRAW2D_SO := $(BIN)/libdraw2d.so
LIBDRAW2D_BASE = $(patsubst libdraw2d.so=%,%,$(filter libdraw2d.so=%,$(LIB_BASE_PAIRS)))
LIBDRAW2D_CFLAGS = $(LIBOS64_CFLAGS) -O2 -fvisibility=hidden -fno-builtin \
    -fno-tree-loop-distribute-patterns -fno-fast-math -ffp-contract=off

$(LIBDRAW2D_SO): $(LIBDRAW2D_OBJS) $(LIBMATH_SO) link/lib.ld tools/app_bases.py libdraw2d/library.mk
	@mkdir -p "$(BIN)"
	$(LD) --defsym LIB_BASE=$(LIBDRAW2D_BASE) --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) \
	    $(SHARED_LIB_LDFLAGS) -soname libdraw2d.so --no-undefined --no-as-needed \
	    -o $@ $(LIBDRAW2D_OBJS) $(LIBMATH_SO)

$(OBJ)/pic/libdraw2d/%.c.o: libdraw2d/%.c libdraw2d/library.mk GNUmakefile
	@mkdir -p "$(dir $@)"
	$(CC) $(LIBDRAW2D_CFLAGS) -c $< -o $@

-include $(LIBDRAW2D_OBJS:.o=.d)
.PHONY: draw2d-library
draw2d-library: $(LIBDRAW2D_SO)
