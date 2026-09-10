#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tls/tls.h"
void *os64_malloc(size_t n) { return malloc(n); }
void os64_free(void *p) { free(p); }
void *os64_memcpy(void *d, const void *s, size_t n) { return memcpy(d,s,n); }
void *os64_memmove(void *d, const void *s, size_t n) { return memmove(d,s,n); }
void *os64_memset(void *d, int c, size_t n) { return memset(d,c,n); }
size_t os64_strlen(const char *s) { return strlen(s); }
static int64_t reader(void *ctx, void *buf, size_t cap) {
    FILE *f=ctx; size_t n=fread(buf,1,cap,f); return ferror(f) ? -1 : (int64_t)n;
}
int main(int argc, char **argv) {
    for(int i=1;i<argc;i++) {
        FILE *f=fopen(argv[i],"rb"); if(!f) return 2;
        os64_tls_trust *t=NULL; os64_tls_store_detail_t d;
        os64_tls_store_status_t s=os64_tls_trust_load_pem(reader,f,&t,&d);
        printf("%s %u %u %zu %zu\n",argv[i],s,d.policy_reason,d.line,d.certificates);
        os64_tls_trust_free(t); fclose(f);
    }
}
