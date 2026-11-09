#ifndef FETCH_PROXY_H
#define FETCH_PROXY_H
// proxy.h — libfetch's private reading of the environment's proxy settings:
// `$http_proxy`, `$https_proxy`, `$no_proxy`. The environment is the same
// for every program, so there is one reader (LIBFETCH.md: policy about this
// MACHINE that every fetcher shares is the library's; what a program SAYS
// about it stays the program's).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fetch/http.h"

typedef struct {
    bool     inUse;
    char     host[HTTP_HOST_MAX];
    uint16_t port;
} fetch_proxy_t;

// Select the scheme-specific proxy, with no fallback between variables;
// `$no_proxy` bypasses an explicitly configured route for matching hosts.
// Returns false when the setting that WOULD carry this URL is unusable —
// refused by name rather than silently bypassed, because an explicit route
// must not change without notice — and writes one sentence saying what is
// wrong with it into `why`. True with `out->inUse` false means "go direct".
bool fetch_proxy_for(const http_url_t *url, fetch_proxy_t *out, char *why, size_t cap);
#endif
