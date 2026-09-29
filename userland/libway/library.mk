# libway is a browsing session (docs/completed/06-navigator.md): the history,
# how a page is loaded, and the judgements about what a page asks for,
# apart from whatever draws them. It records DT_NEEDED edges on libfetch
# (the I/O half), libpage (what a page asks), libhtml (the tree a page
# is) and libos64.
LIBWAY_SRCS := libway/session.c libway/load.c
LIBWAY_OBJS := $(patsubst %,$(OBJ)/pic/%.o,$(LIBWAY_SRCS))
LIBWAY_SO := $(BIN)/libway.so
LIBWAY_CFLAGS = $(LIBOS64_CFLAGS) -O2 -fvisibility=hidden -I$(CURDIR)/libway/include
LIBWAY_BASE = $(patsubst libway.so=%,%,$(filter libway.so=%,$(LIB_BASE_PAIRS)))
LIBWAY_LDFLAGS = $(SHARED_LIB_LDFLAGS) -soname libway.so

$(LIBWAY_SO): $(LIBWAY_OBJS) $(LIBFETCH_SO) $(LIBPAGE_SO) $(LIBHTML_SO) $(LIBOS64_SO) \
              $(CURDIR)/link/lib.ld $(CURDIR)/tools/app_bases.py
	@mkdir -p "$(BIN)"
	$(LD) --defsym LIB_BASE=$(LIBWAY_BASE) \
	      --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) $(LIBWAY_LDFLAGS) --no-as-needed \
	      -o $@ $(LIBWAY_OBJS) $(LIBFETCH_SO) $(LIBPAGE_SO) $(LIBHTML_SO) $(LIBOS64_SO)
	@printf '  %-10s prelinked at %s\n' "libway.so" "$(LIBWAY_BASE)"

$(OBJ)/pic/libway/%.c.o: libway/%.c libway/library.mk GNUmakefile
	@mkdir -p "$(dir $@)"
	$(CC) $(LIBWAY_CFLAGS) -c $< -o $@

-include $(LIBWAY_OBJS:.o=.d)
