LIBIMAGE_SO := $(BIN)/libimage.so
LIBIMAGE_BASE := $(patsubst libimage.so=%,%,$(filter libimage.so=%,$(LIB_BASE_PAIRS)))
LIBIMAGE_OBJ := $(OBJ)/pic/libimage/image.c.o
$(LIBIMAGE_OBJ): libimage/image.c libimage/shared.mk
	@mkdir -p $(dir $@)
	$(CC) $(LIBOS64_CFLAGS) -c $< -o $@
$(LIBIMAGE_SO): $(LIBIMAGE_OBJ) $(LIBPNG_SO) $(LIBJPEG_SO) $(LIBOS64_SO) link/lib.ld tools/app_bases.py libimage/exports.map
	$(LD) --defsym LIB_BASE=$(LIBIMAGE_BASE) --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) \
	    $(SHARED_LIB_LDFLAGS) -soname libimage.so --no-undefined --no-as-needed \
	    --version-script=libimage/exports.map -rpath-link $(BIN) \
	    -o $@ $(LIBIMAGE_OBJ) $(LIBPNG_SO) $(LIBJPEG_SO) $(LIBOS64_SO)
-include $(LIBIMAGE_OBJ:.o=.d)
