#include "env.h"
#include "kmalloc.h"
#include "memset.h"
#include "memcpy.h"
#include "strcmp.h"
#include "strlen.h"

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

    // If the key already exists, compact it out so we can append the new pair.
    char *ptr = env->data;
    char *end = env->data + env->data_end;
    while (ptr < end) {
        char *k = ptr;
        while (ptr < end && *ptr) ptr++;
        ptr++;
        while (ptr < end && *ptr) ptr++;
        ptr++;

        if (strcmp(k, key) == 0) {
            // Shift everything after this val's '\0' back to where key started.
            size_t tail = (size_t)(end - ptr);
            memmove(k, ptr, tail);
            env->data_end -= (uint32_t)(ptr - k);
            env->count--;
            end = env->data + env->data_end;
            break;
        }
    }

    // Append new pair at the end.
    if (env->data_end + key_len + val_len > cap)
        return false;

    memcpy(env->data + env->data_end, key, key_len);
    env->data_end += (uint32_t)key_len;
    memcpy(env->data + env->data_end, val, val_len);
    env->data_end += (uint32_t)val_len;
    env->count++;
    return true;
}
