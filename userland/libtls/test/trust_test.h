#ifndef OS64_TLS_TRUST_TEST_H
#define OS64_TLS_TRUST_TEST_H
#include "../port/trust_store.h"

int tls_trust_selftest(void (*report)(const char *));
bool tls_trust_test_verify(os64_tls_trust *store, bool root_b);
#endif
