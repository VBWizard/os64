#include "trust_test.h"
#include "trust_vectors.h"
#include "os64/mem.h"
#include "os64/str.h"

typedef struct { const char *data; size_t length, at, fragment, fail_at; } memory_reader;
static int64_t read_memory(void *context, void *out, size_t capacity)
{
    memory_reader *r = context;
    if (r->at >= r->fail_at) return -1;
    size_t n = r->length - r->at;
    if (n > capacity) n = capacity;
    if (n > r->fragment) n = r->fragment;
    if (n > r->fail_at - r->at) n = r->fail_at - r->at;
    os64_memcpy(out, r->data + r->at, n); r->at += n;
    return (int64_t)n;
}
static tls_store_status parse(const char *data, size_t length, size_t fragment,
                              size_t fail_at, os64_tls_trust **out, tls_store_detail *detail)
{
    memory_reader reader = {data, length, 0, fragment, fail_at};
    return os64_tls_trust_load_pem(read_memory, &reader, out, detail);
}
bool tls_trust_test_verify(os64_tls_trust *store, bool root_b)
{
    tls_validator_factory f = os64_tls_policy_factory(store);
    const br_x509_class **v = NULL;
    if (f.create(store, "example.test", 740232, 0, &v) != TLS_OK) return false;
    const unsigned char *cert = root_b ? tls_fixture_b_leaf : tls_fixture_a_leaf;
    size_t length = root_b ? tls_fixture_b_leaf_length : tls_fixture_a_leaf_length;
    (*v)->start_chain(v, "example.test");
    (*v)->start_cert(v, (uint32_t)length);
    (*v)->append(v, cert, length);
    (*v)->end_cert(v);
    bool valid = !(*v)->end_chain(v) && (*v)->get_pkey(v, NULL);
    f.destroy(v);
    return valid;
}

