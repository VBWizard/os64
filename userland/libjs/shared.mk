include libjs/core.mk
LIBJS_RUNTIME_OBJS := $(OBJ)/js/runtime/runtime.o $(OBJ)/js/runtime/owner_tickets.o $(OBJ)/js/runtime/owner_incarnation.o $(OBJ)/js/intl/intl.o
# This private metadata unit needs no prepared engine headers. The standalone
# target oracle links the same hidden object without loading libjs/QuickJS.
$(OBJ)/js/runtime/owner_tickets.o: libjs/runtime/owner_tickets.c libjs/runtime/owner_tickets.h \
    libjs/include/os64/js.h libjs/shared.mk GNUmakefile
	@mkdir -p "$(@D)"
	$(CC) $(LIBJS_FLAGS) -c $< -o $@
$(OBJ)/js/runtime/owner_incarnation.o: libjs/runtime/owner_incarnation.c libjs/runtime/owner_incarnation.h \
    libjs/runtime/owner_tickets.h libjs/include/os64/js.h libjs/shared.mk GNUmakefile
	@mkdir -p "$(@D)"
	$(CC) $(LIBJS_FLAGS) -c $< -o $@
$(OBJ)/js/runtime/%.o: libjs/runtime/%.c $(LIBJS_GENERATED)/.prepared libjs/shared.mk libjs/core.mk
	@mkdir -p "$(@D)"
	$(CC) $(LIBJS_FLAGS) -c $< -o $@
# Intl (INTL.md): the script and its CLDR data are carried in intl.o's
# rodata (.incbin), so the object depends on both; the segmentation
# (SEGMENTER.md) is compiled into it, with its generated table.
$(OBJ)/js/intl/intl.o: libjs/intl/intl.c libjs/intl/intl.js libjs/intl/intl_data.js libjs/intl/zones.inc \
    libjs/intl/segment.c libjs/intl/segment.h libjs/intl/segment_data.inc \
    $(LIBJS_GENERATED)/.prepared libjs/shared.mk libjs/core.mk
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

# Private metadata oracle: same object as libjs, no engine/runtime constructor.
$(OBJ)/js/owner-tickets-test.o: libjs/tests/owner_tickets.c libjs/tests/owner_ticket_cases.h \
    libjs/runtime/owner_tickets.h libjs/shared.mk GNUmakefile
	@mkdir -p "$(@D)"
	$(CC) $(filter-out -fPIC,$(LIBJS_FLAGS)) -fno-pic -fno-pie -c $< -o $@
$(OBJ)/js/owner-tickets-test: $(OBJ)/js/owner-tickets-test.o $(OBJ)/js/runtime/owner_tickets.o $(LAUNCH_OBJS) $(LIBOS64_SO)
	$(LD) --defsym APP_BASE=$(APP_BASE_jsembedtest) --defsym APP_SLOT_SIZE=$(APP_SLOT_SIZE) \
	    $(LDFLAGS) $(BIG_STACK_LDFLAGS) -o $@ $^
$(OBJ)/js/owner-tickets-parent.o: libjs/tests/owner_tickets_parent.c libjs/shared.mk GNUmakefile
	@mkdir -p "$(@D)"
	$(CC) $(filter-out -fPIC,$(LIBJS_FLAGS)) -fno-pic -fno-pie -c $< -o $@
$(OBJ)/js/owner-tickets-parent: $(OBJ)/js/owner-tickets-parent.o $(LAUNCH_OBJS) $(LIBOS64_SO)
	$(LD) --defsym APP_BASE=$(APP_BASE_testrun) --defsym APP_SLOT_SIZE=$(APP_SLOT_SIZE) \
	    $(LDFLAGS) -o $@ $^
.PHONY: js-owner-tickets-test
js-owner-tickets-test: $(OBJ)/js/owner-tickets-test $(OBJ)/js/owner-tickets-parent
-include $(OBJ)/js/owner-tickets-test.d $(OBJ)/js/owner-tickets-parent.d

