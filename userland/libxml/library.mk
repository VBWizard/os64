# The XML event parser has allocator callbacks and no runtime dependency.
LIBXML_SRCS := libxml/xml.c
LIBXML_OBJS := $(patsubst %,$(OBJ)/pic/%.o,$(LIBXML_SRCS))
LIBXML_SO := $(BIN)/libxml.so
LIBXML_CFLAGS = $(LIBOS64_CFLAGS) -O2 -fvisibility=hidden
LIBXML_BASE = $(patsubst libxml.so=%,%,$(filter libxml.so=%,$(LIB_BASE_PAIRS)))
LIBXML_LDFLAGS = $(SHARED_LIB_LDFLAGS) -soname libxml.so --no-undefined

$(LIBXML_SO): $(LIBXML_OBJS) $(CURDIR)/link/lib.ld $(CURDIR)/tools/app_bases.py libxml/library.mk
	@mkdir -p "$(BIN)"
	$(LD) --defsym LIB_BASE=$(LIBXML_BASE) \
	      --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) $(LIBXML_LDFLAGS) \
	      -o $@ $(LIBXML_OBJS)
	@printf '  %-10s prelinked at %s\n' "libxml.so" "$(LIBXML_BASE)"

$(OBJ)/pic/libxml/%.c.o: libxml/%.c libxml/library.mk GNUmakefile
	@mkdir -p "$(dir $@)"
	$(CC) $(LIBXML_CFLAGS) -c $< -o $@

-include $(LIBXML_OBJS:.o=.d)

.PHONY: xml-library
xml-library: $(LIBXML_SO)
