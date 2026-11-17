#include "owner_incarnation.h"

_Static_assert(__atomic_always_lock_free(sizeof(uint64_t), 0), "lock-free incarnation counter");
static JSOwnerIncarnationIssuer process_issuer;

JSOwnerStatus js_owner_incarnation_reserve(JSOwnerIncarnationIssuer *issuer, uint64_t *out)
{
    if (!issuer || !out) return JS_OWNER_INVALID;
    uint64_t previous = __atomic_load_n(&issuer->_last, __ATOMIC_RELAXED);
    for (;;) {
        if (previous == UINT64_MAX) return JS_OWNER_EXHAUSTED;
        /* Only uniqueness is synchronized here. Runtime publication and
         * lifetime remain the caller's separately ordered obligations. */
        if (__atomic_compare_exchange_n(&issuer->_last, &previous, previous + 1,
                false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            *out = previous + 1;
            return JS_OWNER_OK;
        }
    }
}
JSOwnerStatus js_owner_incarnation_next(uint64_t *out)
{
    return js_owner_incarnation_reserve(&process_issuer, out);
}