# Private incarnation oracle, using the production issuer object directly.
$(OBJ)/js/owner-incarnation-test.o: libjs/tests/owner_incarnation.c libjs/tests/owner_incarnation_cases.h \
    libjs/runtime/owner_incarnation.h libjs/shared.mk GNUmakefile
	@mkdir -p "$(@D)"
	$(CC) $(filter-out -fPIC,$(LIBJS_FLAGS)) -fno-pic -fno-pie -c $< -o $@
$(OBJ)/js/owner-incarnation-test: $(OBJ)/js/owner-incarnation-test.o $(OBJ)/js/runtime/owner_incarnation.o $(LAUNCH_OBJS) $(LIBOS64_SO)
	$(LD) --defsym APP_BASE=$(APP_BASE_jsembedtest) --defsym APP_SLOT_SIZE=$(APP_SLOT_SIZE) \
	    $(LDFLAGS) -o $@ $^
$(OBJ)/js/owner-incarnation-parent.o: libjs/tests/owner_incarnation_parent.c libjs/shared.mk GNUmakefile
	@mkdir -p "$(@D)"
	$(CC) $(filter-out -fPIC,$(LIBJS_FLAGS)) -fno-pic -fno-pie -c $< -o $@
$(OBJ)/js/owner-incarnation-parent: $(OBJ)/js/owner-incarnation-parent.o $(LAUNCH_OBJS) $(LIBOS64_SO)
	$(LD) --defsym APP_BASE=$(APP_BASE_testrun) --defsym APP_SLOT_SIZE=$(APP_SLOT_SIZE) \
	    $(LDFLAGS) -o $@ $^
.PHONY: js-owner-incarnation-test
js-owner-incarnation-test: $(OBJ)/js/owner-incarnation-test $(OBJ)/js/owner-incarnation-parent
-include $(OBJ)/js/owner-incarnation-test.d $(OBJ)/js/owner-incarnation-parent.d

# Private runtime identity oracle links the production runtime and engine
# objects directly, so hidden inspection does not become a public export.
$(OBJ)/js/owner-runtime-test.o: libjs/tests/owner_runtime.c libjs/tests/owner_runtime_cases.h \
    libjs/runtime/owner_runtime.h $(LIBJS_GENERATED)/.prepared libjs/shared.mk GNUmakefile
	@mkdir -p "$(@D)"
	$(CC) $(filter-out -fPIC,$(LIBJS_FLAGS)) -fno-pic -fno-pie -c $< -o $@
$(OBJ)/js/owner-runtime-test: $(OBJ)/js/owner-runtime-test.o $(LIBJS_CORE) $(LIBJS_RUNTIME_OBJS) $(LAUNCH_OBJS) $(BIN)/libmath.so $(LIBOS64_SO)
	$(LD) --defsym APP_BASE=$(APP_BASE_jsembedtest) --defsym APP_SLOT_SIZE=$(APP_SLOT_SIZE) \
	    $(LDFLAGS) $(BIG_STACK_LDFLAGS) -o $@ $^
$(OBJ)/js/owner-runtime-parent.o: libjs/tests/owner_runtime_parent.c libjs/shared.mk GNUmakefile
	@mkdir -p "$(@D)"
	$(CC) $(filter-out -fPIC,$(LIBJS_FLAGS)) -fno-pic -fno-pie -c $< -o $@
$(OBJ)/js/owner-runtime-parent: $(OBJ)/js/owner-runtime-parent.o $(LAUNCH_OBJS) $(LIBOS64_SO)
	$(LD) --defsym APP_BASE=$(APP_BASE_testrun) --defsym APP_SLOT_SIZE=$(APP_SLOT_SIZE) \
	    $(LDFLAGS) -o $@ $^
.PHONY: js-owner-runtime-test
js-owner-runtime-test: $(OBJ)/js/owner-runtime-test $(OBJ)/js/owner-runtime-parent
-include $(OBJ)/js/owner-runtime-test.d $(OBJ)/js/owner-runtime-parent.d

