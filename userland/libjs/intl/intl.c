#include "intl.h"
#include "quickjs.h"
#include "os64/date.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/str.h"
#include <stdbool.h>

/* ECMA-402's C half (INTL.md § 3): the script (intl.js) and its data
 * (intl_data.js) carried in this object as bytes, compiled once a process,
 * and the DOOR the script runs on: the data, evaluated when the script
 * first asks, and the zones — a name's canonical zone, a zone's offset at
 * an instant, and the machine's own zone. Offsets come from libos64's TZ
 * evaluator (os64/date.h), the one Date's local time runs on, over each
 * zone's current rule (zones.inc), so Date and Intl cannot disagree about
 * an offset. */

#ifndef LIBJS_INTL_DIR
#define LIBJS_INTL_DIR "libjs/intl/"
#endif

/* The scripts, NUL-terminated as JS_Eval requires. The path is the
 * build's: the userland build runs from userland/, a host build passes its
 * own directory. */
__asm__(".section .rodata\n"
        ".global js_intl_data_source\n"
        ".hidden js_intl_data_source\n"
        "js_intl_data_source:\n"
        ".incbin \"" LIBJS_INTL_DIR "intl_data.js\"\n"
        ".global js_intl_data_source_end\n"
        ".hidden js_intl_data_source_end\n"
        "js_intl_data_source_end:\n"
        ".byte 0\n"
        ".global js_intl_script_source\n"
        ".hidden js_intl_script_source\n"
        "js_intl_script_source:\n"
        ".incbin \"" LIBJS_INTL_DIR "intl.js\"\n"
        ".global js_intl_script_source_end\n"
        ".hidden js_intl_script_source_end\n"
        "js_intl_script_source_end:\n"
        ".byte 0\n"
        ".previous\n");
extern const char js_intl_data_source[], js_intl_data_source_end[];
extern const char js_intl_script_source[], js_intl_script_source_end[];

/* ── The zone table ────────────────────────────────────────────────────── */
typedef struct {
    const char *name;
    uint16_t canonical, rule, listed;
} IntlZoneName;
#include "zones.inc"

#define ZONE_NAMES (sizeof(kZoneNames) / sizeof(kZoneNames[0]))
#define ZONE_RULES (sizeof(kZoneRules) / sizeof(kZoneRules[0]))

static int fold(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* The table is sorted by name folded to lowercase; zone names are ASCII. */
static int compare_folded(const char *a, const char *b, size_t b_length)
{
    for (size_t i = 0; i < b_length; i++) {
        int x = fold((unsigned char)a[i]), y = fold((unsigned char)b[i]);
        if (x != y || x == 0) return x - y;
    }
    return fold((unsigned char)a[b_length]);
}

/* The index of `name` (any case), or -1. */
static int32_t zone_find(const char *name, size_t length)
{
    size_t low = 0, high = ZONE_NAMES;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int c = compare_folded(kZoneNames[middle].name, name, length);
        if (c == 0) return (int32_t)middle;
        if (c < 0) low = middle + 1;
        else high = middle;
    }
    return -1;
}

/* A string argument's bytes, or NULL (no exception) when it is not a string. */
static const char *string_arg(JSContext *ctx, JSValueConst value, size_t *length)
{
    if (!JS_IsString(value)) return NULL;
    return JS_ToCStringLen(ctx, length, value);
}

/* zoneCanonical(name): the canonical name, or undefined. */
static JSValue zone_canonical(JSContext *ctx, JSValueConst this_value, int argc, JSValueConst *argv)
{
    (void)this_value;
    size_t length = 0;
    const char *name = argc > 0 ? string_arg(ctx, argv[0], &length) : NULL;
    if (name == NULL) return JS_HasException(ctx) ? JS_EXCEPTION : JS_UNDEFINED;
    int32_t at = os64_strlen(name) == length ? zone_find(name, length) : -1;
    JS_FreeCString(ctx, name);
    if (at < 0) return JS_UNDEFINED;
    return JS_NewString(ctx, kZoneNames[kZoneNames[at].canonical].name);
}

/* The epoch second an epoch millisecond falls in. */
static int64_t epoch_second(double ms)
{
    int64_t whole = (int64_t)ms;
    int64_t seconds = whole / 1000;
    if (whole % 1000 < 0) seconds--;
    return seconds;
}

/* zoneOffset(canonical, ms): minutes east of UTC under the zone's current
 * rule; 0 for a name the table does not hold, which the script never asks. */
