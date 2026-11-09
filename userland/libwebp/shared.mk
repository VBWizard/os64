include libwebp/sources.mk
LIBWEBP_SO := $(BIN)/libwebp.so
LIBWEBP_BASE := $(patsubst libwebp.so=%,%,$(filter libwebp.so=%,$(LIB_BASE_PAIRS)))
WEBP_OBJS := $(patsubst %,$(OBJ)/webp/%.o,$(WEBP_CORE_SRCS) $(WEBP_SSE2_SRCS) libwebp/port/cpu.c libwebp/port/decode.c)
WEBP_GENERATED := $(OBJ)/webp/generated
WEBP_FLAGS := $(LIBOS64_CFLAGS) -O2 -std=c11 -fvisibility=hidden -fno-builtin \
    -ffunction-sections -fdata-sections -msse2 -mno-avx -DHAVE_CONFIG_H \
    -Ilibwebp/port/compat -Ilibwebp/port -Ilibwebp/upstream -I$(WEBP_GENERATED)
$(WEBP_GENERATED)/alpha_dec.c $(WEBP_GENERATED)/webp_license.h &: ../tools/webp_generate.py \
        libwebp/port/alpha_dec.patch libwebp/upstream/src/dec/alpha_dec.c ../license/libwebp-LICENSE
	python3 ../tools/webp_generate.py $(WEBP_GENERATED)
$(OBJ)/webp/libwebp/%.c.o: libwebp/%.c libwebp/shared.mk libwebp/port/src/webp/config.h $(WEBP_GENERATED)/webp_license.h
	@mkdir -p $(dir $@)
	$(CC) $(WEBP_FLAGS) -c $< -o $@
$(OBJ)/webp/libwebp/upstream/src/dec/alpha_dec.c.o: $(WEBP_GENERATED)/alpha_dec.c libwebp/shared.mk libwebp/port/src/webp/config.h
	@mkdir -p $(dir $@)
	$(CC) $(WEBP_FLAGS) -c $< -o $@
# Shared utilities contain encoder helpers. Section GC keeps the decoder
# dependency closure; source lists are prerequisites so removal relinks it.
$(LIBWEBP_SO): $(WEBP_OBJS) $(LIBOS64_SO) libwebp/exports.map link/lib.ld tools/app_bases.py \
        libwebp/sources.mk libwebp/shared.mk
	$(LD) --defsym LIB_BASE=$(LIBWEBP_BASE) --defsym LIB_SLOT_SIZE=$(LIB_SLOT_SIZE) \
	    $(SHARED_LIB_LDFLAGS) --gc-sections -soname libwebp.so --no-undefined --no-as-needed \
	    --version-script=libwebp/exports.map -o $@ $(WEBP_OBJS) $(LIBOS64_SO)
-include $(WEBP_OBJS:.o=.d)

# Two independent benchmark executables keep their DSP state private. The
# wrapper's clocks are compiled out of libwebp.so; no live dispatch toggle.
define WEBP_BENCH_RULE
WEBP_BENCH_OBJS_$(1) := $$(patsubst %,$$(OBJ)/webp-bench-$(1)/%.o,$$(WEBP_CORE_SRCS) $(if $(filter sse2,$(1)),$(WEBP_SSE2_SRCS)) libwebp/port/cpu.c libwebp/port/decode.c)
$$(OBJ)/webp-bench-$(1)/libwebp/%.c.o: libwebp/%.c libwebp/shared.mk libwebp/port/bench.h libwebp/port/src/webp/config.h $$(WEBP_GENERATED)/webp_license.h
	@mkdir -p $$(dir $$@)
	$$(CC) $$(WEBP_FLAGS) -DOS64_WEBP_BENCH $(if $(filter scalar,$(1)),-DOS64_WEBP_SCALAR) -c $$< -o $$@
$$(OBJ)/webp-bench-$(1)/libwebp/upstream/src/dec/alpha_dec.c.o: $$(WEBP_GENERATED)/alpha_dec.c libwebp/shared.mk libwebp/port/src/webp/config.h
	@mkdir -p $$(dir $$@)
	$$(CC) $$(WEBP_FLAGS) -DOS64_WEBP_BENCH $(if $(filter scalar,$(1)),-DOS64_WEBP_SCALAR) -c $$< -o $$@
$$(OBJ)/webp-bench-$(1)/decoder.o: $$(WEBP_BENCH_OBJS_$(1)) libwebp/sources.mk libwebp/shared.mk
	$$(LD) -r --gc-sections -u webp_bench_decode -u os64_webp_free -u os64_webp_status_name -o $$@ $$(WEBP_BENCH_OBJS_$(1))
-include $$(WEBP_BENCH_OBJS_$(1):.o=.d)
endef
$(foreach mode,sse2 scalar,$(eval $(call WEBP_BENCH_RULE,$(mode))))
$(OBJ)/tests/webpbench/webpbench.c.o $(OBJ)/tests/webpbenchscalar/webpbenchscalar.c.o: CFLAGS += -O2
