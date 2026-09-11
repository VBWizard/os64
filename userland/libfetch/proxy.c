// proxy.c — `$http_proxy`, `$https_proxy`, `$no_proxy`, read the way every
// fetcher on this machine reads them. See proxy.h.

#include "proxy.h"

#include "os64/proc.h"
#include "os64/fmt.h"
#include "os64/str.h"

static char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

// A proxy setting's value, in either spelling people actually use: the full
// URL everyone writes ("http://host:8888/") and the bare "host:port" that
// gets typed in a hurry. A value that is neither is refused BY NAME rather
// than silently bypassed: an explicit route must not change without notice.
static bool proxy_take(const char *value, const char *variable, fetch_proxy_t *out,
                       char *why, size_t cap)
{
    http_url_t url;
    http_url_result_t rc = http_url_parse(value, &url);

    if (rc == HTTP_URL_OK)
    {
        if (!os64_streq(url.scheme, "http"))
        {
            os64_snprintf(why, cap, "$%s is %s, and the road TO a proxy is the plain one"
                          "; encrypted proxy connections are not supported", variable, url.scheme);
            return false;
        }
        os64_strcopy(out->host, sizeof(out->host), url.host);
        out->port = url.port;
        out->inUse = true;
        return true;
    }

    if (rc != HTTP_URL_NOT_A_URL)
    {
        os64_snprintf(why, cap, "$%s is not a proxy address (%s): %s",
                      variable, http_url_reason(rc), value);
        return false;
    }

    // "host:port", or a bare "host" meaning the customary 8080.
    const char *colon = NULL;
    for (const char *p = value; *p != '\0'; p++)
        if (*p == ':')
            colon = p;

    size_t hostLen = colon != NULL ? (size_t)(colon - value) : os64_strlen(value);
    if (hostLen == 0 || hostLen >= sizeof(out->host))
    {
        os64_snprintf(why, cap, "$%s names no usable host: %s", variable, value);
        return false;
    }
    for (size_t i = 0; i < hostLen; i++)
        out->host[i] = value[i];
    out->host[hostLen] = '\0';

    out->port = 8080;
    if (colon != NULL)
    {
        uint64_t port = 0;
        if (!os64_parse_u64(colon + 1, &port) || port == 0 || port > 65535)
        {
            os64_snprintf(why, cap, "$%s has no usable port: %s", variable, value);
            return false;
        }
        out->port = (uint16_t)port;
    }
    out->inUse = true;
    return true;
}

// `$no_proxy`: the hosts to reach DIRECTLY even when a proxy is set. Entries
// are separated by commas or spaces; `*` means every host; a bare name
// matches that host and anything under it, so "example.com" also covers
// "www.example.com" but never "notexample.com", and a leading dot is accepted
// because half the world writes ".example.com" for the same thing.
//
// THIS IS NOT DECORATION HERE, it is the difference between a working harness
// and a baffling one. QEMU's guest reaches the host at 10.0.2.2, an address
// that means nothing anywhere else — so a proxy running ON the host, asked to
// fetch `http://10.0.2.2:8080/`, dials into the void and times out. Every
// address that is only meaningful from where os64 is sitting has that shape.
static bool no_proxy_covers(const char *host)
{
    const char *list = os64_getenv("no_proxy");
    if (list == NULL || list[0] == '\0')
        return false;

    size_t hostLen = os64_strlen(host);
    const char *p = list;
    while (*p != '\0')
    {
        while (*p == ',' || *p == ' ' || *p == '\t')
            p++;
        const char *start = p;
        while (*p != '\0' && *p != ',' && *p != ' ' && *p != '\t')
            p++;

        size_t len = (size_t)(p - start);
        if (len == 0)
            continue;
        if (len == 1 && start[0] == '*')
            return true;
        if (start[0] == '.')
        {
            start++;
            len--;
        }
        if (len == 0 || len > hostLen)
            continue;

        // Match the whole host, or a dot-bounded tail of it. The dot is what
        // keeps "example.com" from covering "notexample.com".
        const char *tail = host + (hostLen - len);
        if (len != hostLen && tail[-1] != '.')
            continue;

        size_t i = 0;
        while (i < len && tail[i] == lower_ascii(start[i]))
            i++;
        if (i == len)
            return true;
    }
    return false;
}

bool fetch_proxy_for(const http_url_t *url, fetch_proxy_t *out, char *why, size_t cap)
{
    out->inUse = false;
    if (cap != 0)
        why[0] = '\0';

    const char *variable = os64_streq(url->scheme, "https") ? "https_proxy" : "http_proxy";
    const char *value = os64_getenv(variable);
    if (value == NULL || value[0] == '\0')
        return true;

    if (no_proxy_covers(url->host))
        return true;

    return proxy_take(value, variable, out, why, cap);
}
