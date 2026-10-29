# libdom binds the script-visible document to libhtml and persistent libpage
# control state. Consumers acquire these edges through this shared library.
LIBDOM_SRCS := libdom/core.c libdom/node.c libdom/collection.c libdom/content.c \
    libdom/event.c libdom/timer.c libdom/window.c libdom/geometry.c libdom/classic.c libdom/style.c \
    libdom/mixin.c libdom/modern.c libdom/view.c
# The Modern Web's slice 1a: storage, performance, URL, encoding, screen, base64.
LIBDOM_SRCS += libdom/storage.c libdom/performance.c libdom/url.c libdom/encoding.c libdom/screen.c \
    libdom/base64.c
# Slice 0c: DOMImplementation and the documents it makes.
LIBDOM_SRCS += libdom/implementation.c
# Slice 0d: Element.attributes, NamedNodeMap and Attr.
LIBDOM_SRCS += libdom/attr.c
# The Modern Web's slice 2: fetch and its objects, XMLHttpRequest, DOMException.
LIBDOM_SRCS += libdom/fetch.c libdom/abort.c libdom/blob.c libdom/xhr.c libdom/exception.c
# P1-W: window as a browsing context, its BarProps and external; crypto.
LIBDOM_SRCS += libdom/browsing.c libdom/crypto.c
# The frames row (IFRAME.md): HTMLIFrameElement.
LIBDOM_SRCS += libdom/frames.c
# P2a: a script's failures, as the window's error and rejection events.
LIBDOM_SRCS += libdom/report.c
# P1-Da: the document's state reads and document.body's setter.
LIBDOM_SRCS += libdom/document.c
# P2b: the structured clone, same-window postMessage, idle callbacks.
LIBDOM_SRCS += libdom/clone.c libdom/message.c libdom/idle.c
# 0f P1a: the select's options and the Option factory.
LIBDOM_SRCS += libdom/options.c
# 0f P2's core: the text controls' selection and the window's Selection.
LIBDOM_SRCS += libdom/selection.c
# 0f P3a: the controls' reflections, form owner and labels.
LIBDOM_SRCS += libdom/controls.c
# 0f P6a: the element kinds whose members are reflections.
LIBDOM_SRCS += libdom/elements.c
# 0f P6b: the tables' tree verbs and collections.
LIBDOM_SRCS += libdom/tables.c
# 0f P3b: the form's, label's, fieldset's, legend's and output's own members.
LIBDOM_SRCS += libdom/forms.c
# 0f P4: constraint validation.
LIBDOM_SRCS += libdom/validity.c
# Node's comparisons: isSameNode, isEqualNode, compareDocumentPosition.
LIBDOM_SRCS += libdom/compare.c
LIBDOM_SRCS += libdom/sheets.c
# Import maps: the map's JSON and HTMLScriptElement.supports.
LIBDOM_SRCS += libdom/importmap.c
LIBDOM_OBJS := $(patsubst %,$(OBJ)/pic/%.o,$(LIBDOM_SRCS))
LIBDOM_SO := $(BIN)/libdom.so
LIBDOM_BASE = $(patsubst libdom.so=%,%,$(filter libdom.so=%,$(LIB_BASE_PAIRS)))
LIBDOM_CFLAGS = $(LIBJS_FLAGS) -I$(CURDIR)/libdom/include -I$(CURDIR)/libgarb/include -I$(CURDIR)/libway/include

# libway for one copy of the origin and CORS rules (way/origin.h), which a
# script's Headers and Request ask as the fetch job does.
$(LIBDOM_SO): $(LIBDOM_OBJS) $(LIBJS_SO) $(LIBHTML_SO) $(LIBPAGE_SO) $(LIBGARB_SO) $(LIBWAY_SO) $(LIBOS64_SO) \
    link/lib.ld GNUmakefile libdom/library.mk tools/app_bases.py
	$(LD) --defsym LIB_BASE=$(LIBDOM_BASE) --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) \
	    $(SHARED_LIB_LDFLAGS) -soname libdom.so --no-undefined --no-as-needed \
	    -o $@ $(LIBDOM_OBJS) $(LIBJS_SO) $(LIBPAGE_SO) $(LIBHTML_SO) $(LIBGARB_SO) $(LIBWAY_SO) $(LIBOS64_SO)

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
