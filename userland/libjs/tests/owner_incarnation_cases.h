#ifndef JS_OWNER_INCARNATION_CASES_H
#define JS_OWNER_INCARNATION_CASES_H
#include "../runtime/owner_incarnation.h"

static void owner_incarnation_cases(void (*check)(bool, const char *))
{
    JSOwnerIncarnationIssuer local={0}; uint64_t out=77;
    check(js_owner_incarnation_reserve(NULL,&out)==JS_OWNER_INVALID && out==77 &&
        js_owner_incarnation_reserve(&local,NULL)==JS_OWNER_INVALID &&
        __atomic_load_n(&local._last,__ATOMIC_RELAXED)==0,
        "incarnation: invalid outputs cannot consume identity");
    for(uint64_t i=1;i<=1024;i++)
        check(js_owner_incarnation_reserve(&local,&out)==JS_OWNER_OK && out==i,
            "incarnation: zero is reserved and local issuance is monotonic");
    /* Fresh, never-used test state near exhaustion; no production reset. */
    JSOwnerIncarnationIssuer near_end={UINT64_MAX-1};
    check(js_owner_incarnation_reserve(&near_end,&out)==JS_OWNER_OK && out==UINT64_MAX,
        "incarnation: final representable identity is usable");
    for(size_t i=0;i<16;i++) {
        out=77;
        check(js_owner_incarnation_reserve(&near_end,&out)==JS_OWNER_EXHAUSTED && out==77 &&
            __atomic_load_n(&near_end._last,__ATOMIC_RELAXED)==UINT64_MAX,
            "incarnation: exhausted namespace refuses without wrap or output mutation");
    }
    uint64_t last=0;
    check(js_owner_incarnation_next(&last)==JS_OWNER_OK && last!=0,
        "incarnation: actual loaded-library issuer reserves nonzero identity");
    check(js_owner_incarnation_next(NULL)==JS_OWNER_INVALID,
        "incarnation: invalid global request refuses");
    for(size_t i=0;i<1024;i++) {
        check(js_owner_incarnation_next(&out)==JS_OWNER_OK && out==last+1,
            "incarnation: actual issuer never reuses a number");
        last=out;
    }
}
#endif
