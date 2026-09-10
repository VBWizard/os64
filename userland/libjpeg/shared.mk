include libjpeg/sources.mk
LIBJPEG_SO := $(BIN)/libjpeg.so
LIBJPEG_BASE := $(patsubst libjpeg.so=%,%,$(filter libjpeg.so=%,$(LIB_BASE_PAIRS)))
JPEG_OBJS := $(patsubst %,$(OBJ)/jpeg/%.o,$(JPEG_CORE_SRCS) libjpeg/port/decode.c)
JPEG_FLAGS := $(LIBOS64_CFLAGS) -O2 -std=c11 -fvisibility=hidden -fno-builtin \
    -Ilibjpeg/port/compat -Ilibjpeg/port -Ilibjpeg/upstream -Ilibjpeg/include \
    -I$(OBJ)/jpeg -include libjpeg/port/config.h
JPEG_LICENSE := $(OBJ)/jpeg/jpeg_license.h
$(JPEG_LICENSE): ../tools/jpeg_license.py libjpeg/upstream/LICENSE.md libjpeg/upstream/README.ijg
	@mkdir -p $(dir $@)
	python3 ../tools/jpeg_license.py $@
$(OBJ)/jpeg/libjpeg/%.c.o: libjpeg/%.c libjpeg/shared.mk libjpeg/port/config.h $(JPEG_LICENSE)
	@mkdir -p $(dir $@)
	$(CC) $(JPEG_FLAGS) $(if $(findstring /upstream/,$<),-Wno-unused-parameter -Wno-sign-compare) -c $< -o $@
$(LIBJPEG_SO): $(JPEG_OBJS) $(LIBOS64_SO) libjpeg/exports.map link/lib.ld tools/app_bases.py
	$(LD) --defsym LIB_BASE=$(LIBJPEG_BASE) --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) \
	    $(SHARED_LIB_LDFLAGS) -soname libjpeg.so --no-undefined --no-as-needed \
	    --version-script=libjpeg/exports.map -o $@ $(JPEG_OBJS) $(LIBOS64_SO)
-include $(JPEG_OBJS:.o=.d)
