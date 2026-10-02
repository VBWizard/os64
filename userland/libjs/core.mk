# R1's freestanding engine core; the shared link also needs libmath.
LIBJS_GENERATED := $(OBJ)/js/upstream
LIBJS_CORE_NAMES := quickjs dtoa libregexp libunicode cutils
LIBJS_CORE_OBJS := $(addprefix $(OBJ)/js/,$(addsuffix .o,$(LIBJS_CORE_NAMES)))
LIBJS_PORT_NAMES := allocator platform format memory
LIBJS_PORT_OBJS := $(addprefix $(OBJ)/js/port/,$(addsuffix .o,$(LIBJS_PORT_NAMES)))
LIBJS_CORE := $(OBJ)/js/core.o
LIBJS_FLAGS := $(LIBOS64_CFLAGS) -O2 -std=gnu11 -fvisibility=hidden -fno-builtin \
    -fno-tree-loop-distribute-patterns -DOS64_JS_TARGET \
    -I$(CURDIR)/libmath/include -I$(CURDIR)/libjs/port/compat \
    -I$(CURDIR)/libjs/port -I$(CURDIR)/libjs/include \
    -isystem $(LIBJS_GENERATED) -DCONFIG_VERSION=\"2026-06-04\"
# Unused callback arguments, signed loop comparisons, partial aggregate
# initializers and the JSON switch's intentional fallthrough occur in the
# pinned upstream core. These warning exceptions stay there.
LIBJS_UPSTREAM_WARNINGS := -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers -Wno-implicit-fallthrough

$(LIBJS_GENERATED)/.prepared: $(wildcard libjs/upstream/* libjs/patches/*.patch) libjs/manifest.json ../tools/js_prepare.py
	@mkdir -p "$(LIBJS_GENERATED)"
	python3 ../tools/js_prepare.py "$(LIBJS_GENERATED)"
	@touch $@

$(LIBJS_CORE_OBJS): $(OBJ)/js/%.o: $(LIBJS_GENERATED)/.prepared libjs/core.mk
	$(CC) $(LIBJS_FLAGS) $(LIBJS_UPSTREAM_WARNINGS) -c $(LIBJS_GENERATED)/$*.c -o $@

$(OBJ)/js/port/%.o: libjs/port/%.c $(LIBJS_GENERATED)/.prepared libjs/core.mk
	@mkdir -p "$(@D)"
	$(CC) $(LIBJS_FLAGS) -c $< -o $@

# Only compiler helpers referenced by these target objects are pulled in.
$(LIBJS_CORE): $(LIBJS_CORE_OBJS) $(LIBJS_PORT_OBJS)
	$(LD) -r -o $@ $^ $(shell $(CC) -print-libgcc-file-name)

.PHONY: js-core
js-core: $(LIBJS_CORE)
-include $(LIBJS_CORE_OBJS:.o=.d) $(LIBJS_PORT_OBJS:.o=.d)
