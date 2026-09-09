#include "trust_store.h"
#include "os64/mem.h"
#include "os64/str.h"

static bool setting(const unsigned char *p, size_t n)
{
    static const char key[] = "trust_store";
    if (n != sizeof key - 1) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned ch = p[i];
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        if (ch != (unsigned char)key[i]) return false;
    }
    return true;
}
tls_store_status os64_tls_trust_config_parse(const void *text, size_t length,
                                            char *path, size_t capacity,
                                            tls_store_detail *detail)
{
    tls_store_detail local = {0};
    if (!detail) detail = &local;
    *detail = (tls_store_detail){.line = 1};
    if (path && capacity) path[0] = 0;
    if (!path || !capacity || (length && !text)) return detail->status = TLS_STORE_BAD_ARGUMENT;
    if (length > TLS_STORE_CONFIG_MAX) return detail->status = TLS_STORE_LIMIT;
    const unsigned char *data = text;
    char selected[TLS_STORE_PATH_MAX] = TLS_STORE_DEFAULT;
    size_t at = 0;
    while (at < length) {
        size_t start = at;
        while (at < length && data[at] != '\n') at++;
        size_t end = at;
        if (at < length) at++;
        if (end > start && data[end - 1] == '\r') end--;
        // Inspect the full line, including comments, so NUL cannot hide a
        // suffix that another reader or an editor would consider present.
        for (size_t i = start; i < end; i++)
            if ((data[i] < 32 && data[i] != '\t') || data[i] == 127) return detail->status = TLS_STORE_CONFIG;
        for (size_t i = start; i < end; i++) if (data[i] == '#') { end = i; break; }
        while (start < end && (data[start] == ' ' || data[start] == '\t')) start++;
        while (end > start && (data[end - 1] == ' ' || data[end - 1] == '\t')) end--;
        if (start != end) {
            size_t key_end = start;
            while (key_end < end && data[key_end] != '=' && data[key_end] != ' ' && data[key_end] != '\t') key_end++;
            if (!setting(data + start, key_end - start)) return detail->status = TLS_STORE_CONFIG;
            size_t value = key_end;
            while (value < end && (data[value] == ' ' || data[value] == '\t')) value++;
            if (value == end || data[value++] != '=') return detail->status = TLS_STORE_CONFIG;
            while (value < end && (data[value] == ' ' || data[value] == '\t')) value++;
            if (value == end || data[value] != '/') return detail->status = TLS_STORE_CONFIG;
            size_t size = end - value;
            if (size >= sizeof selected) return detail->status = TLS_STORE_LIMIT;
            os64_memcpy(selected, data + value, size); selected[size] = 0;
        }
        detail->line++;
    }
    size_t n = os64_strlen(selected);
    if (n >= capacity) return detail->status = TLS_STORE_LIMIT;
    os64_memcpy(path, selected, n + 1);
    return detail->status = TLS_STORE_OK;
}
const char *os64_tls_store_status_name(tls_store_status status)
{
    switch (status) {
    case TLS_STORE_OK: return "ok";
    case TLS_STORE_BAD_ARGUMENT: return "bad argument";
    case TLS_STORE_NO_MEMORY: return "out of memory";
    case TLS_STORE_LIMIT: return "size limit";
    case TLS_STORE_CONFIG: return "invalid config";
    case TLS_STORE_FORMAT: return "invalid PEM";
    case TLS_STORE_CERTIFICATE: return "certificate policy refusal";
    case TLS_STORE_OPEN: return "could not open";
    case TLS_STORE_IO: return "I/O error";
    }
    return "unknown";
}
