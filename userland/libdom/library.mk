# libdom binds the script-visible document to libhtml and persistent libpage
# control state. Consumers acquire these edges through this shared library.
LIBDOM_SRCS := libdom/core.c libdom/node.c libdom/collection.c libdom/content.c \
    libdom/event.c libdom/timer.c libdom/window.c
LIBDOM_OBJS := $(patsubst %,$(OBJ)/pic/%.o,$(LIBDOM_SRCS))
LIBDOM_SO := $(BIN)/libdom.so
LIBDOM_BASE = $(patsubst libdom.so=%,%,$(filter libdom.so=%,$(LIB_BASE_PAIRS)))
LIBDOM_CFLAGS = $(LIBJS_FLAGS) -I$(CURDIR)/libdom/include

$(LIBDOM_SO): $(LIBDOM_OBJS) $(LIBJS_SO) $(LIBHTML_SO) $(LIBPAGE_SO) $(LIBOS64_SO) \
    link/lib.ld GNUmakefile libdom/library.mk tools/app_bases.py
	$(LD) --defsym LIB_BASE=$(LIBDOM_BASE) --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) \
	    $(SHARED_LIB_LDFLAGS) -soname libdom.so --no-undefined --no-as-needed \
	    -o $@ $(LIBDOM_OBJS) $(LIBJS_SO) $(LIBPAGE_SO) $(LIBHTML_SO) $(LIBOS64_SO)

$(OBJ)/pic/libdom/%.c.o: libdom/%.c $(LIBJS_GENERATED)/.prepared libdom/library.mk GNUmakefile
	@mkdir -p "$(@D)"
	$(CC) $(LIBDOM_CFLAGS) -c $< -o $@
-include $(LIBDOM_OBJS:.o=.d)

.PHONY: dom-library
dom-library: $(LIBDOM_SO)
