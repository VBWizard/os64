#include "trust_store.h"
#include "os64/mem.h"
#include "os64/str.h"

typedef struct {
    unsigned char der[TLS_CERTIFICATE_MAX], input[512], line[TLS_STORE_LINE_MAX];
    size_t der_length, line_length, quartet_length;
    unsigned quartet[4];
    bool in_certificate, padded;
} pem_workspace;

static int base64(unsigned c)
{
    if (c >= 'A' && c <= 'Z') return (int)c - 'A';
    if (c >= 'a' && c <= 'z') return (int)c - 'a' + 26;
    if (c >= '0' && c <= '9') return (int)c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return 64;
    return -1;
}
static bool matches(const unsigned char *p, size_t n, const char *s)
{
    if (n != os64_strlen(s)) return false;
    for (size_t i = 0; i < n; i++) if (p[i] != (unsigned char)s[i]) return false;
    return true;
}
static tls_store_status body(pem_workspace *w, const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] == ' ' || p[i] == '\t') continue;
        int value = base64(p[i]);
        if (value < 0 || w->padded) return TLS_STORE_FORMAT;
        w->quartet[w->quartet_length++] = (unsigned)value;
        if (w->quartet_length != 4) continue;
        unsigned *q = w->quartet;
        if (q[0] == 64 || q[1] == 64 || (q[2] == 64 && (q[3] != 64 || (q[1] & 15))) ||
            (q[2] != 64 && q[3] == 64 && (q[2] & 3))) return TLS_STORE_FORMAT;
        size_t count = q[2] == 64 ? 1 : q[3] == 64 ? 2 : 3;
        if (count > sizeof w->der - w->der_length) return TLS_STORE_LIMIT;
        w->der[w->der_length++] = (unsigned char)((q[0] << 2) | (q[1] >> 4));
        if (count > 1) w->der[w->der_length++] = (unsigned char)((q[1] << 4) | (q[2] >> 2));
        if (count > 2) w->der[w->der_length++] = (unsigned char)((q[2] << 6) | q[3]);
        w->padded = count != 3;
        w->quartet_length = 0;
    }
    return TLS_STORE_OK;
}
static tls_store_status line(pem_workspace *w, os64_tls_trust *store, tls_store_detail *detail)
{
    const unsigned char *p = w->line;
    size_t n = w->line_length;
    w->line_length = 0;
    if (n && p[n - 1] == '\r') n--;
    for (size_t i = 0; i < n; i++)
        if ((p[i] < 32 && p[i] != '\t') || p[i] > 126) return TLS_STORE_FORMAT;
    while (n && (*p == ' ' || *p == '\t')) { p++; n--; }
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    if (!n) return TLS_STORE_OK;
    if (!w->in_certificate) {
        // Explanations outside blocks carry no trust. Reserve hyphen-led lines
        // for armor so a malformed or truncated extra block cannot be ignored.
        if (!matches(p, n, "-----BEGIN CERTIFICATE-----"))
            return *p == '-' ? TLS_STORE_FORMAT : TLS_STORE_OK;
        w->in_certificate = true; w->der_length = 0;
        w->quartet_length = 0; w->padded = false;
        return TLS_STORE_OK;
    }
    if (matches(p, n, "-----END CERTIFICATE-----")) {
        if (!w->der_length || w->quartet_length) return TLS_STORE_FORMAT;
        tls_status status = os64_tls_trust_add_der(store, w->der, w->der_length, &detail->policy_reason);
        if (status == TLS_NO_MEMORY) return TLS_STORE_NO_MEMORY;
        if (status == TLS_LIMIT) return TLS_STORE_LIMIT;
        if (status != TLS_OK) return TLS_STORE_CERTIFICATE;
        detail->certificates++; w->in_certificate = false;
        return TLS_STORE_OK;
    }
    return body(w, p, n);
}
tls_store_status os64_tls_trust_load_pem(tls_store_reader read, void *context,
                                       os64_tls_trust **out, tls_store_detail *detail)
{
    tls_store_detail local = {0};
    if (!detail) detail = &local;
    *detail = (tls_store_detail){.line = 1};
    if (out) *out = NULL;
    if (!out || !read) return detail->status = TLS_STORE_BAD_ARGUMENT;
    pem_workspace *w = os64_malloc(sizeof *w);
    if (!w) return detail->status = TLS_STORE_NO_MEMORY;
    os64_memset(w, 0, sizeof *w);
    os64_tls_trust *store = NULL;
    tls_store_status status = TLS_STORE_NO_MEMORY;
    if (os64_tls_trust_create(&store) != TLS_OK) goto done;
    size_t total = 0;
    for (;;) {
        size_t capacity = TLS_STORE_PEM_MAX - total;
        if (capacity > sizeof w->input) capacity = sizeof w->input;
        // Probe past the input cap before accepting a complete prefix as EOF.
        int64_t n = read(context, w->input, capacity ? capacity : 1);
        if (n < 0 || (uint64_t)n > (capacity ? capacity : 1)) { status = TLS_STORE_IO; break; }
        if (!n) {
            status = w->line_length ? line(w, store, detail) : TLS_STORE_OK;
            if (status == TLS_STORE_OK && (w->in_certificate || !detail->certificates)) status = TLS_STORE_FORMAT;
            if (status == TLS_STORE_OK && os64_tls_trust_seal(store) != TLS_OK) status = TLS_STORE_CERTIFICATE;
            break;
        }
        if (!capacity) { status = TLS_STORE_LIMIT; break; }
        total += (size_t)n;
        for (size_t i = 0; i < (size_t)n; i++) {
            if (w->input[i] == '\n') {
                status = line(w, store, detail);
                if (status != TLS_STORE_OK) goto done;
                detail->line++;
            } else {
                if (w->line_length == sizeof w->line) { status = TLS_STORE_LIMIT; goto done; }
                w->line[w->line_length++] = w->input[i];
            }
        }
    }
done:
    if (status == TLS_STORE_OK) { *out = store; store = NULL; }
    os64_tls_trust_free(store);
    os64_free(w);
    detail->status = status;
    return status;
}
