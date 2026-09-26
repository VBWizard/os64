// Network regression for early responses and resumable POST upload.
// Run explicitly against tools/httptestd.py; no public internet dependency.
#include "os64/os64.h"
#include "fetch/fetch.h"

static unsigned failures;
static unsigned char body[70000];
#define CHECK(c) do { if (!(c)) { os64_printf("fetchposttest: FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
static void probe(const char *base, const char *path, int status, bool echo)
{
    char url[OS64_FETCH_URL_MAX];
    int32_t n = os64_snprintf(url, sizeof(url), "%s%s", base, path);
    CHECK(n > 0 && (size_t)n < sizeof(url));
    if (n <= 0 || (size_t)n >= sizeof(url)) return;
    os64_fetch_options_t opt = {.method=OS64_FETCH_METHOD_POST,
        .body=body, .body_len=sizeof(body), .no_proxy=true, .idle_ms=3000};
    os64_fetch_t *f = os64_fetch_open(url, &opt);
    CHECK(f && os64_fetch_status(f)==OS64_FETCH_OK);
    const os64_fetch_head_t *h = os64_fetch_head(f);
    CHECK(h && h->status==status && h->method==OS64_FETCH_METHOD_POST);
    const char *prefix = echo ? "method=POST\ncontent-type=\nbytes=70000\n" : "too large";
    size_t prefix_len = os64_strlen(prefix), used = 0;
    unsigned char buf[1024];
    int64_t got;
    bool matches = true;
    while ((got=os64_fetch_read(f,buf,sizeof(buf))) > 0) {
        for (size_t i=0; i<(size_t)got; i++,used++) {
            if (used<prefix_len) matches &= buf[i]==(unsigned char)prefix[used];
            else if (echo && used-prefix_len<sizeof(body)) matches &= buf[i]==body[used-prefix_len];
            else matches=false;
        }
    }
    CHECK(matches && used==prefix_len+(echo ? sizeof(body) : 0));
    CHECK(got==0 && os64_fetch_status(f)==OS64_FETCH_OK);
    os64_fetch_close(f);
}
int main(int argc, char **argv)
{
    if(argc!=2) { os64_printf("usage: fetchposttest http://host:port (tools/httptestd.py)\n"); return 2; }
    for(size_t i=0;i<sizeof(body);i++) body[i]=(unsigned char)i;
    probe(argv[1],"/post-early",413,false);
    probe(argv[1],"/post-continue",200,true);
    os64_printf("fetchposttest: %s (%u failures)\n",failures ? "FAIL" : "PASS",failures);
    return failures ? 1 : 0;
}
