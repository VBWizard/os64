#include "xml/xml.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t allocations, fail_at, live, peak, abort_at, events, cancel_at, polls;
    uint64_t hash;
    xml_parser_t *parser;
    char text[8192], names[8192], attributes[8192];
    size_t text_size, name_size, attribute_size, dtd_events, document_comments;
} host;
static void *allocate(void *ctx, size_t size)
{
    host *h = ctx;
    if (++h->allocations == h->fail_at)
        return NULL;
    void *p = malloc(size);
    assert(p);
    h->live += size;
    if (h->peak < h->live)
        h->peak = h->live;
    return p;
}
static void release(void *ctx, void *p, size_t size)
{
    host *h = ctx;
    assert(h->live >= size);
    h->live -= size;
    free(p);
}
static void hash(host *h, const void *data, size_t size)
{
    const unsigned char *b = data;
    for (size_t i = 0; i < size; ++i)
        h->hash = (h->hash ^ b[i]) * 1099511628211ULL;
}
static void field(host *h, xml_slice_t s)
{
    hash(h, &s.size, sizeof(s.size));
    hash(h, s.data, s.size);
}
static void capture(char *to, size_t *used, xml_slice_t s)
{
    size_t n = s.size;
    if (n > 8191 - *used)
        n = 8191 - *used;
    if (n)
        memcpy(to + *used, s.data, n);
    *used += n;
    to[*used] = 0;
}
static void name(host *h, xml_name_t n)
{
    field(h, n.uri);
    field(h, n.local);
    field(h, n.prefix);
}
static bool event(void *ctx, const xml_event_t *e)
{
    host *h = ctx;
    ++h->events;
    if (e->in_doctype) {
        assert(e->kind == XML_COMMENT || e->kind == XML_PI);
        ++h->dtd_events;
    } else if (e->kind == XML_COMMENT)
        ++h->document_comments;
    size_t consumed = 99;
    assert(xml_feed(h->parser, NULL, 0, &consumed) == XML_BAD_ARGUMENT && consumed == 0);
    assert(xml_finish(h->parser) == XML_BAD_ARGUMENT);
    hash(h, &e->kind, sizeof(e->kind));
    hash(h, &e->in_doctype, sizeof(e->in_doctype));
    hash(h, &e->location.byte, sizeof(size_t));
    hash(h, &e->location.line, sizeof(size_t));
    hash(h, &e->location.column, sizeof(size_t));
    name(h, e->name);
    field(h, e->text);
    field(h, e->target);
    field(h, e->encoding);
    field(h, e->version);
    field(h, e->public_id);
    field(h, e->system_id);
    hash(h, &e->standalone, sizeof(e->standalone));
    for (size_t i = 0; i < e->attribute_count; ++i) {
        name(h, e->attributes[i].name);
        field(h, e->attributes[i].value);
        capture(h->attributes, &h->attribute_size, e->attributes[i].name.uri);
        capture(h->attributes, &h->attribute_size, (xml_slice_t){"|", 1});
        capture(h->attributes, &h->attribute_size, e->attributes[i].name.local);
        capture(h->attributes, &h->attribute_size, (xml_slice_t){"=", 1});
        capture(h->attributes, &h->attribute_size, e->attributes[i].value);
        capture(h->attributes, &h->attribute_size, (xml_slice_t){";", 1});
    }
    for (size_t i = 0; i < e->namespace_count; ++i) {
        field(h, e->namespaces[i].prefix);
        field(h, e->namespaces[i].uri);
    }
    if (e->kind == XML_TEXT || e->kind == XML_CDATA)
        capture(h->text, &h->text_size, e->text);
    if (e->kind == XML_START_ELEMENT) {
        capture(h->names, &h->name_size, e->name.uri);
        capture(h->names, &h->name_size, (xml_slice_t){"|", 1});
        capture(h->names, &h->name_size, e->name.local);
        capture(h->names, &h->name_size, (xml_slice_t){";", 1});
    }
    return !h->abort_at || h->events < h->abort_at;
}
static bool cancelled(void *ctx)
{
    host *h = ctx;
    return h->cancel_at && ++h->polls >= h->cancel_at;
}
static xml_status_t parse(host *h, const void *data, size_t size, size_t chunk, xml_limits_t limits,
                          xml_stats_t *stats)
{
    xml_options_t o = {.memory = {h, allocate, release},
                       .context = h,
                       .event = event,
                       .cancelled = cancelled,
                       .limits = limits};
    xml_status_t s = xml_create(&o, &h->parser);
    if (s != XML_NEED_INPUT) {
        assert(!h->parser && !h->live);
        return s;
    }
    size_t pos = 0, step = 1;
    while (pos < size && s == XML_NEED_INPUT) {
        size_t n = chunk ? chunk : (step * 7) % 31 + 1;
        ++step;
        if (n > size - pos)
            n = size - pos;
        size_t consumed = 0;
        s = xml_feed(h->parser, (const char *)data + pos, n, &consumed);
        assert(consumed <= n);
        if (s == XML_NEED_INPUT)
            assert(consumed == n);
        pos += consumed;
    }
    if (s == XML_NEED_INPUT)
        s = xml_finish(h->parser);
    assert(xml_status(h->parser) == s);
    assert(xml_finish(h->parser) == s);
    xml_stats_t st = xml_stats(h->parser);
    assert(st.live_bytes == h->live);
    assert(st.peak_bytes == h->peak);
    if (stats)
        *stats = st;
    xml_destroy(h->parser);
    h->parser = NULL;
    assert(!h->live);
    return s;
}
static void check(const char *s, xml_status_t want)
{
    host a = {0}, b = {0}, c = {0};
    size_t n = strlen(s);
    xml_status_t sa = parse(&a, s, n, n + 1, (xml_limits_t){0}, NULL);
    xml_status_t sb = parse(&b, s, n, 1, (xml_limits_t){0}, NULL);
    xml_status_t sc = parse(&c, s, n, 0, (xml_limits_t){0}, NULL);
    if (sa != want || sb != want || sc != want) {
        fprintf(stderr, "Expected %s, got %s/%s/%s: %s\n", xml_status_name(want),
                xml_status_name(sa), xml_status_name(sb), xml_status_name(sc), s);
        abort();
    }
    assert(a.hash == b.hash && b.hash == c.hash);
    for (size_t i = 1; i <= a.allocations; ++i) {
        host failure = {.fail_at = i};
        assert(parse(&failure, s, n, 1, (xml_limits_t){0}, NULL) == XML_NO_MEMORY);
    }
}
static void api_checks(void)
{
    host a = {0}, b = {0};
    xml_options_t oa = {.memory = {&a, allocate, release}, .context = &a, .event = event};
    xml_options_t ob = {.memory = {&b, allocate, release}, .context = &b, .event = event};
    xml_parser_t *out = (xml_parser_t *)(uintptr_t)1;
    assert(xml_create(NULL, &out) == XML_BAD_ARGUMENT && !out);
    assert(xml_create(&oa, NULL) == XML_BAD_ARGUMENT);
    assert(xml_create(&oa, &a.parser) == XML_NEED_INPUT);
    assert(xml_create(&ob, &b.parser) == XML_NEED_INPUT);
    size_t consumed = 99;
    assert(xml_feed(a.parser, NULL, 1, &consumed) == XML_BAD_ARGUMENT && !consumed);
    assert(xml_status(a.parser) == XML_NEED_INPUT);
    assert(xml_feed(a.parser, "x", 1, NULL) == XML_BAD_ARGUMENT);
    assert(xml_feed(a.parser, NULL, 0, &consumed) == XML_NEED_INPUT && !consumed);
    const char *sa = "<r xmlns='a'><n/></r>", *sb = "<r xmlns='b'><n/></r>";
    for (size_t i = 0; sa[i]; ++i) {
        char byte = sa[i];
        assert(xml_feed(a.parser, &byte, 1, &consumed) == XML_NEED_INPUT && consumed == 1);
        byte = sb[i];
        assert(xml_feed(b.parser, &byte, 1, &consumed) == XML_NEED_INPUT && consumed == 1);
        byte = '?'; /* The parser cannot keep a borrowed feed pointer. */
    }
    assert(xml_finish(a.parser) == XML_DONE && xml_finish(b.parser) == XML_DONE);
    assert(!strcmp(a.names, "a|r;a|n;") && !strcmp(b.names, "b|r;b|n;"));
    size_t events = a.events;
    assert(xml_finish(a.parser) == XML_DONE && a.events == events);
    assert(xml_feed(a.parser, "x", 1, &consumed) == XML_BAD_ARGUMENT && !consumed);
    xml_destroy(a.parser);
    xml_destroy(b.parser);
    xml_destroy(NULL);
    assert(!a.live && !b.live);

    a = (host){0};
    assert(xml_create(&oa, &a.parser) == XML_NEED_INPUT);
    const char *bad_encoding = "<?xml version='1.0' encoding='Latin-1'?><r/>";
    assert(xml_feed(a.parser, bad_encoding, strlen(bad_encoding), &consumed) ==
           XML_UNSUPPORTED_ENCODING);
    xml_slice_t label = xml_encoding(a.parser);
    assert(label.size == 7 && !memcmp(label.data, "Latin-1", 7));
    assert(!a.name_size && a.events == 1);
    assert(xml_feed(a.parser, "<r/>", 4, &consumed) == XML_UNSUPPORTED_ENCODING && !consumed);
    xml_destroy(a.parser);
    assert(!a.live);
}
static void limit_check(const char *input, xml_limits_t limits)
{
    host h = {0};
    assert(parse(&h, input, strlen(input), 1, limits, NULL) == XML_LIMIT);
}
static void checks(void)
{
    api_checks();
    const char *valid[] = {
        "<r/>",
        "<xmlns/>",
        "<!-->--><r/>",
        "<!--->--><r/>",
        "<!DOCTYPE r [<!-->[-->]><r/>",
        "<!DOCTYPE r [<!-->]>-->]><r/>",
        "<?xml version='1.0' encoding='utf-8' standalone='yes'?><r />",
        "\xEF\xBB\xBF<r/>",
        "<!--x--><?pi?><r> a\r\nb\rc&amp;&#x1F499;<![CDATA[<&]]></r><?z end?>",
        "<p:r a='v' xmlns:p='urn:p' p:a='w'><r xmlns='urn:d'><n xmlns=''/></r></p:r>",
        "<r xmlns:xml='http://www.w3.org/XML/1998/namespace' xml:lang='en'/>",
        "<!DOCTYPE r SYSTEM 'no-network.dtd'><r/>",
        "<!DOCTYPE r PUBLIC 'some public id' 'nothing.dtd' [<!-- ]> --><?x ]>?>]><r/>",
        "<!DOCTYPE r [<!ENTITY ns 'http://www.w3.org/2000/svg'>]><svg xmlns='&ns;'/>",
        "<!DOCTYPE r [<!ENTITY a 'one'><!ENTITY a 'two'>]><r>&a;</r>",
        "<!DOCTYPE r [<!ENTITY a '&lt;&amp;&#38;lt;'>]><r>&a;</r>",
        "<r a='&#9;&#10;&#13;&quot;&apos;'>&amp;lt;]]&gt;</r>",
        "<r><\xF0\x90\x80\x80/></r>",
        "<r xmlns='urn:a'><r xmlns='urn:b'/><r/></r>"};
    for (size_t i = 0; i < sizeof(valid) / sizeof(*valid); ++i)
        check(valid[i], XML_DONE);
    const char *bad[] = {"",
                         " ",
                         "<r>",
                         "<r></s>",
                         "<r/><s/>",
                         "<r/>x",
                         "x<r/>",
                         "&#32;<r/>",
                         "<r a='<'/>",
                         "<r a='&x;'/>",
                         "<r a='1'a='2'/>",
                         "<r a='1' a='2'/>",
                         "<r/ >",
                         "<r p:a='1'/>",
                         "<r xmlns:p=''/>",
                         "<xmlns:r/>",
                         "<r xmlns='http://www.w3.org/XML/1998/namespace'/>",
                         "<r xmlns:p='http://www.w3.org/2000/xmlns/'/>",
                         "<r xmlns:xml='bad'/>",
                         "<r xmlns:a='u' xmlns:b='u' a:v='1' b:v='2'/>",
                         "<r:a:b/>",
                         "<:r/>",
                         "<r:/>",
                         "<r:1/>",
                         "<?a:b x?><r/>",
                         "<!DOCTYPE r [<!ENTITY a:b 'x'>]><r/>",
                         "<r>]]></r>",
                         "<!--a--b--><r/>",
                         "<!--a---><r/>",
                         "<![CDATA[x]]><r/>",
                         " <?xml version='1.0'?><r/>",
                         "<?XML version='1.0'?><r/>",
                         "<?xml encoding='UTF-8' version='1.0'?><r/>",
                         "<?xml version='1.0'encoding='UTF-8'?><r/>",
                         "<?xml version='1.0' standalone='maybe'?><r/>",
                         "<?xml version='1.0' encoding='UTF-16'?><r/>",
                         "<r>&#0;</r>",
                         "<r>&#xD800;</r>",
                         "<r>&#x110000;</r>",
                         "<r>&#;</r>",
                         "<r>&#X41;</r>",
                         "<r>&#12x;</r>",
                         "<r>&amp</r>",
                         "<r>\xc0\xaf</r>",
                         "<r>\xed\xa0\x80</r>",
                         "<r>\xf4\x90\x80\x80</r>",
                         "<!DOCTYPE r><!DOCTYPE r><r/>",
                         "<r/><!DOCTYPE r>",
                         "<!doctype r><r/>"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i)
        check(bad[i], XML_MALFORMED);
    check("<!DOCTYPE r SYSTEM 'external.dtd'><r>&unknown;</r>", XML_UNSUPPORTED);
    check(
        "<?xml version='1.0' standalone='yes'?><!DOCTYPE r SYSTEM 'external.dtd'><r>&unknown;</r>",
        XML_MALFORMED);
    check("<?xml version='1.1'?><r/>", XML_UNSUPPORTED);
    check("<?xml version='1.0' encoding='Latin-1'?><r/>", XML_UNSUPPORTED_ENCODING);
    check("<!DOCTYPE r [<!ELEMENT r EMPTY>]><r/>", XML_UNSUPPORTED);
    check("<!DOCTYPE r [<!ENTITY a '&b;'>]><r/>", XML_UNSUPPORTED);
    check("<!DOCTYPE r [<!ENTITY a '<b/>'>]><r/>", XML_UNSUPPORTED);
    check("<!DOCTYPE r [<!ENTITY a SYSTEM 'file'>]><r/>", XML_UNSUPPORTED);
    check("<!DOCTYPE r [<!ENTITY lt '&#38;#60;'><!ENTITY amp '&#38;#38;'><!ENTITY gt "
          "'&#62;'><!ENTITY apos '&#39;'><!ENTITY quot '&#34;'>]><r>&lt;&amp;&gt;&apos;&quot;</r>",
          XML_DONE);
    check("<!DOCTYPE r [<!ENTITY lt '&lt;'>]><r/>", XML_MALFORMED);
    check("<!DOCTYPE r [<!ENTITY lt '&#60;'>]><r/>", XML_MALFORMED);
    check("<!DOCTYPE r [<!ENTITY amp '&#38;'>]><r/>", XML_MALFORMED);
    check("<!DOCTYPE r [<!ENTITY gt 'wrong'>]><r/>", XML_MALFORMED);
    host h = {0};
    const char *sample = "<!DOCTYPE r [<!ENTITY ns 'urn:svg'><!ENTITY a '&lt;&#38;lt;&amp;'>]><r "
                         "xmlns='&ns;'><p:q xmlns:p='u' p:a='&#9;'>&a;</p:q></r>";
    assert(parse(&h, sample, strlen(sample), 1, (xml_limits_t){0}, NULL) == XML_DONE);
    assert(!strcmp(h.text, "<<&") && !strcmp(h.names, "urn:svg|r;u|q;"));
    assert(!strcmp(h.attributes, "u|a=\t;"));
    size_t allocations = h.allocations, events = h.events;
    for (size_t i = 1; i <= allocations; ++i) {
        h = (host){.fail_at = i};
        assert(parse(&h, sample, strlen(sample), 1, (xml_limits_t){0}, NULL) == XML_NO_MEMORY);
    }
    for (size_t i = 1; i <= events; ++i) {
        h = (host){.abort_at = i};
        assert(parse(&h, sample, strlen(sample), 1, (xml_limits_t){0}, NULL) == XML_CONSUMER_ABORT);
    }
    for (size_t i = 1; i < 200; ++i) {
        h = (host){.cancel_at = i};
        assert(parse(&h, sample, strlen(sample), 1, (xml_limits_t){0}, NULL) == XML_CANCELLED);
    }
    h = (host){0};
    assert(parse(&h, "<a><b/></a>", 11, 1, (xml_limits_t){.depth = 1}, NULL) == XML_LIMIT);
    h = (host){0};
    assert(parse(&h, "<abc/>", 6, 1, (xml_limits_t){.name_bytes = 2}, NULL) == XML_LIMIT);
    h = (host){0};
    assert(parse(&h, "<r/>", 4, 1, (xml_limits_t){.encoded_bytes = 3}, NULL) == XML_LIMIT);
    h = (host){0};
    assert(parse(&h, "<r/>", 4, 1, (xml_limits_t){.work = 2}, NULL) == XML_LIMIT);
    const char *normalization = "<!DOCTYPE r [<!ENTITY a '&#9;&#10;&#13;'>]><r xmlns='u' "
                                "b='\t\r\n&#9;&#10;&#13;&a;'>&a;&amp;lt;</r>";
    h = (host){0};
    assert(parse(&h, normalization, strlen(normalization), 1, (xml_limits_t){0}, NULL) == XML_DONE);
    assert(!strcmp(h.attributes, "|b=  \t\n\r   ;"));
    assert(!strcmp(h.text, "\t\n\r&lt;"));
    const char *contexts = "<!--outer--><!DOCTYPE r [<!--inner--><?inside?>]><r/><!--after-->";
    h = (host){0};
    assert(parse(&h, contexts, strlen(contexts), 1, (xml_limits_t){0}, NULL) == XML_DONE);
    assert(h.dtd_events == 2 && h.document_comments == 2);
    limit_check("<r/>", (xml_limits_t){.decoded_bytes = 3});
    limit_check("<r/>", (xml_limits_t){.live_bytes = 1});
    limit_check("<r>long text</r>", (xml_limits_t){.token_bytes = 5});
    limit_check("<r a='a' b='b'/>", (xml_limits_t){.attributes = 1});
    limit_check("<r xmlns:a='u' xmlns:b='v'/>", (xml_limits_t){.namespaces = 1});
    limit_check("<!DOCTYPE r [<!ENTITY a 'x'><!ENTITY a 'y'>]><r/>", (xml_limits_t){.entities = 1});
    limit_check("<!DOCTYPE r [<!ENTITY a 'abcd'>]><r>&a;&a;</r>",
                (xml_limits_t){.expanded_bytes = 5});
    /* The same scalar stream in both UTF-16 byte orders, including a pair. */
    const uint16_t units[] = {0xfeff, '<',  'r', '>', 0xd83d, 0xdc99,
                              '\r',   '\n', '<', '/', 'r',    '>'};
    for (unsigned be = 0; be < 2; ++be) {
        unsigned char bytes[sizeof(units)];
        for (size_t i = 0; i < sizeof(units) / sizeof(*units); ++i) {
            bytes[2 * i + be] = (unsigned char)units[i];
            bytes[2 * i + 1 - be] = (unsigned char)(units[i] >> 8);
        }
        host a = {0}, b = {0};
        assert(parse(&a, bytes, sizeof(bytes), sizeof(bytes), (xml_limits_t){0}, NULL) == XML_DONE);
        assert(parse(&b, bytes, sizeof(bytes), 1, (xml_limits_t){0}, NULL) == XML_DONE);
        assert(a.hash == b.hash && !strcmp(a.text, "\xF0\x9F\x92\x99\n"));
        h = (host){0};
        assert(parse(&h, bytes, sizeof(bytes) - 1, 1, (xml_limits_t){0}, NULL) == XML_MALFORMED);
    }
    const char *utf16_sources[] = {"<?xml version='1.0'?><r/>", "<?pi?><r/>",
                                   "<?xml version='1.0' encoding='UTF-16LE'?><r/>",
                                   "<?xml version='1.0' encoding='UTF-16BE'?><r/>",
                                   "<?xml version='1.0' encoding='UTF-16'?><r/>"};
    for (unsigned be = 0; be < 2; ++be) {
        for (size_t i = 0; i < sizeof(utf16_sources) / sizeof(*utf16_sources); ++i) {
            unsigned char bytes[256];
            size_t n = strlen(utf16_sources[i]);
            for (size_t j = 0; j < n; ++j) {
                bytes[2 * j + be] = (unsigned char)utf16_sources[i][j];
                bytes[2 * j + 1 - be] = 0;
            }
            xml_status_t expected = i == 2 + be ? XML_DONE : XML_MALFORMED;
            host a = {0}, b = {0};
            assert(parse(&a, bytes, n * 2, n * 2, (xml_limits_t){0}, NULL) == expected);
            assert(parse(&b, bytes, n * 2, 1, (xml_limits_t){0}, NULL) == expected);
            assert(a.hash == b.hash);
        }
    }
    const uint16_t bad_scalars[] = {0, 0xd800, 0xdc00, 0xfffe, 0xffff};
    for (unsigned be = 0; be < 2; ++be) {
        for (size_t i = 0; i < sizeof(bad_scalars) / sizeof(*bad_scalars); ++i) {
            const uint16_t units[] = {0xfeff, '<', 'r', '>', bad_scalars[i], '<', '/', 'r', '>'};
            unsigned char bytes[sizeof(units)];
            for (size_t j = 0; j < sizeof(units) / sizeof(*units); ++j) {
                bytes[j * 2 + be] = (unsigned char)units[j];
                bytes[j * 2 + 1 - be] = (unsigned char)(units[j] >> 8);
            }
            h = (host){0};
            assert(parse(&h, bytes, sizeof(bytes), 1, (xml_limits_t){0}, NULL) == XML_MALFORMED);
        }
    }
    /* Work is bounded by input, including a long unfinished quoted value. */
    size_t n = 100000;
    char *long_token = malloc(n);
    assert(long_token);
    memset(long_token, 'a', n);
    memcpy(long_token, "<r a='", 6);
    xml_stats_t st;
    h = (host){0};
    assert(parse(&h, long_token, n, 1, (xml_limits_t){0}, &st) == XML_MALFORMED);
    assert(st.work < n * 10);
    free(long_token);
    /* Deterministic malformed-byte campaign; every chunking has one verdict. */
    uint32_t rng = 0x6a2026;
    unsigned char bytes[256];
    for (size_t trial = 0; trial < 10000; ++trial) {
        size_t n = trial % sizeof(bytes);
        for (size_t i = 0; i < n; ++i) {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            bytes[i] = (unsigned char)rng;
        }
        host a = {0}, b = {0};
        xml_status_t sa = parse(&a, bytes, n, n + 1, (xml_limits_t){0}, NULL),
                     sb = parse(&b, bytes, n, 1, (xml_limits_t){0}, NULL);
        assert(sa == sb && a.hash == b.hash);
    }
    puts("XML checks: events, encodings, namespaces, allocation failures, cancellation, limits, "
         "10000 byte/chunk fuzz cases passed");
}
static void file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    assert(!fseek(f, 0, SEEK_END));
    long n = ftell(f);
    assert(n >= 0 && n < 64 * 1024 * 1024);
    rewind(f);
    char *bytes = malloc((size_t)n + 1);
    assert(bytes);
    assert(fread(bytes, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    host a = {0}, b = {0}, c = {0};
    xml_status_t sa = parse(&a, bytes, (size_t)n, (size_t)n + 1, (xml_limits_t){0}, NULL);
    xml_status_t sb = parse(&b, bytes, (size_t)n, 1, (xml_limits_t){0}, NULL);
    xml_status_t sc = parse(&c, bytes, (size_t)n, 0, (xml_limits_t){0}, NULL);
    if (sa != sb || sb != sc || a.hash != b.hash || b.hash != c.hash) {
        fprintf(stderr, "Chunk mismatch: %s\n", path);
        abort();
    }
    printf("%s\n", xml_status_name(sa));
    free(bytes);
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--checks")) {
        checks();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--batch")) {
        char path[4096];
        while (fgets(path, sizeof(path), stdin)) {
            path[strcspn(path, "\n")] = 0;
            file(path);
        }
        return ferror(stdin) ? 2 : 0;
    }
    if (argc == 2) {
        file(argv[1]);
        return 0;
    }
    fprintf(stderr, "usage: %s --checks | --batch | file\n", argv[0]);
    return 2;
}
