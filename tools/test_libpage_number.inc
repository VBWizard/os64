// Compare the adapted conversion with the host libc, and shortest spelling
// with round-trip bits. These tests do not use libpage to derive expectations.
#include "../userland/libpage/internal.h"
#include <math.h>

static uint64_t number_rng = UINT64_C(0x28492fe10a57c633);
static uint64_t number_next(void)
{
    number_rng ^= number_rng << 13;
    number_rng ^= number_rng >> 7;
    number_rng ^= number_rng << 17;
    return number_rng;
}

#include "test_libpage_range.inc"

static void number_cases(void)
{
    for (size_t i = 0; i < sizeof(range_reference)/sizeof(range_reference[0]); i++) {
        char label[96];
        snprintf(label,sizeof(label),"range decimal oracle %zu",i);
        os64_html_document_t *doc=parse(range_reference[i].html,"utf-8");
        os64_page_t *page=os64_page_build(doc,kPage,NULL);
        const os64_page_control_t *c=os64_page_control(page,0);
        char *end=NULL;
        double actual=strtod(c->value,&end), want=strtod(range_reference[i].value,NULL);
        state_expect(label,!os64_page_incomplete(page) && c->value[0] != '\0' &&
                           *end == '\0' && actual == want);
        os64_page_free(page);os64_html_document_free(doc);
    }
    static const char *valid[] = {".5", "-.5", "12345678901", "00000000000000000000001",
        "1e-999999999999999999999999", "-0", "1.7976931348623157e308", "5e-324",
        "2.2250738585072012e-308", "2.2250738585072014e-308"};
    for (size_t i = 0; i < sizeof(valid)/sizeof(valid[0]); i++) {
        union {double d; uint64_t u;} actual, expected = {strtod(valid[i],NULL)};
        state_expect("number: valid boundary",p_number_parse(valid[i],true,&actual.d));
        state_expect("number: boundary bits agree with libc",actual.u == expected.u);
    }
    static const char *invalid[] = {"", "+1", "1.", " 1", "1 ", "0x10", "nan", "Infinity",
        "1e", "1e+", ".", "1e99999999999999999999999", "1.7976931348623159e308"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
        double v;
        state_expect("number: invalid syntax or nonfinite",!p_number_parse(invalid[i],true,&v));
    }
    static const struct {double value; const char *text;} spell[] = {
        {0,"0"}, {-0.0,"0"}, {1,"1"}, {0.5,"0.5"}, {1e-6,"0.000001"},
        {1e-7,"1e-7"}, {1e20,"100000000000000000000"}, {1e21,"1e+21"},
        {-123.5,"-123.5"}, {5e-324,"5e-324"}
    };
    for (size_t i = 0; i < sizeof(spell)/sizeof(spell[0]); i++) {
        char s[32]; p_number_spell(spell[i].value,s);
        state_expect("number: HTML spelling thresholds",!strcmp(s,spell[i].text));
    }
    for (int i = 0; i < 30000; i++) {
        union {double d; uint64_t u;} value = {.u=number_next()}, parsed;
        if (!isfinite(value.d)) continue;
        char text[96];
        size_t n = p_number_spell(value.d,text);
        state_expect("number: shortest form bounded",n < 32 && strlen(text) == n);
        state_expect("number: shortest form parses",p_number_parse(text,true,&parsed.d));
        state_expect("number: shortest roundtrip bits",value.d == 0 ? parsed.d == 0 : value.u == parsed.u);
        union {double d; uint64_t u;} libc = {strtod(text,NULL)};
        state_expect("number: independent roundtrip bits",libc.u == parsed.u);
        snprintf(text,sizeof(text),"%llu.%016llue%d",(unsigned long long)number_next(),
                 (unsigned long long)(number_next()%UINT64_C(10000000000000000)),
                 (int)(number_next()%701)-350);
        libc.d = strtod(text,NULL);
        bool got = p_number_parse(text,true,&parsed.d);
        state_expect("number: decimal conversion finite result",got == isfinite(libc.d));
        if (got)
            state_expect("number: decimal conversion matches libc",parsed.u == libc.u);
    }
    char *long_text = malloc(25004);
    strcpy(long_text,"0.");
    memset(long_text+2,'1',25000);
    long_text[25002]='\0';
    union {double d; uint64_t u;} actual, expected = {strtod(long_text,NULL)};
    state_expect("number: long decimal accepted",p_number_parse(long_text,true,&actual.d));
    state_expect("number: long decimal rounds like libc",actual.u == expected.u);
    memset(long_text,'0',25000);strcpy(long_text+25000,"1");
    state_expect("number: arbitrary leading zeros",p_number_parse(long_text,true,&actual.d) && actual.d == 1);
    free(long_text);
}
