#include "ssh_internal.h"

static int space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
/* 1 enrolled, 0 blank/comment, -1 unsupported type, -2 malformed/options,
 * -3 key capacity. Options are rejected as a whole line, never stripped. */
int ssh_authorized_line(ssh_engine *s, const char *line, size_t n)
{
    size_t i = 0; while (i < n && space(line[i])) i++;
    if (i == n || line[i] == '#') return 0;
    size_t start = i; while (i < n && !space(line[i])) i++;
    ssh_reader type = {(const uint8_t *)line + start, i-start, 0};
    if (!ssh_equal(type, SSH_KEY_TYPE)) {
        for (size_t k = start; k < i; k++) if (line[k] == '=' || line[k] == '"' || line[k] == ',') return -2;
        /* Recognizable OpenSSH key types may coexist in the same file.
         * Bare option names such as restrict must fail closed as options. */
        if ((type.n >= 4 && !memcmp(type.p, "ssh-", 4)) ||
            (type.n >= 6 && !memcmp(type.p, "ecdsa-", 6)) ||
            (type.n >= 3 && !memcmp(type.p, "sk-", 3))) return -1;
        return -2;
    }
    while (i < n && space(line[i])) i++;
    start = i; while (i < n && !space(line[i])) i++;
    uint8_t blob[SSH_KEY_BLOB_MAX], point[65];
    int len = ssh_base64_decode(line + start, i-start, blob, sizeof(blob));
    if (len < 0 || !ssh_parse_public(blob, (size_t)len, point)) return -2;
    for (size_t k = 0; k < s->key_count; k++)
        if (s->keys[k].len == (size_t)len && !memcmp(s->keys[k].blob, blob, (size_t)len)) return 1;
    if (s->key_count == SSH_KEYS_MAX) return -3;
    memcpy(s->keys[s->key_count].blob, blob, (size_t)len);
    s->keys[s->key_count++].len = (size_t)len; return 1;
}
static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
int ssh_private_parse(const char *text, size_t n, uint8_t private_key[32])
{
    if (n < 76 || memcmp(text, "ecdsa-p256 ", 11)) return 0;
    for (size_t i = 0; i < 32; i++) {
        int a = hex(text[11+i*2]), b = hex(text[12+i*2]);
        if (a < 0 || b < 0) goto bad;
        private_key[i] = (uint8_t)((a << 4) | b);
    }
    size_t i = 75;
    if (i < n && text[i] == '\r') i++;
    if (i == n || text[i++] != '\n') goto bad;
    while (i < n) {
        if (text[i] != '#') goto bad;
        while (i < n && text[i] != '\n') i++;
        if (i < n) i++;
    }
    uint8_t public_key[65];
    if (ssh_host_public(private_key, public_key)) return 1;
bad:
    ssh_wipe(private_key, 32); return 0;
}
