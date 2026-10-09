# libdom binds the script-visible document to libhtml and persistent libpage
# control state. Consumers acquire these edges through this shared library.
LIBDOM_SRCS := libdom/core.c libdom/node.c libdom/collection.c libdom/content.c \
    libdom/event.c libdom/timer.c libdom/window.c libdom/geometry.c libdom/classic.c libdom/style.c \
    libdom/mixin.c
LIBDOM_OBJS := $(patsubst %,$(OBJ)/pic/%.o,$(LIBDOM_SRCS))
LIBDOM_SO := $(BIN)/libdom.so
LIBDOM_BASE = $(patsubst libdom.so=%,%,$(filter libdom.so=%,$(LIB_BASE_PAIRS)))
LIBDOM_CFLAGS = $(LIBJS_FLAGS) -I$(CURDIR)/libdom/include -I$(CURDIR)/libgarb/include

$(LIBDOM_SO): $(LIBDOM_OBJS) $(LIBJS_SO) $(LIBHTML_SO) $(LIBPAGE_SO) $(LIBGARB_SO) $(LIBOS64_SO) \
    link/lib.ld GNUmakefile libdom/library.mk tools/app_bases.py
	$(LD) --defsym LIB_BASE=$(LIBDOM_BASE) --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) \
	    $(SHARED_LIB_LDFLAGS) -soname libdom.so --no-undefined --no-as-needed \
	    -o $@ $(LIBDOM_OBJS) $(LIBJS_SO) $(LIBPAGE_SO) $(LIBHTML_SO) $(LIBGARB_SO) $(LIBOS64_SO)

$(OBJ)/pic/libdom/%.c.o: libdom/%.c $(LIBJS_GENERATED)/.prepared libdom/library.mk GNUmakefile
	@mkdir -p "$(@D)"
	$(CC) $(LIBDOM_CFLAGS) -c $< -o $@
-include $(LIBDOM_OBJS:.o=.d)

.PHONY: dom-library
dom-library: $(LIBDOM_SO)

# Optional geometry/stack consumer in the reserved JavaScript fixture slot.
# It is installed into validation images, outside the standard app inventory.
LIBDOM_GEOMETRY_TEST_OBJ := $(OBJ)/dom/geometry-test.o
LIBDOM_GEOMETRY_TEST_ELF := $(OBJ)/dom/geometry-test
$(LIBDOM_GEOMETRY_TEST_OBJ): libdom/tests/geometry.c libdom/include/dom/dom.h \
    apps/yonder/geometry.h $(LIBJS_GENERATED)/.prepared libdom/library.mk
	@mkdir -p "$(@D)"
	$(CC) $(filter-out -fPIC,$(LIBDOM_CFLAGS)) -fno-pic -fno-pie -c $< -o $@
$(LIBDOM_GEOMETRY_TEST_ELF): $(LIBDOM_GEOMETRY_TEST_OBJ) $(OBJ)/apps/yonder/geometry.c.o \
    $(LAUNCH_OBJS) $(LIBDOM_SO) $(LIBJS_SO) $(LIBFLOW_SO) $(LIBGARB_SO) $(LIBPAGE_SO) $(LIBHTML_SO) $(BIN)/libfreetype.so $(LIBOS64_SO)
	$(LD) --defsym APP_BASE=$(APP_BASE_jsembedtest) --defsym APP_SLOT_SIZE=$(APP_SLOT_SIZE) \
	    $(LDFLAGS) -o $@ $^
.PHONY: dom-geometry-test
dom-geometry-test: $(LIBDOM_GEOMETRY_TEST_ELF)
-include $(LIBDOM_GEOMETRY_TEST_OBJ:.o=.d)
