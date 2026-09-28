# libgarb is the page's garb (GARB.md): CSS, parsed and cascaded. It records
# DT_NEEDED edges on libhtml (the encoding labels today, the tree the
# cascade walks) and libos64 (strings, arenas, the formatter).
LIBGARB_SRCS := libgarb/tokenize.c libgarb/parse.c libgarb/decode.c libgarb/dump.c
LIBGARB_OBJS := $(patsubst %,$(OBJ)/pic/%.o,$(LIBGARB_SRCS))
LIBGARB_SO := $(BIN)/libgarb.so
# -O2, libhtml's reasoning: a 2026 page brings hundreds of kilobytes of CSS,
# and the tokenizer reads every byte.
LIBGARB_CFLAGS = $(LIBOS64_CFLAGS) -O2 -fvisibility=hidden -I$(CURDIR)/libgarb/include
LIBGARB_BASE = $(patsubst libgarb.so=%,%,$(filter libgarb.so=%,$(LIB_BASE_PAIRS)))
LIBGARB_LDFLAGS = $(SHARED_LIB_LDFLAGS) -soname libgarb.so

$(LIBGARB_SO): $(LIBGARB_OBJS) $(LIBHTML_SO) $(LIBOS64_SO) $(CURDIR)/link/lib.ld \
               $(CURDIR)/tools/app_bases.py
	@mkdir -p "$(BIN)"
	$(LD) --defsym LIB_BASE=$(LIBGARB_BASE) \
	      --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) $(LIBGARB_LDFLAGS) --no-as-needed \
	      -o $@ $(LIBGARB_OBJS) $(LIBHTML_SO) $(LIBOS64_SO)
	@printf '  %-10s prelinked at %s\n' "libgarb.so" "$(LIBGARB_BASE)"

$(OBJ)/pic/libgarb/%.c.o: libgarb/%.c libgarb/library.mk GNUmakefile
	@mkdir -p "$(dir $@)"
	$(CC) $(LIBGARB_CFLAGS) -c $< -o $@

-include $(LIBGARB_OBJS:.o=.d)
