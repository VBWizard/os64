# Private PIC archive for build auditing and bearssltest. No public TLS ABI.
include libtls/sources.mk
BEARSSL_OBJS := $(patsubst %,$(OBJ)/pic/%.o,$(BEARSSL_SOURCES)) \
                $(OBJ)/pic/libtls/port/runtime.c.o
BEARSSL_TEST_OBJ := $(OBJ)/libtls/test/foundation.c.o
BEARSSL_ARCHIVE := $(OBJ)/libbearssl-foundation.a
BEARSSL_FLAGS := $(filter-out -fno-pic -fno-pie,$(CFLAGS)) -O2 -fPIC \
                 -fvisibility=hidden -fno-builtin -fstack-usage \
                 -include $(CURDIR)/libtls/port/config.h \
                 -I$(CURDIR)/libtls/port/include \
                 -I$(CURDIR)/libtls/upstream/inc -I$(CURDIR)/libtls/upstream/src

$(OBJ)/pic/libtls/%.c.o: libtls/%.c libtls/port/config.h libtls/foundation.mk
	@mkdir -p "$(dir $@)"
	$(CC) $(BEARSSL_FLAGS) -c $< -o $@

$(BEARSSL_ARCHIVE): $(BEARSSL_OBJS) libtls/sources.mk libtls/foundation.mk
	@mkdir -p "$(dir $@)"
	rm -f $@
	x86_64-elf-ar rcs $@ $(BEARSSL_OBJS)

$(OBJ)/tests/bearssltest/%.c.o: CFLAGS += -O2 -I$(CURDIR)/libtls/upstream/inc -I$(CURDIR)/libtls/port/include
-include $(BEARSSL_OBJS:.o=.d)

$(BEARSSL_TEST_OBJ): CFLAGS += -O2 -I$(CURDIR)/libtls/upstream/inc -I$(CURDIR)/libtls/port/include
-include $(BEARSSL_TEST_OBJ:.o=.d)
