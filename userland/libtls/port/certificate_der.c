#include "certificate_der.h"
#include "os64/mem.h"
#include "os64/str.h"

typedef tls_der_span span;
static bool equal(span a, span b)
{
    if (a.length != b.length) return false;
    for (size_t i = 0; i < a.length; i++) if (a.data[i] != b.data[i]) return false;
    return true;
}
static bool oid(span s, const char *bytes, size_t n)
{
    return equal(s, (span){(const unsigned char *)bytes, n});
}
#define OID(s, bytes) oid(s, bytes, sizeof(bytes) - 1)

// Bounds are checked before advancing; high-tag-number forms are outside the
// certificate subset. Nested readers cannot consume bytes in a sibling value.
static bool take(span *s, unsigned *tag, span *value)
{
    if (s->length < 2) return false;
    *tag = s->data[0];
    if (!*tag || (*tag & 31) == 31) return false;
    size_t n = s->data[1], header = 2;
    if (n & 128) {
        size_t count = n & 127;
        if (!count || count > 4 || count > s->length - 2 || !s->data[2]) return false;
        n = 0;
        for (size_t i = 0; i < count; i++) n = (n << 8) | s->data[header++];
        if (n < 128) return false;
    }
    if (n > s->length - header) return false;
    *value = (span){s->data + header, n};
    s->data += header + n; s->length -= header + n;
    return true;
}
static bool field(span *s, unsigned expected, span *value)
{
    unsigned tag;
    return take(s, &tag, value) && tag == expected;
}
static bool oid_valid(span s)
{
    if (!s.length) return false;
    bool first = true;
    for (size_t i = 0; i < s.length; i++) {
        if (first && s.data[i] == 0x80) return false;
        first = !(s.data[i] & 128);
    }
    return first;
}
static bool integer_valid(span s)
{
    if (!s.length) return false;
    return s.length == 1 || !((s.data[0] == 0 && !(s.data[1] & 128)) ||
        (s.data[0] == 255 && (s.data[1] & 128)));
}
static bool bits_valid(span s)
{
    return s.length && s.data[0] <= 7 &&
        (s.length > 1 ? !(s.data[s.length - 1] & ((1u << s.data[0]) - 1)) : !s.data[0]);
}
static bool structural(span s, unsigned depth, unsigned *nodes)
{
    if (depth > 16) return false;
    while (s.length) {
        unsigned tag; span v;
        if (++*nodes > 4096 || !take(&s, &tag, &v)) return false;
        if ((tag & 0xc0) == 0) {
            unsigned kind = tag & 31;
            if (((kind == 16 || kind == 17) != !!(tag & 32))) return false;
            if ((kind == 1 && (v.length != 1 || (v.data[0] != 0 && v.data[0] != 255))) ||
                (kind == 2 && !integer_valid(v)) || (kind == 3 && !bits_valid(v)) ||
                (kind == 5 && v.length) || (kind == 6 && !oid_valid(v))) return false;
        }
        if ((tag & 32) && !structural(v, depth + 1, nodes)) return false;
    }
    return true;
}
static bool positive(span *s, span *v)
{
    if (!field(s, 2, v) || !integer_valid(*v) || (v->data[0] & 128)) return false;
    if (v->length > 1 && !v->data[0]) { v->data++; v->length--; }
    return true;
}
static unsigned lower(unsigned c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static bool dns_valid(span s)
{
    if (!s.length || s.length > 253) return false;
    size_t start = s.length >= 2 && s.data[0] == '*' && s.data[1] == '.' ? 2 : 0;
    size_t label = 0;
    for (size_t i = start; i < s.length; i++) {
        unsigned c = lower(s.data[i]);
        if (c == '.') {
            if (!label || s.data[i - 1] == '-') return false;
            label = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
            if ((!label && c == '-') || ++label > 63) return false;
        }
    }
    return label && s.data[s.length - 1] != '-';
}
static bool dns_match(span name, const char *host)
{
    if (!host) return false;
    if (name.length >= 2 && name.data[0] == '*') {
        while (*host && *host != '.') host++;
        if (!*host) return false;
        host++; name.data += 2; name.length -= 2;
    }
    if (os64_strlen(host) != name.length) return false;
    for (size_t i = 0; i < name.length; i++) if (lower(name.data[i]) != lower((unsigned char)host[i])) return false;
    return true;
}
static tls_policy_reason san(span s, const char *hostname, bool *matched)
{
    span names;
    if (!field(&s, 0x30, &names) || s.length || !names.length) return TLS_POLICY_DER;
    while (names.length) {
        unsigned tag; span name;
        if (!take(&names, &tag, &name)) return TLS_POLICY_DER;
        if (tag == 0x82) {
            if (!dns_valid(name)) return TLS_POLICY_SAN;
            if (dns_match(name, hostname)) *matched = true;
        } else if (tag == 0x81 || tag == 0x86) {
            if (!name.length) return TLS_POLICY_DER;
            for (size_t i = 0; i < name.length; i++)
                if (!name.data[i] || name.data[i] > 127) return TLS_POLICY_DER;
        } else if (tag == 0x87) {
            if (name.length != 4 && name.length != 16) return TLS_POLICY_DER;
        } else if (tag == 0x88) {
            if (!oid_valid(name)) return TLS_POLICY_DER;
        } else if (tag == 0xa0 || tag == 0xa3 || tag == 0xa4 || tag == 0xa5) {
            // These constructed alternatives need schemas beyond this DNS
            // profile; structural DER alone cannot validate their contents.
            return TLS_POLICY_SAN;
        } else return TLS_POLICY_DER;
    }
    return TLS_POLICY_OK;
}
static tls_policy_reason eku(span s, unsigned role)
{
    span list, seen[32]; size_t count = 0; bool server = false;
    if (!field(&s, 0x30, &list) || s.length || !list.length) return TLS_POLICY_DER;
    while (list.length) {
        span value;
        if (count == 32) return TLS_POLICY_LIMIT;
        if (!field(&list, 6, &value) || !oid_valid(value)) return TLS_POLICY_DER;
        for (size_t i = 0; i < count; i++) if (equal(seen[i], value)) return TLS_POLICY_DUPLICATE;
        seen[count++] = value;
        if (OID(value, "\x2b\x06\x01\x05\x05\x07\x03\x01")) server = true;
    }
    return role == 2 || !server ? TLS_POLICY_EKU : TLS_POLICY_OK;
}
static tls_policy_reason basic(span s, unsigned role, bool *ca)
{
    span seq, v;
    if (!field(&s, 0x30, &seq) || s.length) return TLS_POLICY_DER;
    if (seq.length && seq.data[0] == 1) {
        // FALSE is the DEFAULT and must be omitted in DER.
        if (!field(&seq, 1, &v) || v.length != 1 || v.data[0] != 255) return TLS_POLICY_DER;
        *ca = true;
    }
    if (seq.length) {
        if (!positive(&seq, &v) || !*ca) return TLS_POLICY_DER;
        if (role == 2) return TLS_POLICY_ANCHOR;
    }
    if (seq.length) return TLS_POLICY_DER;
    return (*ca != (role != 0)) ? TLS_POLICY_CA : TLS_POLICY_OK;
}
static tls_policy_reason usage(span s, unsigned role)
{
    span bits;
    if (!field(&s, 3, &bits) || s.length || !bits_valid(bits) || bits.length < 2 || bits.length > 3)
        return TLS_POLICY_DER;
    unsigned last = bits.data[bits.length - 1];
    if (!last || !(last & (1u << bits.data[0])) || (bits.length == 3 && bits.data[0] != 7))
        return TLS_POLICY_DER;
    return bits.data[1] & (role ? 4 : 128) ? TLS_POLICY_OK : TLS_POLICY_KEY_USAGE;
}
static tls_policy_reason extensions(span s, unsigned role, const char *hostname, unsigned *nodes)
{
    span list, seen[32]; size_t count = 0; bool ca = false, matched = false;
    if (!field(&s, 0x30, &list) || s.length || !list.length) return TLS_POLICY_DER;
    while (list.length) {
        span ext, id, value; bool critical = false;
        if (count == 32) return TLS_POLICY_LIMIT;
        if (!field(&list, 0x30, &ext) || !field(&ext, 6, &id)) return TLS_POLICY_DER;
        for (size_t i = 0; i < count; i++) if (equal(id, seen[i])) return TLS_POLICY_DUPLICATE;
        seen[count++] = id;
        if (ext.length && ext.data[0] == 1) {
            if (!field(&ext, 1, &value) || value.length != 1 || value.data[0] != 255) return TLS_POLICY_DER;
            critical = true;
        }
        if (!field(&ext, 4, &value) || ext.length || !value.length) return TLS_POLICY_DER;
        if (!structural(value, 0, nodes)) return TLS_POLICY_DER;
        tls_policy_reason result;
        if (OID(id, "\x55\x1d\x13")) result = basic(value, role, &ca);
        else if (OID(id, "\x55\x1d\x0f")) result = usage(value, role);
        else if (OID(id, "\x55\x1d\x11")) result = san(value, hostname, &matched);
        else if (OID(id, "\x55\x1d\x25")) result = eku(value, role);
        else if (OID(id, "\x2b\x06\x01\x04\x01\xd6\x79\x02\x04\x02")) {
            // RFC 6962 SCT receipts are opaque metadata; this client makes no CT claim.
            span receipts;
            if (!field(&value, 4, &receipts) || value.length) result = TLS_POLICY_DER;
            else result = critical ? TLS_POLICY_CRITICAL : TLS_POLICY_OK;
        } else if (OID(id, "\x2b\x06\x01\x04\x01\x82\xda\x4b\x2c")) {
            // RFC 9345 permits delegation; the TLS 1.2 profile never negotiates it.
            span permission;
            if (!field(&value, 5, &permission) || permission.length || value.length) result = TLS_POLICY_DER;
            else result = critical ? TLS_POLICY_CRITICAL : TLS_POLICY_OK;
        } else {
            bool metadata = OID(id, "\x55\x1d\x0e") || OID(id, "\x55\x1d\x23") ||
                OID(id, "\x55\x1d\x1f") || OID(id, "\x55\x1d\x2e") || OID(id, "\x55\x1d\x20") ||
                OID(id, "\x2b\x06\x01\x05\x05\x07\x01\x01") || OID(id, "\x2b\x06\x01\x05\x05\x07\x01\x0b");
            result = !metadata ? TLS_POLICY_EXTENSION : critical ? TLS_POLICY_CRITICAL : TLS_POLICY_OK;
        }
        if (result != TLS_POLICY_OK) return result;
    }
    if (role && !ca) return TLS_POLICY_CA;
    return !role && !matched ? TLS_POLICY_SAN : TLS_POLICY_OK;
}
static bool signature_algorithm(span s, bool *ecdsa)
{
    span id, parameter;
    if (!field(&s, 6, &id)) return false;
    bool rsa = OID(id, "\x2a\x86\x48\x86\xf7\x0d\x01\x01\x0b") ||
        OID(id, "\x2a\x86\x48\x86\xf7\x0d\x01\x01\x0c") || OID(id, "\x2a\x86\x48\x86\xf7\x0d\x01\x01\x0d");
    bool ec = OID(id, "\x2a\x86\x48\xce\x3d\x04\x03\x02") ||
        OID(id, "\x2a\x86\x48\xce\x3d\x04\x03\x03") || OID(id, "\x2a\x86\x48\xce\x3d\x04\x03\x04");
    if (rsa && s.length && (!field(&s, 5, &parameter) || parameter.length)) return false;
    *ecdsa = ec;
    return (rsa || ec) && !s.length;
}
static bool ecdsa_signature_valid(span s)
{
    span sequence, value;
    if (!field(&s, 0x30, &sequence) || s.length) return false;
    // The BIT STRING contains DER here; BearSSL's signature converter also
    // accepts nonminimal encodings. Keep that leniency outside this profile.
    for (unsigned i = 0; i < 2; i++)
        if (!positive(&sequence, &value) || (value.length == 1 && !value.data[0])) return false;
    return !sequence.length;
}
static tls_policy_reason public_key(span s, br_x509_pkey *key)
{
    span algorithm, id, bits, v;
    if (!field(&s, 0x30, &algorithm) || !field(&algorithm, 6, &id) ||
        !field(&s, 3, &bits) || s.length || bits.length < 2 || bits.data[0]) return TLS_POLICY_DER;
    bits.data++; bits.length--;
    if (OID(id, "\x2a\x86\x48\x86\xf7\x0d\x01\x01\x01")) {
        span seq, n, e;
        if (algorithm.length && (!field(&algorithm, 5, &v) || v.length)) return TLS_POLICY_DER;
        if (algorithm.length || !field(&bits, 0x30, &seq) || bits.length ||
            !positive(&seq, &n) || !positive(&seq, &e) || seq.length) return TLS_POLICY_DER;
        unsigned top = n.data[0], topbits = 0;
        while (top) { topbits++; top >>= 1; }
        size_t bitcount = (n.length - 1) * 8 + topbits;
        if (bitcount < 2048 || bitcount > 4096 || !(n.data[n.length - 1] & 1) ||
            n.length + e.length > BR_X509_BUFSIZE_KEY || !(e.data[e.length - 1] & 1) ||
            (e.length == 1 && e.data[0] < 3)) return TLS_POLICY_KEY;
        key->key_type = BR_KEYTYPE_RSA;
        key->key.rsa = (br_rsa_public_key){(unsigned char *)n.data, n.length, (unsigned char *)e.data, e.length};
    } else if (OID(id, "\x2a\x86\x48\xce\x3d\x02\x01")) {
        if (!field(&algorithm, 6, &v) || algorithm.length) return TLS_POLICY_DER;
        int curve = OID(v, "\x2a\x86\x48\xce\x3d\x03\x01\x07") ? BR_EC_secp256r1 :
            OID(v, "\x2b\x81\x04\x00\x22") ? BR_EC_secp384r1 :
            OID(v, "\x2b\x81\x04\x00\x23") ? BR_EC_secp521r1 : 0;
        size_t expected = curve == BR_EC_secp256r1 ? 65 : curve == BR_EC_secp384r1 ? 97 : 133;
        if (!curve || bits.length != expected || bits.data[0] != 4) return TLS_POLICY_KEY;
        unsigned char point[133], scalar = 1;
        os64_memcpy(point, bits.data, bits.length);
        if (!br_ec_all_m31.mul(point, bits.length, &scalar, 1, curve)) return TLS_POLICY_KEY;
        key->key_type = BR_KEYTYPE_EC;
        key->key.ec = (br_ec_public_key){curve, (unsigned char *)bits.data, bits.length};
    } else return TLS_POLICY_KEY;
    return TLS_POLICY_OK;
}

static bool date(span *s, uint64_t *order)
{
    unsigned tag; span v;
    if (!take(s, &tag, &v) || (tag != 23 && tag != 24) ||
        v.length != (tag == 23 ? 13u : 15u) || v.data[v.length - 1] != 'Z') return false;
    unsigned parts[7] = {0}, at = 0;
    for (size_t i = 0; i + 1 < v.length; i += 2) {
        if (v.data[i] < '0' || v.data[i] > '9' || v.data[i + 1] < '0' || v.data[i + 1] > '9') return false;
        parts[at++] = (v.data[i] - '0') * 10 + v.data[i + 1] - '0';
    }
    unsigned year = tag == 23 ? parts[0] + (parts[0] < 50 ? 2000 : 1900) : parts[0] * 100 + parts[1];
    unsigned *p = parts + (tag == 23 ? 1 : 2);
    static const unsigned month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (!year || !p[0] || p[0] > 12 || !p[1] || p[2] > 23 || p[3] > 59 || p[4] > 59) return false;
    unsigned days = month_days[p[0] - 1] + (p[0] == 2 && !(year % 4) && (year % 100 || !(year % 400)));
    if (p[1] > days || (tag == 24 && year < 2050)) return false;
    *order = year;
    for (unsigned i = 0; i < 5; i++) *order = *order * 100 + p[i];
    return true;
}

static bool der_ordered(span previous, span current)
{
    size_t n = previous.length < current.length ? previous.length : current.length;
    for (size_t i = 0; i < n; i++)
        if (previous.data[i] != current.data[i]) return previous.data[i] < current.data[i];
    return previous.length <= current.length;
}
static bool scalar_valid(unsigned c)
{
    // NUL is outside the name profile, even in encodings that can represent it.
    return c && c <= 0x10ffff && !(c >= 0xd800 && c <= 0xdfff);
}
static bool name_string_valid(unsigned tag, span s)
{
    if (!s.length) return false;
    if (tag == 12) {
        for (size_t i = 0; i < s.length;) {
            unsigned c = s.data[i++], count = 0, minimum = 0;
            if (c >= 0xc2 && c <= 0xdf) { count = 1; minimum = 0x80; c &= 31; }
            else if (c >= 0xe0 && c <= 0xef) { count = 2; minimum = 0x800; c &= 15; }
            else if (c >= 0xf0 && c <= 0xf4) { count = 3; minimum = 0x10000; c &= 7; }
            else if (c >= 0x80) return false;
            if (count > s.length - i) return false;
            while (count--) {
                unsigned next = s.data[i++];
                if ((next & 0xc0) != 0x80) return false;
                c = (c << 6) | (next & 63);
            }
            if (c < minimum || !scalar_valid(c)) return false;
        }
        return true;
    }
    if (tag == 28 || tag == 30) {
        unsigned width = tag == 28 ? 4 : 2;
        if (s.length % width) return false;
        for (size_t i = 0; i < s.length; i += width) {
            unsigned c = 0;
            for (unsigned j = 0; j < width; j++) c = (c << 8) | s.data[i + j];
            // BMPString uses single BMP characters, not UTF-16 surrogate pairs.
            if (!scalar_valid(c)) return false;
        }
        return true;
    }
    if (tag != 18 && tag != 19 && tag != 22 && tag != 26) return false;
    for (size_t i = 0; i < s.length; i++) {
        unsigned c = s.data[i];
        bool digit = c >= '0' && c <= '9';
        if (tag == 18 && !(digit || c == ' ')) return false;
        if (tag == 19 && !(digit || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            c == ' ' || c == '\'' || c == '(' || c == ')' || c == '+' || c == ',' ||
            c == '-' || c == '.' || c == '/' || c == ':' || c == '=' || c == '?')) return false;
        if (tag == 22 && (!c || c > 127)) return false;
        if (tag == 26 && (c < 32 || c > 126)) return false;
    }
    return true;
}
static bool name_valid(span name)
{
    while (name.length) {
        span set;
        if (!field(&name, 0x31, &set) || !set.length) return false;
        // RDNs are SET OF: DER orders complete member encodings, not OIDs
        // or values. The enclosing RDNSequence retains its supplied order.
        span previous = {0};
        while (set.length) {
            span encoded = set, attribute, id, value; unsigned tag;
            if (!field(&set, 0x30, &attribute)) return false;
            encoded.length -= set.length;
            if (previous.length && !der_ordered(previous, encoded)) return false;
            previous = encoded;
            if (!field(&attribute, 6, &id) || !take(&attribute, &tag, &value) ||
                attribute.length || !name_string_valid(tag, value)) return false;
        }
    }
    return true;
}

tls_policy_reason os64_tls_certificate_inspect(const void *der, size_t length,
    unsigned role, const char *hostname, tls_certificate_view *view)
{
    if (!der || !length || !view || role > 2) return TLS_POLICY_DER;
    if (length > TLS_CERTIFICATE_MAX) return TLS_POLICY_LIMIT;
    span s = {der, length}, cert, tbs, inner, outer, issuer, subject, spki, v;
    unsigned nodes = 0;
    if (!structural(s, 0, &nodes) || !field(&s, 0x30, &cert) || s.length ||
        !field(&cert, 0x30, &tbs) || !field(&cert, 0x30, &outer) ||
        !field(&cert, 3, &v) || cert.length || v.length < 2 || v.data[0]) return TLS_POLICY_DER;
    span signature = {v.data + 1, v.length - 1};
    // The policy requires v3 because its role and identity rules use extensions.
    span version;
    if (!field(&tbs, 0xa0, &version) || !field(&version, 2, &v) || version.length ||
        v.length != 1 || v.data[0] != 2 || !positive(&tbs, &v) ||
        !field(&tbs, 0x30, &inner)) return TLS_POLICY_DER;
    bool ecdsa;
    if (!equal(inner, outer) || !signature_algorithm(inner, &ecdsa)) return TLS_POLICY_SIGNATURE;
    if (ecdsa && !ecdsa_signature_valid(signature)) return TLS_POLICY_DER;
    if (!field(&tbs, 0x30, &issuer) || !issuer.length || !name_valid(issuer) ||
        !field(&tbs, 0x30, &v)) return TLS_POLICY_DER;
    uint64_t before, after;
    if (!date(&v, &before) || !date(&v, &after) || v.length || before > after) return TLS_POLICY_DER;
    // Keep the full encoded subject Name for BearSSL's anchor DN hashing.
    span encoded_subject = tbs;
    if (!field(&tbs, 0x30, &subject) || (role && !subject.length) || !name_valid(subject)) return TLS_POLICY_DER;
    encoded_subject.length -= tbs.length;
    if (role == 2 && !equal(issuer, subject)) return TLS_POLICY_ANCHOR;
    if (!field(&tbs, 0x30, &spki)) return TLS_POLICY_DER;
    tls_policy_reason result = public_key(spki, &view->key);
    if (result != TLS_POLICY_OK) return result;
    // Unique IDs are outside this profile, so no ignored fields can hide an
    // additional extension container between SPKI and the expected [3].
    if (!field(&tbs, 0xa3, &v) || tbs.length) return TLS_POLICY_DER;
    result = extensions(v, role, hostname, &nodes);
    if (result == TLS_POLICY_OK) view->subject = encoded_subject;
    return result;
}