# V1 is a separate consumer alternative in the reserved fixture slot. Neither
# fixture belongs to the normal image; validation images select one executable.
LIBJS_ACCEPTANCE_DIR := $(OBJ)/js/acceptance
LIBJS_ACCEPTANCE_ELF := $(LIBJS_ACCEPTANCE_DIR)/consumer
LIBJS_ACCEPTANCE_HEADER := $(LIBJS_ACCEPTANCE_DIR)/language.h
$(LIBJS_ACCEPTANCE_HEADER): ../tools/js_acceptance/prepare.py $(wildcard ../tools/js_acceptance/upstream/*)
	@mkdir -p "$(@D)"
	python3 ../tools/js_acceptance/prepare.py $@
$(LIBJS_ACCEPTANCE_DIR)/consumer.o: ../tools/js_acceptance/consumer.c $(LIBJS_ACCEPTANCE_HEADER) $(LIBJS_GENERATED)/.prepared libjs/shared.mk libjs/core.mk
	$(CC) $(filter-out -fPIC,$(LIBJS_FLAGS)) -I$(LIBJS_ACCEPTANCE_DIR) -fno-pic -fno-pie -c $< -o $@
$(LIBJS_ACCEPTANCE_ELF): $(LIBJS_ACCEPTANCE_DIR)/consumer.o $(LAUNCH_OBJS) $(LIBJS_SO) $(LIBOS64_SO) GNUmakefile libjs/shared.mk tools/app_bases.py
	$(LD) --defsym APP_BASE=$(APP_BASE_jsembedtest) --defsym APP_SLOT_SIZE=$(APP_SLOT_SIZE) \
	    $(LDFLAGS) -o $@ $(LIBJS_ACCEPTANCE_DIR)/consumer.o $(LAUNCH_OBJS) $(LIBJS_SO) $(LIBOS64_SO)
.PHONY: js-acceptance-test
js-acceptance-test: $(LIBJS_ACCEPTANCE_ELF)
-include $(LIBJS_ACCEPTANCE_DIR)/consumer.d

# J2's guest measurements select the same reserved slot in their own process.
LIBJS_MEASURE_DIR := $(OBJ)/js/measure
LIBJS_MEASURE_ELF := $(LIBJS_MEASURE_DIR)/consumer
LIBJS_MEASURE_OBJS := $(LIBJS_MEASURE_DIR)/consumer.o $(LIBJS_MEASURE_DIR)/fp_pause.o
$(LIBJS_MEASURE_DIR)/consumer.o: ../tools/js_measure/consumer.c $(LIBJS_GENERATED)/.prepared libjs/shared.mk libjs/core.mk
	@mkdir -p "$(@D)"
	$(CC) $(filter-out -fPIC,$(LIBJS_FLAGS)) -fno-pic -fno-pie -c $< -o $@
$(LIBJS_MEASURE_DIR)/fp_pause.o: ../tools/js_measure/fp_pause.S libjs/shared.mk
	@mkdir -p "$(@D)"
	$(CC) $(CFLAGS) -c $< -o $@
$(LIBJS_MEASURE_ELF): $(LIBJS_MEASURE_OBJS) $(LAUNCH_OBJS) $(LIBJS_SO) $(LIBOS64_SO) GNUmakefile libjs/shared.mk tools/app_bases.py
	$(LD) --defsym APP_BASE=$(APP_BASE_jsembedtest) --defsym APP_SLOT_SIZE=$(APP_SLOT_SIZE) \
	    $(LDFLAGS) -o $@ $(LIBJS_MEASURE_OBJS) $(LAUNCH_OBJS) $(LIBJS_SO) $(LIBOS64_SO)
.PHONY: js-measure-test
js-measure-test: $(LIBJS_MEASURE_ELF)
-include $(LIBJS_MEASURE_OBJS:.o=.d)
