# Private PIC archive for build auditing and TLS tests. No public TLS ABI.
# Normal builds compile the client and track its headers; archive extraction
# leaves unused client objects out of bearssltest.
TLS_CLIENT_SOURCES := libtls/port/client_engine.c libtls/port/client_profile.c \
                      libtls/port/certificate_policy.c libtls/port/certificate_der.c
include libtls/sources.mk
BEARSSL_OBJS := $(patsubst %,$(OBJ)/pic/%.o,$(BEARSSL_SOURCES) $(TLS_CLIENT_SOURCES)) \
                $(OBJ)/pic/libtls/port/runtime.c.o
BEARSSL_TEST_OBJ := $(OBJ)/libtls/test/foundation.c.o
BEARSSL_ARCHIVE := $(OBJ)/libbearssl-foundation.a
TLS_TRUST_TEST_OBJS := $(addprefix $(OBJ)/pic/libtls/port/,certificate_der.c.o \
                       certificate_policy.c.o trust_pem.c.o trust_config.c.o trust_file.c.o) \
                       $(OBJ)/pic/libtls/test/trust_test.c.o $(OBJ)/pic/libtls/test/trust_vectors.c.o
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
$(OBJ)/tests/tlstrusttest/%.c.o: CFLAGS += -O2 -I$(CURDIR)/libtls/upstream/inc -I$(CURDIR)/libtls/port/include
-include $(BEARSSL_OBJS:.o=.d)
-include $(TLS_TRUST_TEST_OBJS:.o=.d)

$(BEARSSL_TEST_OBJ): CFLAGS += -O2 -I$(CURDIR)/libtls/upstream/inc -I$(CURDIR)/libtls/port/include
-include $(BEARSSL_TEST_OBJ:.o=.d)
