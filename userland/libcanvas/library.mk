# Native canvas: allocation and scheduling belong to the caller.
LIBCANVAS_SRCS := libcanvas/gradient.c libcanvas/core.c libcanvas/path.c libcanvas/shadow.c
LIBCANVAS_OBJS := $(patsubst %,$(OBJ)/pic/%.o,$(LIBCANVAS_SRCS))
LIBCANVAS_SO := $(BIN)/libcanvas.so
LIBCANVAS_BASE = $(patsubst libcanvas.so=%,%,$(filter libcanvas.so=%,$(LIB_BASE_PAIRS)))
LIBCANVAS_CFLAGS = $(LIBOS64_CFLAGS) -O2 -fvisibility=hidden -fno-builtin \
    -fno-tree-loop-distribute-patterns -fno-fast-math -ffp-contract=off

$(LIBCANVAS_SO): $(LIBCANVAS_OBJS) $(LIBDRAW2D_SO) $(LIBMATH_SO) link/lib.ld tools/app_bases.py libcanvas/library.mk
	@mkdir -p "$(BIN)"
	$(LD) --defsym LIB_BASE=$(LIBCANVAS_BASE) --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) \
	    $(SHARED_LIB_LDFLAGS) -soname libcanvas.so --no-undefined --no-as-needed \
	    -o $@ $(LIBCANVAS_OBJS) $(LIBDRAW2D_SO) $(LIBMATH_SO)

$(OBJ)/pic/libcanvas/%.c.o: libcanvas/%.c libcanvas/library.mk GNUmakefile
	@mkdir -p "$(dir $@)"
	$(CC) $(LIBCANVAS_CFLAGS) -c $< -o $@

-include $(LIBCANVAS_OBJS:.o=.d)
.PHONY: canvas-library
canvas-library: $(LIBCANVAS_SO)
