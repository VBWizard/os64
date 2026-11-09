#ifndef JS_SUPPORT_CASES_H
#define JS_SUPPORT_CASES_H
#include "os64/str.h"

/* Shared boundary fixtures run against the actual libos64 implementation on
 * the host and against its dynamically loaded exports in the guest. */
static int js_support_string_cases(void)
{
    int failed = 0;
    const char empty[] = "";
    const char text[] = "abaca";
    const unsigned char high[] = {0x80, 0xff, 0};
    const unsigned char low[] = {0x7f, 0};
    const unsigned char binary[] = {0, 0xff, 0x80, 0xff};
    failed += os64_strcmp(empty, empty) != 0;
    failed += os64_strcmp("a", "ab") >= 0;
    failed += os64_strcmp("ab", "a") <= 0;
    failed += os64_strcmp((const char *)high, (const char *)low) <= 0;
    failed += os64_strcmp("same", "same") != 0;
    failed += os64_strchr(text, 'a') != text;
    failed += os64_strrchr(text, 'a') != text + 4;
    failed += os64_strchr(text, 'z') != NULL;
    failed += os64_strrchr(text, 'z') != NULL;
    failed += os64_strchr(text, 0) != text + 5;
    failed += os64_strrchr(text, 0) != text + 5;
    failed += os64_strchr(empty, 0) != empty;
    failed += os64_strrchr(empty, 'x') != NULL;
    failed += os64_strchr((const char *)high, -1) != (const char *)high + 1;
    failed += os64_strrchr(text, 'a' + 256) != text + 4;
    failed += os64_memchr(NULL, 0, 0) != NULL;
    failed += os64_memchr(binary, 0xff, 1) != NULL;
    failed += os64_memchr(binary, -1, sizeof(binary)) != binary + 1;
    failed += os64_memchr(binary, 0, sizeof(binary)) != binary;
    failed += os64_memchr(binary, 0x80, 3) != binary + 2;
    failed += os64_memchr(binary, 0x80, 2) != NULL;
    return failed;
}
#endif
