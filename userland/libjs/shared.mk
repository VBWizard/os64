include libjs/core.mk
LIBJS_RUNTIME_OBJS := $(OBJ)/js/runtime/runtime.o
$(OBJ)/js/runtime/%.o: libjs/runtime/%.c $(LIBJS_GENERATED)/.prepared libjs/shared.mk libjs/core.mk
	@mkdir -p "$(@D)"
	$(CC) $(LIBJS_FLAGS) -c $< -o $@
-include $(LIBJS_RUNTIME_OBJS:.o=.d)
LIBJS_SO := $(BIN)/libjs.so
LIBJS_BASE := $(patsubst libjs.so=%,%,$(filter libjs.so=%,$(LIB_BASE_PAIRS)))
# The runtime library requires libmath's real target library; a host libm
# cannot satisfy its freestanding link. The default build reaches it through
# the js runner (apps/js/runner.mk); `make js-library` builds it alone.
$(LIBJS_SO): $(LIBJS_CORE) $(LIBJS_RUNTIME_OBJS) $(LIBOS64_SO) $(BIN)/libmath.so libjs/exports.map link/lib.ld \
    GNUmakefile libjs/shared.mk tools/app_bases.py
	$(LD) --defsym LIB_BASE=$(LIBJS_BASE) --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) \
	    $(SHARED_LIB_LDFLAGS) -soname libjs.so --no-undefined --no-as-needed \
	    --version-script=libjs/exports.map -Map=$(OBJ)/js/link.map \
	    -o $@ $(LIBJS_CORE) $(LIBJS_RUNTIME_OBJS) $(BIN)/libmath.so $(LIBOS64_SO)
.PHONY: js-library
js-library: $(LIBJS_SO)

# The library fixture uses an explicit target and a reserved application slot.
# It is installed only in validation images.
LIBJS_TEST_ELF := $(OBJ)/js/runtime-core-test
LIBJS_TEST_OBJ := $(OBJ)/js/runtime-core-test.o
$(LIBJS_TEST_OBJ): libjs/tests/runtime_core.c libjs/tests/cases.h $(LIBJS_GENERATED)/.prepared libjs/shared.mk libjs/core.mk
	$(CC) $(filter-out -fPIC,$(LIBJS_FLAGS)) -fno-pic -fno-pie -c $< -o $@
$(LIBJS_TEST_ELF): $(LIBJS_TEST_OBJ) $(LAUNCH_OBJS) $(LIBJS_SO) $(LIBOS64_SO) GNUmakefile libjs/shared.mk tools/app_bases.py
	$(LD) --defsym APP_BASE=$(APP_BASE_jsembedtest) --defsym APP_SLOT_SIZE=$(APP_SLOT_SIZE) \
	    $(LDFLAGS) -o $@ $(LIBJS_TEST_OBJ) $(LAUNCH_OBJS) $(LIBJS_SO) $(LIBOS64_SO)
.PHONY: js-runtime-test
js-runtime-test: $(LIBJS_TEST_ELF)
-include $(LIBJS_TEST_OBJ:.o=.d)
