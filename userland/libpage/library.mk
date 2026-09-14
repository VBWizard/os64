# libpage is what a parsed page MEANS (LIBPAGE.md): form ownership, the
# submitter, the entry list, the encodings, reference resolution. It records
# real DT_NEEDED edges on libhtml (the tree it reads) and libos64 (the URL
# grammar and the Unicode table `dir=auto` needs); neither of those knows a
# form exists. Browsers and the focused guest test acquire this edge.
LIBPAGE_SRCS := libpage/core.c libpage/resolve.c libpage/value.c libpage/submit.c \
                libpage/encode.c libpage/refresh.c libpage/activate.c libpage/number.c libpage/range.c \
                libpage/upstream/ryu/ryu/d2s.c
LIBPAGE_OBJS := $(patsubst %,$(OBJ)/pic/%.o,$(LIBPAGE_SRCS))
LIBPAGE_SO := $(BIN)/libpage.so
# -O2 and the sanitizer suite at the same optimization, libhtml's reasoning:
# a page's forms are walked once per page and the walk is all pointer chasing.
LIBPAGE_CFLAGS = $(LIBOS64_CFLAGS) -O2 -fvisibility=hidden \
                -I$(CURDIR)/libpage/upstream/ryu/compat -I$(CURDIR)/libpage/upstream/ryu
LIBPAGE_BASE = $(patsubst libpage.so=%,%,$(filter libpage.so=%,$(LIB_BASE_PAIRS)))
LIBPAGE_LDFLAGS = $(SHARED_LIB_LDFLAGS) -soname libpage.so

$(LIBPAGE_SO): $(LIBPAGE_OBJS) $(LIBHTML_SO) $(LIBOS64_SO) $(CURDIR)/link/lib.ld \
               $(CURDIR)/tools/app_bases.py
	@mkdir -p "$(BIN)"
	$(LD) --defsym LIB_BASE=$(LIBPAGE_BASE) \
	      --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) $(LIBPAGE_LDFLAGS) --no-as-needed \
	      -o $@ $(LIBPAGE_OBJS) $(LIBHTML_SO) $(LIBOS64_SO)
	@printf '  %-10s prelinked at %s\n' "libpage.so" "$(LIBPAGE_BASE)"

$(OBJ)/pic/libpage/%.c.o: libpage/%.c libpage/library.mk GNUmakefile
	@mkdir -p "$(dir $@)"
	$(CC) $(LIBPAGE_CFLAGS) -c $< -o $@

-include $(LIBPAGE_OBJS:.o=.d)
