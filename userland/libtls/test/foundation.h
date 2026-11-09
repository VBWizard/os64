#ifndef OS64_BEARSSL_FOUNDATION_TEST_H
#define OS64_BEARSSL_FOUNDATION_TEST_H
#include "bearssl.h"

// Fixture-owned heap workspace; no production connection API is exposed.
typedef struct {
    br_ssl_client_context client;
    br_x509_minimal_context x509;
    unsigned char records[BR_SSL_BUFSIZE_BIDI];
    unsigned char data[4097];
} bearssl_test_workspace;

typedef void (*bearssl_test_report)(const char *line);
int bearssl_foundation_test(bearssl_test_workspace *w, int port_checks,
                           bearssl_test_report report);
const char *bearssl_test_license(void);
#endif