static JSValue zone_offset(JSContext *ctx, JSValueConst this_value, int argc, JSValueConst *argv)
{
    (void)this_value;
    double ms = 0;
    size_t length = 0;
    const char *name = argc > 0 ? string_arg(ctx, argv[0], &length) : NULL;
    if (name == NULL) return JS_HasException(ctx) ? JS_EXCEPTION : JS_NewInt32(ctx, 0);
    int32_t at = os64_strlen(name) == length ? zone_find(name, length) : -1;
    JS_FreeCString(ctx, name);
    if (argc < 2 || JS_ToFloat64(ctx, &ms, argv[1]) < 0) return JS_EXCEPTION;
    os64_tz_t tz;
    if (at < 0 || ms != ms || os64_tz_parse(kZoneRules[kZoneNames[at].rule], &tz) != 0) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, os64_tz_offset_at(&tz, epoch_second(ms)));
}

static bool same_rule(const os64_tz_rule_t *a, const os64_tz_rule_t *b)
{
    return a->month == b->month && a->week == b->week && a->weekday == b->weekday && a->minute == b->minute;
}

/* Two parsed zones keep the same time: the same offsets and, with
 * daylight time, the same changes. The names are display only. */
static bool same_time(const os64_tz_t *a, const os64_tz_t *b)
{
    if (a->std_offset_min != b->std_offset_min || (a->has_dst != 0) != (b->has_dst != 0)) return false;
    if (!a->has_dst) return true;
    return a->dst_offset_min == b->dst_offset_min && same_rule(&a->dst_start, &b->dst_start) &&
           same_rule(&a->dst_end, &b->dst_end);
}

/* "+HH:MM" or "-HH:MM", ECMA-402's offset zone, for a whole-minute offset. */
static JSValue offset_zone(JSContext *ctx, int32_t minutes)
{
    char text[8];
    int32_t m = minutes < 0 ? -minutes : minutes;
    text[0] = minutes < 0 ? '-' : '+';
    text[1] = (char)('0' + m / 600 % 10);
    text[2] = (char)('0' + m / 60 % 10);
    text[3] = ':';
    text[4] = (char)('0' + m % 60 / 10);
    text[5] = (char)('0' + m % 10);
    text[6] = '\0';
    return JS_NewString(ctx, text);
}

/* zoneHost(): the machine's zone, by the policy Date's local time follows
 * (os64/date.h). A TZ that is a rule names the zone whose rule it is (the
 * table's preferred zone for that rule, kRulePreferred): the same text
 * first, then the same times under other names ("EST5EDT" is
 * "EST5EDT,M3.2.0,M11.1.0"). A rule no zone keeps is its standard offset
 * as an offset zone, and so is the kernel's offset when there is no TZ;
 * no offset at all is UTC. */
static JSValue zone_host(JSContext *ctx, JSValueConst this_value, int argc, JSValueConst *argv)
{
    (void)this_value; (void)argc; (void)argv;
    const char *text = os64_getenv("TZ");
    os64_tz_t tz;
    if (text != NULL && os64_tz_parse(text, &tz) == 0) {
        for (size_t r = 0; r < ZONE_RULES; r++)
            if (os64_strcmp(kZoneRules[r], text) == 0) return JS_NewString(ctx, kZoneNames[kRulePreferred[r]].name);
        for (size_t r = 0; r < ZONE_RULES; r++) {
            os64_tz_t candidate;
            if (os64_tz_parse(kZoneRules[r], &candidate) == 0 && same_time(&tz, &candidate))
                return JS_NewString(ctx, kZoneNames[kRulePreferred[r]].name);
        }
        return tz.std_offset_min == 0 ? JS_NewString(ctx, "UTC") : offset_zone(ctx, tz.std_offset_min);
    }
    os64_time_t now;
    if (os64_time(&now) != 0 || now.tz_offset_minutes == 0) return JS_NewString(ctx, "UTC");
    return offset_zone(ctx, now.tz_offset_minutes);
}

/* ── The compiled scripts, kept for the process ────────────────────────── */
/* As streams.c keeps streams.js (STREAMS.md § 8): the first realm compiles
 * each script and keeps its bytecode here, and every later one reads it,
 * which costs a small part of a compile. The bytes never leave the
 * process, so the engine that reads them is the one that wrote them. The
 * buffers are static, so the cache is no allocation of anybody's to
 * account for, and untouched pages of them cost nothing; a script that
 * outgrows its buffer is compiled by every realm, which is slower and
 * nothing else. One realm claims FILLING, fills, then releases READY; a
 * realm reads the bytes only after acquiring READY. */
