include libjs/core.mk
LIBJS_SO := $(BIN)/libjs.so
LIBJS_BASE := $(patsubst libjs.so=%,%,$(filter libjs.so=%,$(LIB_BASE_PAIRS)))
# R2 adds the os64 runtime API. This explicit engine target requires libmath's
# real target library; a host libm cannot satisfy its freestanding link.
$(LIBJS_SO): $(LIBJS_CORE) $(LIBOS64_SO) $(BIN)/libmath.so libjs/exports.map link/lib.ld \
    GNUmakefile libjs/shared.mk tools/app_bases.py
	$(LD) --defsym LIB_BASE=$(LIBJS_BASE) --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) \
	    $(SHARED_LIB_LDFLAGS) -soname libjs.so --no-undefined --no-as-needed \
	    --version-script=libjs/exports.map -Map=$(OBJ)/js/link.map \
	    -o $@ $(LIBJS_CORE) $(BIN)/libmath.so $(LIBOS64_SO)
.PHONY: js-library
js-library: $(LIBJS_SO)
