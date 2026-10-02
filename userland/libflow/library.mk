# libflow is the geometry of a page (LAYOUT.md): what libhtml parsed and
# libpage understood, laid out as boxes. It records DT_NEEDED edges on
# libgarb (the author's winners it computes), libpage (whether a node is a
# link or a control), libhtml (the tree it walks) and libos64 (strings, the
# formatter, the text engine); none of those knows a box exists. libgarb is
# included first so LIBGARB_SO is defined when this file's rules are read.
LIBFLOW_SRCS := libflow/store.c libflow/attrs.c libflow/style.c libflow/boxes.c libflow/layout.c \
                libflow/flow.c libflow/dump.c
LIBFLOW_OBJS := $(patsubst %,$(OBJ)/pic/%.o,$(LIBFLOW_SRCS))
LIBFLOW_SO := $(BIN)/libflow.so
# -O2, libhtml's and libpage's reasoning: a page is walked once per layout
# and the walk is pointer chasing.
LIBFLOW_CFLAGS = $(LIBOS64_CFLAGS) -O2 -fvisibility=hidden -I$(CURDIR)/libflow/include \
                 -I$(CURDIR)/libgarb/include
LIBFLOW_BASE = $(patsubst libflow.so=%,%,$(filter libflow.so=%,$(LIB_BASE_PAIRS)))
LIBFLOW_LDFLAGS = $(SHARED_LIB_LDFLAGS) -soname libflow.so

# library.mk holds the object list and the link flags: removing a source
# creates no newer object, so without it the old library would look current.
$(LIBFLOW_SO): $(LIBFLOW_OBJS) $(LIBGARB_SO) $(LIBPAGE_SO) $(LIBHTML_SO) $(LIBOS64_SO) $(CURDIR)/link/lib.ld \
               $(CURDIR)/tools/app_bases.py libflow/library.mk
	@mkdir -p "$(BIN)"
	$(LD) --defsym LIB_BASE=$(LIBFLOW_BASE) \
	      --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) $(LIBFLOW_LDFLAGS) --no-as-needed \
	      -o $@ $(LIBFLOW_OBJS) $(LIBGARB_SO) $(LIBPAGE_SO) $(LIBHTML_SO) $(LIBOS64_SO)
	@printf '  %-10s prelinked at %s\n' "libflow.so" "$(LIBFLOW_BASE)"

$(OBJ)/pic/libflow/%.c.o: libflow/%.c libflow/library.mk GNUmakefile
	@mkdir -p "$(dir $@)"
	$(CC) $(LIBFLOW_CFLAGS) -c $< -o $@

-include $(LIBFLOW_OBJS:.o=.d)