#define CHECK(test, message) do { if (!(test)) { report(message); os64_tls_trust_free(store); os64_free(buffer); return 1; } } while (0)
int tls_trust_selftest(void (*report)(const char *))
{
    os64_tls_trust *store = NULL;
    tls_store_detail detail;
    char *buffer = os64_malloc(TLS_STORE_PEM_MAX + 1);
    CHECK(buffer, "FAIL trust test buffer");
    const size_t fragments[] = {1, 7, 64, 511, 65536};
    for (size_t i = 0; i < sizeof fragments / sizeof fragments[0]; i++) {
        CHECK(parse(tls_fixture_a_pem, tls_fixture_a_pem_length, fragments[i], SIZE_MAX, &store, &detail) == TLS_STORE_OK, "FAIL fragmented PEM");
        CHECK(detail.certificates == 1 && tls_trust_test_verify(store, false) && !tls_trust_test_verify(store, true), "FAIL selected root identity");
        os64_tls_trust_free(store); store = NULL;
    }
    os64_memcpy(buffer, tls_fixture_a_pem, tls_fixture_a_pem_length);
    os64_memcpy(buffer + tls_fixture_a_pem_length, tls_fixture_b_pem, tls_fixture_b_pem_length);
    size_t combined = tls_fixture_a_pem_length + tls_fixture_b_pem_length;
    CHECK(parse(buffer, combined, 37, SIZE_MAX, &store, &detail) == TLS_STORE_OK, "FAIL multiple roots");
    CHECK(detail.certificates == 2 && tls_trust_test_verify(store, false) && tls_trust_test_verify(store, true), "FAIL two-root snapshot");
    os64_tls_trust_free(store); store = NULL;
    static const char preamble[] = "## CA root bundle\n\nFixture Root A\n==============\n";
    static const char between[] = "\nFixture Root B\n==============\n";
    static const char after[] = "\nEnd of explanatory text";
    const char *parts[] = {preamble, tls_fixture_a_pem, between, tls_fixture_b_pem, after};
    size_t annotated = 0;
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        size_t n = os64_strlen(parts[i]);
        os64_memcpy(buffer + annotated, parts[i], n); annotated += n;
    }
    for (size_t i = 0; i < sizeof fragments / sizeof fragments[0]; i++) {
        CHECK(parse(buffer, annotated, fragments[i], SIZE_MAX, &store, &detail) == TLS_STORE_OK, "FAIL annotated PEM");
        CHECK(detail.certificates == 2 && tls_trust_test_verify(store, false) && tls_trust_test_verify(store, true), "FAIL annotated two-root snapshot");
        os64_tls_trust_free(store); store = NULL;
    }
    for (size_t i = 0; i <= annotated; i++)
        CHECK(parse(buffer, annotated, 37, i, &store, &detail) == TLS_STORE_IO && !store, "FAIL annotated read error versus EOF");
    report("PASS trust annotated PEM and complete-stream reads");
    // Convert the fixture's armor and body lines to CRLF with whitespace.
    size_t used = 0;
    for (size_t i = 0; i < tls_fixture_a_pem_length; i++) {
        if (tls_fixture_a_pem[i] == '\n') { buffer[used++] = ' '; buffer[used++] = '\t'; buffer[used++] = '\r'; }
        buffer[used++] = tls_fixture_a_pem[i];
    }
    CHECK(parse(buffer, used, 1, SIZE_MAX, &store, &detail) == TLS_STORE_OK, "FAIL CRLF and armor whitespace");
    os64_tls_trust_free(store); store = NULL;
    report("PASS trust PEM fragments and one/two-root identities");

    static const struct { const char *text; tls_store_status status; } invalid[] = {
        {"", TLS_STORE_FORMAT}, {"# comment\n\n", TLS_STORE_FORMAT},
        {"-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n", TLS_STORE_FORMAT},
        {"preamble\n", TLS_STORE_FORMAT},
        {"-----BEGIN CERTIFICATE-----\n-----END CERTIFICATE-----\n", TLS_STORE_FORMAT},
        {"-----BEGIN CERTIFICATE-----\nTQ=\n-----END CERTIFICATE-----\n", TLS_STORE_FORMAT},
        {"-----BEGIN CERTIFICATE-----\nTR==\n-----END CERTIFICATE-----\n", TLS_STORE_FORMAT},
        {"-----BEGIN CERTIFICATE-----\nTWF=\n-----END CERTIFICATE-----\n", TLS_STORE_FORMAT},
        {"-----BEGIN CERTIFICATE-----\nTQ==AAAA\n-----END CERTIFICATE-----\n", TLS_STORE_FORMAT},
        {"-----BEGIN CERTIFICATE-----\n=AAA\n-----END CERTIFICATE-----\n", TLS_STORE_FORMAT},
        {"-----BEGIN CERTIFICATE-----\nAA=A\n-----END CERTIFICATE-----\n", TLS_STORE_FORMAT},
        {"-----BEGIN CERTIFICATE-----\n# comment\n-----END CERTIFICATE-----\n", TLS_STORE_FORMAT},
        {"-----BEGIN CERTIFICATE-----\n-----BEGIN CERTIFICATE-----\n", TLS_STORE_FORMAT},
        {"-----BEGIN CERTIFICATE-----\nAAAA\n-----END PUBLIC KEY-----\n", TLS_STORE_FORMAT},
        {"-----BEGIN CERTIFICATE-----\nTQ==\n-----END CERTIFICATE-----\n", TLS_STORE_CERTIFICATE}
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
        CHECK(parse(invalid[i].text, os64_strlen(invalid[i].text), 1, SIZE_MAX, &store, &detail) == invalid[i].status && !store, "FAIL PEM negative");
    // Removing the final newline is allowed; removing END armor is not.
    CHECK(parse(tls_fixture_a_pem, tls_fixture_a_pem_length - 1, 7, SIZE_MAX, &store, &detail) == TLS_STORE_OK, "FAIL final newline optional");
    os64_tls_trust_free(store); store = NULL;
    CHECK(parse(tls_fixture_a_pem, tls_fixture_a_pem_length - 10, 7, SIZE_MAX, &store, &detail) == TLS_STORE_FORMAT && !store, "FAIL truncated armor");
    os64_memcpy(buffer, tls_fixture_a_pem, tls_fixture_a_pem_length);
    buffer[tls_fixture_a_pem_length] = '!';
    CHECK(parse(buffer, tls_fixture_a_pem_length + 1, 7, SIZE_MAX, &store, &detail) == TLS_STORE_OK, "FAIL explanatory suffix");
    os64_tls_trust_free(store); store = NULL;
    static const char *bad_suffixes[] = {
        "-", "----", "-----BEGIN", "-----BEGIN CERTIFICATE----", "-----begin CERTIFICATE-----",
        "-----BEGIN CERTIFICATE-----", "-----END CERTIFICATE-----",
        "-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n",
        "-----BEGIN CERTIFICATE-----\nTQ=\n-----END CERTIFICATE-----\n",
        "-----BEGIN CERTIFICATE-----\nSubject: Fixture Root B\n-----END CERTIFICATE-----\n"
    };
    for (size_t i = 0; i < sizeof bad_suffixes / sizeof bad_suffixes[0]; i++) {
        size_t n = os64_strlen(bad_suffixes[i]);
        os64_memcpy(buffer + tls_fixture_a_pem_length, bad_suffixes[i], n);
        CHECK(parse(buffer, tls_fixture_a_pem_length + n, 1, SIZE_MAX, &store, &detail) == TLS_STORE_FORMAT && !store, "FAIL malformed armor after valid root");
    }
    buffer[tls_fixture_a_pem_length] = 0;
    CHECK(parse(buffer, tls_fixture_a_pem_length + 1, 7, SIZE_MAX, &store, &detail) == TLS_STORE_FORMAT && !store, "FAIL hidden PEM suffix");
    buffer[tls_fixture_a_pem_length] = (char)0xff;
    CHECK(parse(buffer, tls_fixture_a_pem_length + 1, 7, SIZE_MAX, &store, &detail) == TLS_STORE_FORMAT && !store, "FAIL non-ASCII explanation");
    for (size_t i = 0; i <= tls_fixture_a_pem_length; i++)
        CHECK(parse(tls_fixture_a_pem, tls_fixture_a_pem_length, 37, i, &store, &detail) == TLS_STORE_IO && !store, "FAIL read error versus EOF");
    report("PASS trust malformed PEM, padding, suffixes and read-error boundaries");

    os64_memcpy(buffer, tls_fixture_a_pem, tls_fixture_a_pem_length);
    os64_memset(buffer + tls_fixture_a_pem_length, '\n', TLS_STORE_PEM_MAX + 1 - tls_fixture_a_pem_length);
    CHECK(parse(buffer, TLS_STORE_PEM_MAX, 65536, SIZE_MAX, &store, &detail) == TLS_STORE_OK, "FAIL exact PEM cap");
    os64_tls_trust_free(store); store = NULL;
    CHECK(parse(buffer, TLS_STORE_PEM_MAX + 1, 65536, SIZE_MAX, &store, &detail) == TLS_STORE_LIMIT && !store, "FAIL PEM cap overflow");
    CHECK(parse(buffer, TLS_STORE_PEM_MAX, 65536, TLS_STORE_PEM_MAX, &store, &detail) == TLS_STORE_IO && !store, "FAIL EOF probe error");
    os64_memset(buffer, ' ', TLS_STORE_LINE_MAX + 1);
    CHECK(parse(buffer, TLS_STORE_LINE_MAX + 1, 511, SIZE_MAX, &store, &detail) == TLS_STORE_LIMIT && !store, "FAIL PEM line limit");
    for (size_t decoded = TLS_CERTIFICATE_MAX; decoded <= TLS_CERTIFICATE_MAX + 1; decoded++) {
        static const char begin[] = "-----BEGIN CERTIFICATE-----\n", end[] = "\n-----END CERTIFICATE-----\n";
        os64_memcpy(buffer, begin, sizeof begin - 1); used = sizeof begin - 1;
        size_t encoded = ((decoded + 2) / 3) * 4;
        for (size_t i = 0; i < encoded; i++) {
            if (i && !(i % 64)) buffer[used++] = '\n';
            buffer[used++] = (decoded % 3 && i >= encoded - (3 - decoded % 3)) ? '=' : 'A';
        }
        os64_memcpy(buffer + used, end, sizeof end - 1); used += sizeof end - 1;
        tls_store_status expected = decoded == TLS_CERTIFICATE_MAX ? TLS_STORE_CERTIFICATE : TLS_STORE_LIMIT;
        CHECK(parse(buffer, used, 511, SIZE_MAX, &store, &detail) == expected && !store, "FAIL decoded certificate cap");
    }
    report("PASS trust PEM input and line bounds");

    static const struct { const char *text, *path; tls_store_status status; } configs[] = {
        {"", TLS_STORE_DEFAULT, TLS_STORE_OK}, {"# absent setting\r\n", TLS_STORE_DEFAULT, TLS_STORE_OK},
        {" TRUST_STORE = /home/my roots.pem # chosen\r\n", "/home/my roots.pem", TLS_STORE_OK},
        {"trust_store=/etc/first\ntrust_store=/home/second", "/home/second", TLS_STORE_OK},
        {"trust_store=/home/$ROOTS.pem", "/home/$ROOTS.pem", TLS_STORE_OK},
        {"trust_store=", "", TLS_STORE_CONFIG}, {"trust_store=relative.pem", "", TLS_STORE_CONFIG},
        {"trust_stroe=/etc/a", "", TLS_STORE_CONFIG}, {"= /etc/a", "", TLS_STORE_CONFIG},
        {"trust_store /etc/a", "", TLS_STORE_CONFIG}, {"trust_store=bad\ntrust_store=/etc/good", "", TLS_STORE_CONFIG}
    };
    char path[TLS_STORE_PATH_MAX];
    for (size_t i = 0; i < sizeof configs / sizeof configs[0]; i++) {
        CHECK(os64_tls_trust_config_parse(configs[i].text, os64_strlen(configs[i].text), path, sizeof path, &detail) == configs[i].status, "FAIL config status");
        CHECK(os64_streq(path, configs[i].path), "FAIL config selection");
    }
    static const char hidden[] = "trust_store=/etc/good\0trust_store=bad";
    CHECK(os64_tls_trust_config_parse(hidden, sizeof hidden - 1, path, sizeof path, &detail) == TLS_STORE_CONFIG && !path[0], "FAIL config embedded NUL");
    os64_memset(buffer, '\n', TLS_STORE_CONFIG_MAX + 1);
    CHECK(os64_tls_trust_config_parse(buffer, TLS_STORE_CONFIG_MAX, path, sizeof path, &detail) == TLS_STORE_OK, "FAIL exact config cap");
    CHECK(os64_tls_trust_config_parse(buffer, TLS_STORE_CONFIG_MAX + 1, path, sizeof path, &detail) == TLS_STORE_LIMIT, "FAIL config cap overflow");
    os64_memcpy(buffer, "trust_store=/", 13);
    os64_memset(buffer + 13, 'a', TLS_STORE_PATH_MAX);
    CHECK(os64_tls_trust_config_parse(buffer, 12 + TLS_STORE_PATH_MAX - 1, path, sizeof path, &detail) == TLS_STORE_OK, "FAIL exact path cap");
    CHECK(os64_tls_trust_config_parse(buffer, 12 + TLS_STORE_PATH_MAX, path, sizeof path, &detail) == TLS_STORE_LIMIT && !path[0], "FAIL path cap overflow");
    os64_free(buffer);
    report("PASS trust config dialect, strict refusals and path bounds");
    return 0;
}
