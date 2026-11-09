#include "env.h"
#include "kmalloc.h"
#include "memset.h"
#include "memcpy.h"
#include "strcmp.h"
#include "strlen.h"

// The env-swap lock (declared in env.h — see the doctrine there). Zero is
// the unlocked state, same as every spinlock in the tree.
spinlock_t kTaskEnvLock = 0;

envpage_t *env_create(void)
{
    envpage_t *env = kmalloc_aligned(PAGE_SIZE);
    if (!env)
        return NULL;
    memset(env, 0, PAGE_SIZE);
    env->page_count = 1;
    // count and data_end start at 0 (already zeroed)
    return env;
}

envpage_t *env_inherit(const envpage_t *parent)
{
    if (!parent)
        return env_create();
    size_t size = (size_t)parent->page_count * PAGE_SIZE;
    envpage_t *env = kmalloc_aligned(size);
    if (!env)
        return NULL;
    memcpy(env, parent, size);
    return env;
}

envpage_t *env_grow(const envpage_t *env, uint32_t max_pages)
{
    if (!env || env->page_count >= max_pages)
        return NULL;                        // already at the ceiling — the honest refusal

    uint32_t new_pages = env->page_count * 2;
    if (new_pages > max_pages)
        new_pages = max_pages;

    envpage_t *bigger = kmalloc_aligned((size_t)new_pages * PAGE_SIZE);
    if (!bigger)
        return NULL;

    // Copy header + live data; the allocator's choke-point zeroing already
    // cleared the new tail, so the "data[data_end..end] is zeroed" contract
    // holds without another memset.
    memcpy(bigger, env, ENV_HEADER_SIZE + env->data_end);
    bigger->page_count = new_pages;
    return bigger;
}

const char *env_get(const envpage_t *env, const char *key)
{
    if (!env || !key)
        return NULL;

    const char *ptr = env->data;
    const char *end = env->data + env->data_end;

    while (ptr < end) {
        const char *k = ptr;
        while (ptr < end && *ptr) ptr++;   // find end of key
        ptr++;                              // step over '\0'
        const char *v = ptr;
        while (ptr < end && *ptr) ptr++;   // find end of val
        ptr++;                              // step over '\0'

        if (strcmp(k, key) == 0)
            return v;
    }
    return NULL;
}

bool env_set(envpage_t *env, const char *key, const char *val)
{
    if (!env || !key || !val)
        return false;

    uint32_t cap     = ENV_DATA_CAPACITY(env->page_count);
    size_t   key_len = strlen(key) + 1;    // include null terminator
    size_t   val_len = strlen(val) + 1;

    // Find the old pair first, but do not disturb it until the replacement's
    // final size is known to fit. A false return is a transaction failure:
    // callers may report "ignored" or try to grow the block, and both rely on
    // the environment still containing the value that was there on entry.
    char *ptr = env->data;
    char *end = env->data + env->data_end;
    char *old_start = NULL;
    char *old_end = NULL;
    while (ptr < end) {
        char *k = ptr;
        while (ptr < end && *ptr) ptr++;
        ptr++;
        while (ptr < end && *ptr) ptr++;
        ptr++;

        if (strcmp(k, key) == 0) {
            old_start = k;
            old_end = ptr;
            break;
        }
    }

    size_t old_len = old_start ? (size_t)(old_end - old_start) : 0;
    size_t new_len = key_len + val_len;
    size_t retained = (size_t)env->data_end - old_len;
    if (new_len > (size_t)cap - retained)
        return false;

    // Preserve the established ordering rule: a replacement is compacted
    // out and appended at the end, so every successful set has one write
    // path. The preflight above makes these mutations non-failing.
    if (old_start) {
        size_t tail = (size_t)(end - old_end);
        memmove(old_start, old_end, tail);
        env->data_end -= (uint32_t)old_len;
        env->count--;
    }

    memcpy(env->data + env->data_end, key, key_len);
    env->data_end += (uint32_t)key_len;
    memcpy(env->data + env->data_end, val, val_len);
    env->data_end += (uint32_t)val_len;
    env->count++;

    // A shorter replacement leaves part of the old live range beyond the
    // new data_end. Keep the ABI's unused-tail-is-zero contract true rather
    // than exposing fragments of the retired value through the task mapping.
    if (old_len > new_len)
        memset(env->data + env->data_end, 0, old_len - new_len);
    return true;
}

bool env_unset(envpage_t *env, const char *key)
{
    if (!env || !key)
        return false;

    // Same walk-and-compact env_set uses to replace a key — minus the append.
    char *ptr = env->data;
    char *end = env->data + env->data_end;
    while (ptr < end) {
        char *k = ptr;
        while (ptr < end && *ptr) ptr++;
        ptr++;
        while (ptr < end && *ptr) ptr++;
        ptr++;

        if (strcmp(k, key) == 0) {
            size_t tail = (size_t)(end - ptr);
            memmove(k, ptr, tail);
            env->data_end -= (uint32_t)(ptr - k);
            env->count--;
            // Re-zero the vacated tail: the region past data_end is
            // documented as zeroed, and the read-only task mapping shows
            // these bytes to userland — stale value fragments shouldn't
            // linger where an env walker could trip over them.
            memset(env->data + env->data_end, 0, (size_t)(ptr - k));
            break;
        }
    }
    return true;   // absent == already unset == success
}
