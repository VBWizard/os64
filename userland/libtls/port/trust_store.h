#ifndef OS64_TLS_TRUST_STORE_H
#define OS64_TLS_TRUST_STORE_H
#include "certificate_policy.h"

typedef os64_tls_store_status_t tls_store_status;
typedef os64_tls_store_detail_t tls_store_detail;
typedef os64_tls_store_report_t tls_store_report;
typedef os64_tls_store_reader_t tls_store_reader;
#define TLS_STORE_BAD_ARGUMENT OS64_TLS_STORE_BAD_ARGUMENT
#define TLS_STORE_CERTIFICATE OS64_TLS_STORE_CERTIFICATE
#define TLS_STORE_CONFIG OS64_TLS_STORE_CONFIG
#define TLS_STORE_CONFIG_MAX OS64_TLS_STORE_CONFIG_MAX
#define TLS_STORE_DEFAULT OS64_TLS_STORE_DEFAULT
#define TLS_STORE_FORMAT OS64_TLS_STORE_FORMAT
#define TLS_STORE_IO OS64_TLS_STORE_IO
#define TLS_STORE_LIMIT OS64_TLS_STORE_LIMIT
#define TLS_STORE_LINE_MAX OS64_TLS_STORE_LINE_MAX
#define TLS_STORE_NO_MEMORY OS64_TLS_STORE_NO_MEMORY
#define TLS_STORE_OK OS64_TLS_STORE_OK
#define TLS_STORE_OPEN OS64_TLS_STORE_OPEN
#define TLS_STORE_PATH_MAX OS64_TLS_STORE_PATH_MAX
#define TLS_STORE_PEM_MAX OS64_TLS_STORE_PEM_MAX

// Private parser used by the OS config adapter and host fixtures.
tls_store_status os64_tls_trust_config_parse(const void *text, size_t length,
                                            char *path, size_t capacity,
                                            tls_store_detail *detail);
#endif