#define INTL_BYTECODE_MAX ((size_t)512 * 1024)
enum { CACHE_EMPTY, CACHE_FILLING, CACHE_READY, CACHE_UNCACHED };

typedef struct {
    const char *source, *end, *name;
    uint8_t bytes[INTL_BYTECODE_MAX];
    size_t length;
    uint32_t state;
} Compiled;

static Compiled s_data = {js_intl_data_source, js_intl_data_source_end, "<intl data>", {0}, 0, CACHE_EMPTY};
static Compiled s_script = {js_intl_script_source, js_intl_script_source_end, "<intl>", {0}, 0, CACHE_EMPTY};

/* The script's completion value, or JS_EXCEPTION. A compile keeps no
 * source text, so Intl's functions read as native ones do to toString;
 * file names and line numbers stay, for a failure's frames. A bytecode
 * write the engine had no memory for is no verdict on the script: the
 * next realm tries again, and the runtime's allocator has recorded the
 * refusal, which the installer reports. */
static JSValue completion(JSContext *ctx, Compiled *c)
{
    if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == CACHE_READY) {
        JSValue function = JS_ReadObject(ctx, c->bytes, c->length, JS_READ_OBJ_BYTECODE);
        if (JS_IsException(function)) return function;
        return JS_EvalFunction(ctx, function);
    }
    JSRuntime *rt = JS_GetRuntime(ctx);
    int strip = JS_GetStripInfo(rt);
    JS_SetStripInfo(rt, strip | JS_STRIP_SOURCE);
    JSValue function = JS_Eval(ctx, c->source, (size_t)(c->end - c->source), c->name,
                               JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    JS_SetStripInfo(rt, strip);
    if (JS_IsException(function)) return function;
    uint32_t expected = CACHE_EMPTY;
    if (__atomic_compare_exchange_n(&c->state, &expected, CACHE_FILLING, false, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
        size_t length = 0;
        uint8_t *bytes = JS_WriteObject(ctx, &length, function, JS_WRITE_OBJ_BYTECODE);
        uint32_t state = CACHE_UNCACHED;
        if (bytes == NULL) {
            state = CACHE_EMPTY;
            JS_FreeValue(ctx, JS_GetException(ctx));
        } else if (length <= sizeof(c->bytes)) {
            os64_memcpy(c->bytes, bytes, length);
            c->length = length;
            state = CACHE_READY;
        }
        js_free(ctx, bytes);
        __atomic_store_n(&c->state, state, __ATOMIC_RELEASE);
    }
    return JS_EvalFunction(ctx, function);
}

/* data(): the CLDR tables, evaluated in this realm. intl.js asks once, at
 * the first construction (INTL.md § 6: a realm that never constructs one
 * never pays for them). */
static JSValue data(JSContext *ctx, JSValueConst this_value, int argc, JSValueConst *argv)
{
    (void)this_value; (void)argc; (void)argv;
    return completion(ctx, &s_data);
}

static const JSCFunctionListEntry kDoor[] = {
    JS_CFUNC_DEF("data", 0, data),
    JS_CFUNC_DEF("zoneCanonical", 1, zone_canonical),
    JS_CFUNC_DEF("zoneOffset", 2, zone_offset),
    JS_CFUNC_DEF("zoneHost", 0, zone_host),
};

JSValue js_intl_build(JSContext *ctx, const char *locale)
{
    JSValue factory = completion(ctx, &s_script);
    if (JS_IsException(factory)) return factory;
    JSValue host = JS_NewObjectProto(ctx, JS_NULL);
    JSValue tag = JS_IsException(host) ? JS_EXCEPTION : JS_NewString(ctx, locale);
    JSValue result = JS_EXCEPTION;
    /* The define takes the tag, whether or not it succeeds. */
    if (!JS_IsException(tag) && JS_DefinePropertyValueStr(ctx, host, "locale", tag, JS_PROP_THROW) >= 0 &&
        JS_SetPropertyFunctionList(ctx, host, kDoor, (int)(sizeof(kDoor) / sizeof(kDoor[0]))) == 0) {
        JSValue args[1] = {host};
        result = JS_Call(ctx, factory, JS_UNDEFINED, 1, args);
    }
    JS_FreeValue(ctx, host);
    JS_FreeValue(ctx, factory);
    return result;
}
