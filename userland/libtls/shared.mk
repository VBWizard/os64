# Public objects have their own visibility policy; keep the private archive
# suitable for auditing and fixture injection without exporting those hooks.
LIBTLS_SO := $(BIN)/libtls.so
LIBTLS_BASE := $(patsubst libtls.so=%,%,$(filter libtls.so=%,$(LIB_BASE_PAIRS)))
include libtls/public_sources.mk
LIBTLS_CORE_OBJS := $(patsubst %,$(OBJ)/tls-shared/%.o,$(LIBTLS_CORE_SOURCES))
LIBTLS_ADAPTER_SRCS := libtls/api.c libtls/transport.c libtls/port/platform_inputs.c \
                      libtls/port/trust_pem.c libtls/port/trust_config.c libtls/port/trust_file.c
LIBTLS_ADAPTER_OBJS := $(patsubst %,$(OBJ)/tls-shared/%.o,$(LIBTLS_ADAPTER_SRCS))
LIBTLS_ARCHIVE := $(OBJ)/tls-shared/core.a
LIBTLS_LICENSE := $(OBJ)/tls-shared/tls_license.h

$(LIBTLS_LICENSE): libtls/upstream/LICENSE.txt ../tools/tls_license.py
	@mkdir -p "$(dir $@)"
	python3 ../tools/tls_license.py $@

$(OBJ)/tls-shared/libtls/%.c.o: libtls/%.c libtls/port/config.h libtls/foundation.mk libtls/shared.mk
	@mkdir -p "$(dir $@)"
	$(CC) $(BEARSSL_FLAGS) -DOS64_TLS_BUILD_PUBLIC -I$(OBJ)/tls-shared -c $< -o $@

$(OBJ)/tls-shared/libtls/api.c.o: $(LIBTLS_LICENSE)
$(LIBTLS_ARCHIVE): $(LIBTLS_CORE_OBJS) libtls/public_sources.mk libtls/shared.mk
	rm -f $@
	x86_64-elf-ar rcs $@ $(LIBTLS_CORE_OBJS)

$(LIBTLS_SO): $(LIBTLS_ADAPTER_OBJS) $(LIBTLS_ARCHIVE) $(LIBOS64_SO) \
              libtls/exports.map link/lib.ld tools/app_bases.py libtls/shared.mk
	@mkdir -p "$(BIN)"
	$(LD) --defsym LIB_BASE=$(LIBTLS_BASE) --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) \
	      $(SHARED_LIB_LDFLAGS) -soname libtls.so --no-undefined --no-as-needed \
	      --version-script=libtls/exports.map -Map=$(OBJ)/tls-shared/link.map \
	      -o $@ $(LIBTLS_ADAPTER_OBJS) $(LIBTLS_ARCHIVE) $(LIBOS64_SO)

-include $(LIBTLS_CORE_OBJS:.o=.d) $(LIBTLS_ADAPTER_OBJS:.o=.d)
