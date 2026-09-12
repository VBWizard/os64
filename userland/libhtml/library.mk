# HTML is an optional userland parser. Only its consumers acquire this edge;
# libos64 remains independent of parser code and generated entity tables.
LIBHTML_SRCS := libhtml/core.c libhtml/encoding.c libhtml/tokenizer.c libhtml/tree.c
LIBHTML_OBJS := $(patsubst %,$(OBJ)/pic/%.o,$(LIBHTML_SRCS))
LIBHTML_SO := $(BIN)/libhtml.so
# The saved Wikipedia page takes 54.5ms at -O0 and 23.6ms at -O2 on the host
# (same tree/work/arena). The sanitizer suite also runs at this optimization.
LIBHTML_CFLAGS = $(LIBOS64_CFLAGS) -O2 -fvisibility=hidden
LIBHTML_BASE = $(patsubst libhtml.so=%,%,$(filter libhtml.so=%,$(LIB_BASE_PAIRS)))
LIBHTML_LDFLAGS = $(SHARED_LIB_LDFLAGS) -soname libhtml.so

$(LIBHTML_SO): $(LIBHTML_OBJS) $(LIBOS64_SO) $(CURDIR)/link/lib.ld $(CURDIR)/tools/app_bases.py
	@mkdir -p "$(BIN)"
	$(LD) --defsym LIB_BASE=$(LIBHTML_BASE) \
	      --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) $(LIBHTML_LDFLAGS) --no-as-needed \
	      -o $@ $(LIBHTML_OBJS) $(LIBOS64_SO)
	@printf '  %-10s prelinked at %s\n' "libhtml.so" "$(LIBHTML_BASE)"

$(OBJ)/pic/libhtml/%.c.o: libhtml/%.c libhtml/library.mk GNUmakefile
	@mkdir -p "$(dir $@)"
	$(CC) $(LIBHTML_CFLAGS) -c $< -o $@

-include $(LIBHTML_OBJS:.o=.d)
