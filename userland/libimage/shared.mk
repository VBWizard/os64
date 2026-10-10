LIBIMAGE_SO := $(BIN)/libimage.so
LIBIMAGE_BASE := $(patsubst libimage.so=%,%,$(filter libimage.so=%,$(LIB_BASE_PAIRS)))
LIBIMAGE_OBJ := $(OBJ)/pic/libimage/image.c.o $(OBJ)/pic/libimage/gif.c.o
# Expansion and composition run for each displayed GIF frame. Their host
# reference corpus also exercises the -O2 build; other image arms retain
# their own build flags.
$(OBJ)/pic/libimage/gif.c.o: private LIBOS64_CFLAGS += -O2
$(OBJ)/pic/libimage/%.c.o: libimage/%.c libimage/shared.mk
	@mkdir -p $(dir $@)
	$(CC) $(LIBOS64_CFLAGS) -c $< -o $@
# shared.mk holds the object list: removing a source creates no newer
# object, so without it the old library would look up to date.
$(LIBIMAGE_SO): $(LIBIMAGE_OBJ) $(LIBPNG_SO) $(LIBJPEG_SO) $(LIBWEBP_SO) $(LIBOS64_SO) link/lib.ld tools/app_bases.py libimage/exports.map \
        libimage/shared.mk
	$(LD) --defsym LIB_BASE=$(LIBIMAGE_BASE) --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) \
	    $(SHARED_LIB_LDFLAGS) -soname libimage.so --no-undefined --no-as-needed \
	    --version-script=libimage/exports.map -rpath-link $(BIN) \
	    -o $@ $(LIBIMAGE_OBJ) $(LIBPNG_SO) $(LIBJPEG_SO) $(LIBWEBP_SO) $(LIBOS64_SO)
-include $(LIBIMAGE_OBJ:.o=.d)
