#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "../userland/libtls/include/tls/tls.h"
#include "../userland/libtls/port/tls13_engine.h"
#include "../userland/libtls/port/client_engine.h"
#include "os64/date.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); ERR_print_errors_fp(stderr); exit(1); } } while (0)
static size_t allocation_calls, fail_allocation, live, peak;
static bool next_engine;
typedef union { max_align_t alignment; struct { size_t size; bool secret; } data; } allocation;
void *os64_malloc(size_t n)
{
    bool secret=next_engine; next_engine=false;
    if (++allocation_calls==fail_allocation) return NULL;
    allocation *a=malloc(sizeof *a+n); CHECK(a); a->data.size=n; a->data.secret=secret;
    live++; if (n>peak) peak=n; memset(a+1,0xa5,n); return a+1;
}
void os64_free(void *p)
{
    if (!p) return;
    allocation *a=(allocation *)p-1;
    if (a->data.secret) for (size_t i=0;i<a->data.size;i++) CHECK(!((unsigned char *)p)[i]);
    live--; free(a);
}
static unsigned entropy_serial;
int64_t os64_open(const char *path, const char *mode)
{ CHECK(!strcmp(path,"/dev/random") && !strcmp(mode,"r")); return ++entropy_serial; }
int64_t os64_close(int32_t fd) { CHECK(fd>0); return 0; }
int64_t os64_read(int32_t fd, void *p, uint64_t n)
{ CHECK(fd>0); for (size_t i=0;i<n;i++) ((unsigned char *)p)[i]=(unsigned char)(i*13+(unsigned)fd); return (int64_t)n; }
static int64_t epoch=1791504000;
int64_t os64_time(os64_time_t *t) { memset(t,0,sizeof *t); t->epoch=epoch; return 0; }
static const char *directory;
static os64_tls_trust *trust;
static void path(char *out, const char *name) { CHECK(snprintf(out,1024,"%s/%s",directory,name)<1024); }
static void root_file(const char *filename)
{
    char name[1024]; path(name,filename); FILE *f=fopen(name,"rb"); CHECK(f);
    unsigned char der[4096]; size_t n=fread(der,1,sizeof der,f); CHECK(n && !ferror(f)); fclose(f);
    CHECK(os64_tls_trust_create(&trust)==OS64_TLS_OK);
    CHECK(os64_tls_trust_add_der(trust,der,n,NULL)==OS64_TLS_OK);
    CHECK(os64_tls_trust_seal(trust)==OS64_TLS_OK);
}
typedef struct {
    SSL_CTX *ctx; SSL *ssl; BIO *in, *out;
    os64_tls_client *client;
    unsigned char incoming[131072]; size_t incoming_at, incoming_len;
    size_t fragment; bool server_error;
    unsigned char captured[131072], hs_secret[48], app_secret[48];
    size_t captured_len, secret_len;
    unsigned seed;
} connection;
static void keylog(const SSL *ssl, const char *line)
{
    connection *c=SSL_get_app_data(ssl);
    if (!c) return;
    const char *p=strchr(line,' '); if (!p) return; p=strchr(p+1,' '); if (!p) return; p++;
    unsigned char *out=!strncmp(line,"SERVER_HANDSHAKE_TRAFFIC_SECRET ",31)?c->hs_secret:
        !strncmp(line,"SERVER_TRAFFIC_SECRET_0 ",24)?c->app_secret:NULL;
    if (!out) return;
    size_t n=strlen(p)/2; CHECK(n==32 || n==48); c->secret_len=n;
    for (size_t i=0;i<n;i++) { unsigned x; CHECK(sscanf(p+2*i,"%2x",&x)==1); out[i]=(unsigned char)x; }
}
static tls13_engine *inner(os64_tls_client *c)
{ return os64_tls_engine_test_context((os64_tls_engine *)c); }
static os64_tls_client *new_client(os64_tls_protocol_t protocol, const char *hostname)
{
    const os64_tls_name_t name={"http/1.1",8};
    os64_tls_config_t cfg={.hostname={hostname,strlen(hostname)},.alpn=&name,.alpn_count=1,.trust=trust,.protocol=protocol};
    os64_tls_client *client=NULL; next_engine=true;
    CHECK(os64_tls_client_create(&cfg,&client)==OS64_TLS_OK); return client;
}
static int alpn(SSL *ssl, const unsigned char **out, unsigned char *length,
                const unsigned char *in, unsigned int in_len, void *arg)
{
    (void)ssl; (void)arg;
    static const unsigned char name[]={8,'h','t','t','p','/','1','.','1'};
    return SSL_select_next_proto((unsigned char **)out,length,name,sizeof name,in,in_len)==OPENSSL_NPN_NEGOTIATED?
        SSL_TLSEXT_ERR_OK:SSL_TLSEXT_ERR_ALERT_FATAL;
}
static connection *connect_peer(const char *kind, const char *suite, const char *group,
                                 os64_tls_protocol_t protocol, int version, size_t fragment)
{
    connection *c=calloc(1,sizeof *c); CHECK(c); c->fragment=fragment;
    c->ctx=SSL_CTX_new(TLS_server_method()); CHECK(c->ctx);
    CHECK(SSL_CTX_set_min_proto_version(c->ctx,version)); CHECK(SSL_CTX_set_max_proto_version(c->ctx,version));
    if (version==TLS1_3_VERSION) CHECK(SSL_CTX_set_ciphersuites(c->ctx,suite));
    else CHECK(SSL_CTX_set_cipher_list(c->ctx,suite));
    CHECK(SSL_CTX_set1_groups_list(c->ctx,group));
    char file[1024], name[64]; snprintf(name,sizeof name,"%s.pem",kind); path(file,name);
    CHECK(SSL_CTX_use_certificate_chain_file(c->ctx,file));
    snprintf(name,sizeof name,"%s.key",kind); path(file,name); CHECK(SSL_CTX_use_PrivateKey_file(c->ctx,file,SSL_FILETYPE_PEM));
    SSL_CTX_set_alpn_select_cb(c->ctx,alpn,NULL);
    SSL_CTX_set_keylog_callback(c->ctx,keylog);
    c->ssl=SSL_new(c->ctx); CHECK(c->ssl); SSL_set_app_data(c->ssl,c); c->in=BIO_new(BIO_s_mem()); c->out=BIO_new(BIO_s_mem()); CHECK(c->in && c->out);
    BIO_set_mem_eof_return(c->in,-1); BIO_set_mem_eof_return(c->out,-1);
    SSL_set_bio(c->ssl,c->in,c->out); SSL_set_accept_state(c->ssl);
    c->client=new_client(protocol,"example.test"); c->seed=entropy_serial;
    return c;
}
static void destroy(connection *c)
{ os64_tls_free(c->client); SSL_free(c->ssl); SSL_CTX_free(c->ctx); free(c); }
static void ssl_result(connection *c, int result)
{
    if (result>0) return;
    int error=SSL_get_error(c->ssl,result);
    if (error!=SSL_ERROR_WANT_READ && error!=SSL_ERROR_WANT_WRITE && error!=SSL_ERROR_ZERO_RETURN) c->server_error=true;
}
static void drive(connection *c)
{
    unsigned char buf[4096]; size_t cap=c->fragment<sizeof buf?c->fragment:sizeof buf;
    os64_tls_transfer_t sent=os64_tls_take_ciphertext(c->client,buf,cap);
    if (sent.transferred) CHECK(BIO_write(c->in,buf,(int)sent.transferred)==(int)sent.transferred);
    if (!SSL_is_init_finished(c->ssl) && !c->server_error) ssl_result(c,SSL_do_handshake(c->ssl));
    if (c->incoming_at==c->incoming_len) {
        c->incoming_at=c->incoming_len=0;
        int n=BIO_read(c->out,c->incoming,sizeof c->incoming);
        if (n>0) {
            c->incoming_len=(size_t)n;
            if (c->captured_len+(size_t)n<=sizeof c->captured) {
                memcpy(c->captured+c->captured_len,c->incoming,(size_t)n); c->captured_len+=(size_t)n;
            }
        }
    }
    if (c->incoming_at<c->incoming_len) {
        size_t n=c->incoming_len-c->incoming_at; if (n>cap) n=cap;
        os64_tls_transfer_t got=os64_tls_feed_ciphertext(c->client,c->incoming+c->incoming_at,n);
        c->incoming_at+=got.transferred;
    }
}
static void handshake_done(connection *c)
{
    for (unsigned i=0;i<200000;i++) {
        drive(c);
        os64_tls_state_t s=os64_tls_state(c->client);
        if (s.status!=OS64_TLS_OK || c->server_error) {
            fprintf(stderr,"handshake failed status=%d error=%d policy=%d server=%d\n",s.status,s.upstream_error,s.policy_reason,c->server_error);
            CHECK(0);
        }
        if ((s.flags&OS64_TLS_HANDSHAKE_DONE) && SSL_is_init_finished(c->ssl)) {
            CHECK(s.version==SSL_version(c->ssl)); CHECK(s.alpn && !strcmp(s.alpn,"http/1.1")); return;
        }
    }
    CHECK(0);
}
static unsigned char pattern(size_t i, unsigned salt) { return (unsigned char)(i*17+salt); }
static void exchange(connection *c, size_t total)
{
    size_t written=0,received=0,server_written=0,server_received=0;
    unsigned char out[4096],in[4096];
    for (unsigned step=0;step<3000000;step++) {
        if (written<total) {
            size_t n=total-written; if (n>sizeof out) n=sizeof out;
            for (size_t i=0;i<n;i++) out[i]=pattern(written+i,7);
            os64_tls_transfer_t t=os64_tls_write_plaintext(c->client,out,n); written+=t.transferred;
            CHECK(t.status==OS64_TLS_OK || t.status==OS64_TLS_NEED_PROGRESS);
        }
        if (written==total) CHECK(os64_tls_flush(c->client)==OS64_TLS_OK);
        if (server_written<total) {
            size_t n=total-server_written; if (n>sizeof out) n=sizeof out;
            for (size_t i=0;i<n;i++) out[i]=pattern(server_written+i,83);
            int count=SSL_write(c->ssl,out,(int)n);
            if (count>0) server_written+=(size_t)count; else ssl_result(c,count);
        }
        drive(c);
        int count=SSL_read(c->ssl,in,sizeof in);
        if (count>0) {
            CHECK(server_received+(size_t)count<=total);
            for (int i=0;i<count;i++) CHECK(in[i]==pattern(server_received+(size_t)i,7));
            server_received+=(size_t)count;
        } else ssl_result(c,count);
        os64_tls_transfer_t t=os64_tls_read_plaintext(c->client,in,1);
        CHECK(t.status==OS64_TLS_OK || t.status==OS64_TLS_NEED_PROGRESS);
        if (t.transferred) { CHECK(received<total && in[0]==pattern(received,83)); received++; }
        CHECK(!c->server_error);
        if (written==total && received==total && server_written==total && server_received==total) return;
    }
    CHECK(0);
}
static void close_connection(connection *c, bool peer_first)
{
    if (peer_first) { int result=SSL_shutdown(c->ssl); if (result<0) ssl_result(c,result); }
    else CHECK(os64_tls_begin_close(c->client)==OS64_TLS_OK);
    for (unsigned i=0;i<100000;i++) {
        drive(c); int result=SSL_shutdown(c->ssl); if (result<0) ssl_result(c,result);
        os64_tls_state_t s=os64_tls_state(c->client);
        if (s.status==OS64_TLS_CLEAN_EOF) { CHECK(!c->server_error); return; }
        CHECK(s.status==OS64_TLS_OK);
    }
    CHECK(0);
}
static size_t drain(os64_tls_client *client, unsigned char *out, size_t capacity)
{
    size_t at=0;
    while (at<capacity) {
        os64_tls_transfer_t t=os64_tls_take_ciphertext(client,out+at,capacity-at);
        if (!t.transferred) break;
        at+=t.transferred;
    }
    return at;
}
static size_t feed(os64_tls_client *client, const unsigned char *p, size_t n)
{
    size_t at=0;
    while (at<n) {
        os64_tls_transfer_t t=os64_tls_feed_ciphertext(client,p+at,n-at);
        CHECK(t.transferred<=n-at); at+=t.transferred;
        if (t.status!=OS64_TLS_OK || !t.transferred) break;
    }
    return at;
}
// Service incoming records without taking client output, as when socket writes
// are blocked. Include the suffix already retained by the handshake driver.
static void receive_only(connection *c)
{
    if (c->incoming_at<c->incoming_len) {
        size_t n=c->incoming_len-c->incoming_at;
        CHECK(feed(c->client,c->incoming+c->incoming_at,n)==n);
        c->incoming_at=c->incoming_len;
    }
    unsigned char wire[32768]; int n;
    while ((n=BIO_read(c->out,wire,sizeof wire))>0)
        CHECK(feed(c->client,wire,(size_t)n)==(size_t)n);
}
static void pending_output(void)
{
    const char *suites[]={"TLS_AES_128_GCM_SHA256","TLS_AES_256_GCM_SHA384","TLS_CHACHA20_POLY1305_SHA256"};
    const char response[]="HTTP/1.1 413 Content Too Large\r\nContent-Length: 0\r\n\r\n";
    for (unsigned suite=0;suite<3;suite++) for (unsigned mode=0;mode<3;mode++) {
        connection *c=connect_peer("ec",suites[suite],"X25519",OS64_TLS_PROTOCOL_DEFAULT,TLS1_3_VERSION,4096);
        handshake_done(c); receive_only(c);
        unsigned char upload[8193], wire[32768], saved[TLS13_SEND_MAX], plain[8193];
        for (size_t i=0;i<sizeof upload;i++) upload[i]=pattern(i,29);
        CHECK(os64_tls_write_plaintext(c->client,upload,sizeof upload).transferred==sizeof upload);
        CHECK(os64_tls_flush(c->client)==OS64_TLS_OK);
        if (mode==1) {
            CHECK(os64_tls_take_ciphertext(c->client,wire,7).transferred==7);
            CHECK(BIO_write(c->in,wire,7)==7);
        }
        tls13_engine *e=inner(c->client);
        size_t send_at=e->send_at, send_used=e->send_used;
        CHECK(send_used>send_at); memcpy(saved,e->send,sizeof saved);
        os64_tls_state_t state=os64_tls_state(c->client);
        CHECK((state.flags&(OS64_TLS_SEND_CIPHER|OS64_TLS_RECV_CIPHER))==
              (OS64_TLS_SEND_CIPHER|OS64_TLS_RECV_CIPHER));
        if (mode==2) {
            CHECK(SSL_key_update(c->ssl,SSL_KEY_UPDATE_REQUESTED));
            ssl_result(c,SSL_do_handshake(c->ssl));
        }
        CHECK(SSL_write(c->ssl,response,sizeof response)==sizeof response);
        receive_only(c);
        state=os64_tls_state(c->client);
        CHECK(state.status==OS64_TLS_OK && (state.flags&OS64_TLS_RECV_PLAIN));
        CHECK(!(state.flags&OS64_TLS_RECV_CIPHER));
        CHECK(e->send_at==send_at && e->send_used==send_used && !memcmp(saved,e->send,sizeof saved));
        CHECK(os64_tls_read_plaintext(c->client,plain,sizeof plain).transferred==sizeof response);
        CHECK(!memcmp(plain,response,sizeof response));
        CHECK(os64_tls_state(c->client).flags&OS64_TLS_RECV_CIPHER);
        // Drain just the upload record; a requested KeyUpdate reply remains
        // pending and must also permit receiving with the new server read key.
        size_t n=send_used-send_at;
        CHECK(os64_tls_take_ciphertext(c->client,wire,n).transferred==n);
        CHECK(BIO_write(c->in,wire,(int)n)==(int)n);
        CHECK(SSL_read(c->ssl,plain,sizeof plain)==sizeof upload);
        CHECK(!memcmp(plain,upload,sizeof upload));
        if (mode==2) {
            CHECK(e->send_used && !e->update_pending);
            CHECK(SSL_write(c->ssl,"next",4)==4); receive_only(c);
            CHECK(os64_tls_read_plaintext(c->client,plain,sizeof plain).transferred==4);
            CHECK(!memcmp(plain,"next",4));
            n=drain(c->client,wire,sizeof wire); CHECK(n);
            CHECK(BIO_write(c->in,wire,(int)n)==(int)n);
        }
        exchange(c,257); close_connection(c,false); destroy(c);
    }
    puts("PASS early response with untaken/partial upload and pending KeyUpdate reply, preserved output and bidirectional rekey");
}
static void pending_close(void)
{
    connection *c=connect_peer("ec","TLS_AES_128_GCM_SHA256","X25519",OS64_TLS_PROTOCOL_DEFAULT,TLS1_3_VERSION,4096);
    handshake_done(c); receive_only(c);
    CHECK(os64_tls_begin_close(c->client)==OS64_TLS_OK);
    tls13_engine *e=inner(c->client);
    unsigned char saved[TLS13_SEND_MAX], wire[32768];
    size_t send_used=e->send_used; CHECK(send_used && e->local_closed);
    memcpy(saved,e->send,sizeof saved);
    CHECK(SSL_shutdown(c->ssl)==0); receive_only(c);
    CHECK(e->peer_closed && e->send_used==send_used && !memcmp(saved,e->send,sizeof saved));
    CHECK(os64_tls_state(c->client).status==OS64_TLS_OK);
    size_t n=drain(c->client,wire,sizeof wire); CHECK(n==send_used);
    CHECK(BIO_write(c->in,wire,(int)n)==(int)n && SSL_shutdown(c->ssl)==1);
    CHECK(os64_tls_state(c->client).status==OS64_TLS_CLEAN_EOF);
    destroy(c);
    puts("PASS peer close while local close_notify is pending, with output preserved until drained");
}
static void fallback(void)
{
    for (unsigned rsa=0;rsa<2;rsa++) {
        const char *kind=rsa?"rsa":"ec", *suite=rsa?"ECDHE-RSA-AES128-GCM-SHA256":"ECDHE-ECDSA-AES128-GCM-SHA256";
        connection *c=connect_peer(kind,suite,"P-256",OS64_TLS_PROTOCOL_DEFAULT,TLS1_2_VERSION,37);
        if (rsa) CHECK(SSL_set1_sigalgs_list(c->ssl,"rsa_pkcs1_sha256"));
        for (unsigned i=0;i<20000 && os64_tls_state(c->client).status==OS64_TLS_OK;i++) drive(c);
        os64_tls_state_t state=os64_tls_state(c->client);
        CHECK(state.status==OS64_TLS_PEER_CHOSE_TLS12 && state.version==0x0303 && !(state.flags&OS64_TLS_HANDSHAKE_DONE));
        unsigned seed=c->seed; destroy(c);
        c=connect_peer(kind,suite,"P-256",OS64_TLS_PROTOCOL_TLS12_FALLBACK,TLS1_2_VERSION,37);
        if (rsa) CHECK(SSL_set1_sigalgs_list(c->ssl,"rsa_pkcs1_sha256"));
        CHECK(seed!=c->seed); handshake_done(c); exchange(c,8193); close_connection(c,false); destroy(c);
    }
    puts("PASS real TLS 1.2 first response, PKCS#1-only RSA and fresh authenticated Bear connections");
}
static void failures(void)
{
    // The constructor has an engine allocation followed by its owned validator.
    size_t baseline=live;
    const os64_tls_name_t alpn_name={"http/1.1",8};
    os64_tls_config_t cfg={.hostname={"example.test",12},.alpn=&alpn_name,.alpn_count=1,.trust=trust};
    for (size_t i=1;i<=2;i++) {
        fail_allocation=allocation_calls+i; os64_tls_client *c=(void *)1; next_engine=true;
        CHECK(os64_tls_client_create(&cfg,&c)==OS64_TLS_NO_MEMORY && !c && live==baseline);
        fail_allocation=0;
    }
    for (unsigned kind=0;kind<3;kind++) {
        if (kind==1) epoch=2208988800;
        connection *c=connect_peer("ec","TLS_AES_128_GCM_SHA256","X25519",OS64_TLS_PROTOCOL_DEFAULT,TLS1_3_VERSION,37);
        epoch=1791504000;
        if (kind==0) { os64_tls_free(c->client); c->client=new_client(OS64_TLS_PROTOCOL_DEFAULT,"wrong.test"); }
        if (kind==2) {
            os64_tls_trust *saved=trust; root_file("other.der");
            os64_tls_free(c->client); c->client=new_client(OS64_TLS_PROTOCOL_DEFAULT,"example.test");
            os64_tls_trust_free(trust); trust=saved;
        }
        for (unsigned i=0;i<100000 && os64_tls_state(c->client).status==OS64_TLS_OK;i++) drive(c);
        os64_tls_state_t s=os64_tls_state(c->client);
        CHECK(s.status==OS64_TLS_CERTIFICATE && !(s.flags&(OS64_TLS_HANDSHAKE_DONE|OS64_TLS_RECV_PLAIN)));
        CHECK(os64_tls_abort(c->client,OS64_TLS_CANCELLED)==OS64_TLS_CERTIFICATE);
        destroy(c);
    }
    connection *a=connect_peer("ec","TLS_AES_128_GCM_SHA256","X25519",OS64_TLS_PROTOCOL_DEFAULT,TLS1_3_VERSION,37);
    connection *b=connect_peer("rsa","TLS_AES_256_GCM_SHA384","P-384",OS64_TLS_PROTOCOL_DEFAULT,TLS1_3_VERSION,37);
    for (unsigned i=0;i<100;i++) { drive(a); drive(b); }
    CHECK(os64_tls_abort(a->client,OS64_TLS_CANCELLED)==OS64_TLS_CANCELLED);
    handshake_done(b); exchange(b,1025); destroy(a); destroy(b);
    os64_tls_client *c=new_client(OS64_TLS_PROTOCOL_DEFAULT,"example.test");
    CHECK(os64_tls_input_eof(c)==OS64_TLS_OK); unsigned char out[32768];
    drain(c,out,sizeof out); CHECK(os64_tls_state(c).status==OS64_TLS_TRUNCATED); os64_tls_free(c);
    CHECK(live==baseline);
    puts("PASS allocation failures/wiping, certificate gate, independent connections, abort and handshake EOF");
}
typedef struct {
    size_t at, wire_len, plain_len;
    bool bypass;
    unsigned char plain[16384];
} replay_record;
typedef struct {
    unsigned seed; size_t raw_len, records;
    unsigned char raw[131072]; replay_record record[16];
} replay;
static replay *capture(const char *kind, const char *suite, const char *group)
{
    connection *c=connect_peer(kind,suite,group,OS64_TLS_PROTOCOL_DEFAULT,TLS1_3_VERSION,4096);
    if (!strcmp(kind,"rsa")) SSL_set_verify(c->ssl,SSL_VERIFY_PEER,NULL);
    handshake_done(c);
    CHECK(SSL_key_update(c->ssl,SSL_KEY_UPDATE_REQUESTED)); ssl_result(c,SSL_do_handshake(c->ssl));
    for (unsigned i=0;i<30;i++) { unsigned char b; drive(c); ssl_result(c,SSL_read(c->ssl,&b,1)); }
    replay *r=calloc(1,sizeof *r); CHECK(r); r->seed=c->seed; r->raw_len=c->captured_len;
    memcpy(r->raw,c->captured,r->raw_len);
    const tls13_suite *s=tls13_suite_find(SSL_CIPHER_get_protocol_id(SSL_get_current_cipher(c->ssl)));
    CHECK(s && c->secret_len==s->hash_len);
    unsigned char key[32],iv[12]; tls13_record decoder={0};
    CHECK(!tls13_traffic_keys(s,c->hs_secret,key,iv)); CHECK(!tls13_record_init(&decoder,s,key,iv));
    for (size_t at=0;at<r->raw_len;) {
        CHECK(r->raw_len-at>=5 && r->records<16);
        replay_record *rec=&r->record[r->records++]; rec->at=at;
        rec->wire_len=5+((size_t)r->raw[at+3]<<8)+r->raw[at+4]; CHECK(rec->wire_len<=r->raw_len-at);
        if (r->raw[at]==23) {
            unsigned char wire[TLS13_RECV_MAX]; memcpy(wire,r->raw+at,rec->wire_len);
            unsigned type; CHECK(!tls13_record_open(&decoder,wire,rec->wire_len,true,&type,&rec->plain_len));
            CHECK(type==TLS13_HANDSHAKE && rec->plain_len<=sizeof rec->plain);
            memcpy(rec->plain,wire+5,rec->plain_len); rec->bypass=true;
            for (size_t pos=0;pos+4<=rec->plain_len;) {
                unsigned msg=rec->plain[pos]; size_t n=((size_t)rec->plain[pos+1]<<16)+((size_t)rec->plain[pos+2]<<8)+rec->plain[pos+3];
                CHECK(n<=rec->plain_len-pos-4); pos+=4+n;
                if (msg==24) {
                    CHECK(!tls13_update_secret(s,c->app_secret));
                    CHECK(!tls13_traffic_keys(s,c->app_secret,key,iv)); CHECK(!tls13_record_init(&decoder,s,key,iv));
                }
                if (msg==20) {
                    CHECK(!tls13_traffic_keys(s,c->app_secret,key,iv)); CHECK(!tls13_record_init(&decoder,s,key,iv));
                }
            }
        }
        at+=rec->wire_len;
    }
    destroy(c); return r;
}
// Protected mutations use the live receive key to authenticate deliberately
// malformed plaintext. This exercises the public record path as well as parsing.
static void protected_input(os64_tls_client *c, unsigned type, const void *p, size_t n)
{
    unsigned char wire[TLS13_SEND_MAX]; size_t length;
    tls13_record encoder=inner(c)->read_key;
    CHECK(n<=TLS13_CONTENT_MAX); memcpy(wire+5,p,n);
    CHECK(!tls13_record_seal(&encoder,type,wire,n,sizeof wire,&length));
    CHECK(feed(c,wire,length)==length);
}
static os64_tls_client *before_message(replay *r, unsigned wanted, const unsigned char **body, size_t *length)
{
    entropy_serial=r->seed-1;
    os64_tls_client *c=new_client(OS64_TLS_PROTOCOL_DEFAULT,"example.test"); unsigned char out[32768];
    CHECK(drain(c,out,sizeof out));
    for (size_t i=0;i<r->records;i++) {
        replay_record *rec=&r->record[i];
        if (!rec->bypass) CHECK(feed(c,r->raw+rec->at,rec->wire_len)==rec->wire_len);
        else for (size_t at=0;at<rec->plain_len;) {
            const unsigned char *p=rec->plain+at;
            size_t n=4+((size_t)p[1]<<16)+((size_t)p[2]<<8)+p[3]; CHECK(n<=rec->plain_len-at);
            if (p[0]==wanted) { *body=p; *length=n; return c; }
            protected_input(c,TLS13_HANDSHAKE,p,n); at+=n;
        }
        drain(c,out,sizeof out); CHECK(os64_tls_state(c).status==OS64_TLS_OK);
    }
    CHECK(0); return NULL;
}
static void refused(os64_tls_client *c, os64_tls_status_t status, int error)
{
    os64_tls_state_t s=os64_tls_state(c); CHECK(s.status==status);
    if (error && s.upstream_error!=error) fprintf(stderr,"refusal error actual=%d expected=%d\n",s.upstream_error,error);
    if (error) CHECK(s.upstream_error==error);
    unsigned char out[32768];
    CHECK(os64_tls_abort(c,OS64_TLS_CANCELLED)==status);
    CHECK(os64_tls_write_plaintext(c,"x",1).transferred==0);
    drain(c,out,sizeof out); CHECK(os64_tls_state(c).status==status); os64_tls_free(c);
}
static void put_u16(unsigned char *p,size_t n) { p[0]=(unsigned char)(n>>8); p[1]=(unsigned char)n; }
static size_t scripted_hello(os64_tls_client *c, unsigned char *wire, bool retry, int group, bool cookie, bool modern)
{
    static const unsigned char hrr[]={0xcf,0x21,0xad,0x74,0xe5,0x9a,0x61,0x11,0xbe,0x1d,0x8c,0x02,0x1e,0x65,0xb8,0x91,
        0xc2,0xa2,0x11,0x16,0x7a,0xbb,0x8c,0x5e,0x07,0x9e,0x09,0xe2,0xc8,0xa8,0x33,0x9c};
    memset(wire,0,256); wire[0]=22; wire[1]=wire[2]=3; wire[5]=2; wire[9]=wire[10]=3;
    if (retry) memcpy(wire+11,hrr,32);
    wire[43]=32; memcpy(wire+44,inner(c)->session,32); put_u16(wire+76,modern?0x1301:0xc02b);
    size_t at=81;
    if (modern) { put_u16(wire+at,43); put_u16(wire+at+2,2); put_u16(wire+at+4,0x0304); at+=6; }
    if (group>=0) { put_u16(wire+at,51); put_u16(wire+at+2,2); put_u16(wire+at+4,(unsigned)group); at+=6; }
    if (cookie) { put_u16(wire+at,44); put_u16(wire+at+2,5); put_u16(wire+at+4,3); memcpy(wire+at+6,"yum",3); at+=9; }
    put_u16(wire+79,at-81); put_u16(wire+3,at-5); put_u16(wire+7,at-9); return at;
}
static void fallback_downgrade(void)
{
    for (unsigned rsa=0;rsa<2;rsa++) {
        // An unauthenticated legacy response may trigger a fresh connection,
        // but that connection must retain the client's TLS 1.3 capability.
        os64_tls_client *first=new_client(OS64_TLS_PROTOCOL_DEFAULT,"example.test");
        unsigned seed=entropy_serial;
        unsigned char wire[32768],random[32],plain[32];
        CHECK(drain(first,wire,sizeof wire));
        size_t n=scripted_hello(first,wire,false,-1,false,false);
        CHECK(feed(first,wire,n)==n);
        CHECK(os64_tls_state(first).status==OS64_TLS_PEER_CHOSE_TLS12);
        os64_tls_free(first);
        const char *kind=rsa?"rsa":"ec", *suite=rsa?"ECDHE-RSA-AES128-GCM-SHA256":"ECDHE-ECDSA-AES128-GCM-SHA256";
        connection *c=connect_peer(kind,suite,"P-256",OS64_TLS_PROTOCOL_TLS12_FALLBACK,TLS1_2_VERSION,rsa?37:1);
        CHECK(seed!=c->seed && SSL_set_max_proto_version(c->ssl,TLS1_3_VERSION));
        os64_tls_state_t state=os64_tls_state(c->client);
        for (unsigned i=0;i<100000 && state.status==OS64_TLS_OK;i++) {
            drive(c); state=os64_tls_state(c->client);
            CHECK(!(state.flags&(OS64_TLS_HANDSHAKE_DONE|OS64_TLS_SEND_PLAIN|OS64_TLS_RECV_PLAIN)));
        }
        CHECK(!c->server_error && SSL_is_init_finished(c->ssl));
        CHECK(SSL_get_server_random(c->ssl,random,sizeof random)==sizeof random);
        CHECK(!memcmp(random+24,"DOWNGRD\1",8));
        CHECK(state.status==OS64_TLS_PROTOCOL && state.upstream_error==TLS13_ERR_DOWNGRADE && state.version==0x0303);
        os64_tls_transfer_t moved=os64_tls_write_plaintext(c->client,"GET /",5);
        CHECK(moved.status==OS64_TLS_PROTOCOL && !moved.transferred);
        moved=os64_tls_read_plaintext(c->client,plain,sizeof plain);
        CHECK(moved.status==OS64_TLS_PROTOCOL && !moved.transferred);
        CHECK(os64_tls_input_eof(c->client)==OS64_TLS_PROTOCOL);
        CHECK(os64_tls_abort(c->client,OS64_TLS_CANCELLED)==OS64_TLS_PROTOCOL);
        CHECK(os64_tls_state(c->client).upstream_error==TLS13_ERR_DOWNGRADE);
        destroy(c);
        // Explicit TLS 1.2 selection is a caller policy, not a fallback retry.
        c=connect_peer(kind,suite,"P-256",OS64_TLS_PROTOCOL_TLS12,TLS1_2_VERSION,37);
        CHECK(SSL_set_max_proto_version(c->ssl,TLS1_3_VERSION));
        handshake_done(c); exchange(c,257); close_connection(c,false); destroy(c);
    }
    puts("PASS forged legacy trigger followed by authenticated downgrade refusal, no plaintext, sticky status and explicit TLS 1.2 control");
}
static void hello_cases(void)
{
    unsigned char out[32768],first[32768],wire[256];
    for (unsigned mode=0;mode<9;mode++) {
        os64_tls_client *c=new_client(OS64_TLS_PROTOCOL_DEFAULT,"example.test");
        size_t first_len=drain(c,first,sizeof first); CHECK(first_len>100);
        bool modern=mode<6; size_t n=scripted_hello(c,wire,modern,mode==1?29:mode==2?25:-1,mode==0 || mode==3 || mode==4,modern);
        if (mode==4) wire[44]^=1;
        if (mode==7 || mode==8) memcpy(wire+11+24,mode==7?"DOWNGRD\x01":"DOWNGRD\x00",8);
        CHECK(feed(c,wire,n)==n);
        if (mode==0 || mode==3) {
            CHECK(os64_tls_state(c).status==OS64_TLS_OK && inner(c)->retry);
            size_t second_len=drain(c,out,sizeof out); CHECK(second_len==first_len+6+9);
            CHECK(!memcmp(out,"\24\3\3\0\1\1",6));
            // Cookie-only HRR preserves both offered shares and every unchanged
            // hello field, including random/session, suites and extension order.
            CHECK(!memcmp(first+9,out+15,98-9));
            CHECK(!memcmp(first+100,out+106,first_len-100));
            CHECK(!memcmp(out+6+first_len,"\0\54\0\5\0\3yum",9));
            CHECK(feed(c,wire,n)==n); refused(c,OS64_TLS_PROTOCOL,TLS13_ERR_SECOND_HRR);
        } else if (mode==6) refused(c,OS64_TLS_PEER_CHOSE_TLS12,TLS13_ERR_PEER_TLS12);
        else refused(c,OS64_TLS_PROTOCOL,mode==1 || mode==5?TLS13_ERR_RETRY:mode==2?TLS13_ERR_KEY_SHARE:
            mode==4?TLS13_ERR_SESSION_ID:TLS13_ERR_DOWNGRADE);
    }
    puts("PASS cookie-only HRR preservation, repeated/unchanged/unsupported HRR, session echo and downgrade sentinels");
}
static void adversarial(void)
{
    const char *suites[]={"TLS_AES_128_GCM_SHA256","TLS_AES_256_GCM_SHA384","TLS_CHACHA20_POLY1305_SHA256"};
    size_t baseline=live; unsigned char data[16384],out[32768];
    hello_cases();
    for (unsigned suite=0;suite<3;suite++) {
        replay *r=capture(suite==1?"rsa":"ec",suites[suite],"X25519");
        for (unsigned test=0;test<17;test++) {
            unsigned target=test<3?15:test<6?20:test<12?8:24;
            const unsigned char *p; size_t n;
            os64_tls_client *c=before_message(r,target,&p,&n);
            memcpy(data,p,n);
            if (test==0 || test==3) { data[n-1]^=1; protected_input(c,22,data,n); refused(c,OS64_TLS_PROTOCOL,test?BR_ERR_BAD_FINISHED:BR_ERR_BAD_SIGNATURE); }
            else if (test==1) { put_u16(data+4,0x0401); protected_input(c,22,data,n); refused(c,OS64_TLS_PROTOCOL,TLS13_ERR_PKCS1); }
            else if (test==2) { put_u16(data+4,suite==1?0x0403:0x0804); protected_input(c,22,data,n); refused(c,OS64_TLS_PROTOCOL,TLS13_ERR_SIGNATURE_SCHEME); }
            else if (test==4 || test==12) { data[n++]=0; protected_input(c,22,data,n); refused(c,OS64_TLS_PROTOCOL,BR_ERR_UNEXPECTED); }
            else if (test==5) { protected_input(c,23,"early",5); refused(c,OS64_TLS_PROTOCOL,BR_ERR_UNEXPECTED); }
            else if (test==6) {
                const unsigned char groups[]={8,0,0,8,0,6,0,10,0,2,0,0};
                protected_input(c,22,groups,sizeof groups); refused(c,OS64_TLS_PROTOCOL,BR_ERR_BAD_HANDSHAKE);
            } else if (test==7) {
                const unsigned char groups[]={8,0,0,10,0,8,0,10,0,4,0,2,0xfa,0xfa};
                protected_input(c,22,groups,sizeof groups); CHECK(os64_tls_state(c).status==OS64_TLS_OK); os64_tls_free(c);
            } else if (test==8) {
                const unsigned char alpn_bad[]={8,0,0,11,0,9,0,16,0,5,0,3,2,'h','2'};
                protected_input(c,22,alpn_bad,sizeof alpn_bad); refused(c,OS64_TLS_PROTOCOL,TLS13_ERR_ALPN);
            } else if (test==9) {
                protected_input(c,22,p,2); const unsigned char alert[]={1,90};
                protected_input(c,21,alert,2); refused(c,OS64_TLS_PROTOCOL,BR_ERR_UNEXPECTED);
            } else if (test==10) {
                const unsigned char large[]={8,0,0x20,0}; protected_input(c,22,large,4); refused(c,OS64_TLS_LIMIT,BR_ERR_TOO_LARGE);
            } else if (test==11) {
                // Split the handshake header and body across authenticated records.
                for (size_t j=0;j<n;j++) protected_input(c,22,p+j,1);
                CHECK(os64_tls_state(c).status==OS64_TLS_OK); os64_tls_free(c);
            } else if (test==13) {
                data[4]=2; protected_input(c,22,data,n); refused(c,OS64_TLS_PROTOCOL,BR_ERR_BAD_HANDSHAKE);
            } else if (test==14) {
                const unsigned char ccs[]={20,3,3,0,1,1}; CHECK(feed(c,ccs,sizeof ccs)==sizeof ccs);
                refused(c,OS64_TLS_PROTOCOL,BR_ERR_BAD_CCS);
            } else if (test==15) {
                // A reply to requested KeyUpdate precedes already accepted app bytes.
                CHECK(os64_tls_write_plaintext(c,"held",4).transferred==4);
                tls13_record old=inner(c)->write_key; protected_input(c,22,p,n);
                size_t length=drain(c,out,sizeof out); unsigned type; size_t plain;
                CHECK(!tls13_record_open(&old,out,length,true,&type,&plain));
                CHECK(type==22 && plain==5 && out[5]==24 && out[9]==0);
                CHECK(os64_tls_flush(c)==OS64_TLS_OK); length=drain(c,out,sizeof out);
                tls13_record fresh=inner(c)->write_key; fresh.sequence=0;
                CHECK(!tls13_record_open(&fresh,out,length,true,&type,&plain));
                CHECK(type==23 && plain==4 && !memcmp(out+5,"held",4)); os64_tls_free(c);
            } else {
                protected_input(c,23,"held",4); CHECK(os64_tls_input_eof(c)==OS64_TLS_OK);
                CHECK(os64_tls_read_plaintext(c,out,2).transferred==2 && !memcmp(out,"he",2));
                CHECK(os64_tls_read_plaintext(c,out,2).transferred==2 && !memcmp(out,"ld",2));
                refused(c,OS64_TLS_TRUNCATED,0);
            }
        }
        free(r);
    }
    // Accepted bytes are bounded by the wrapper's pre-authentication work cap.
    os64_tls_client *c=new_client(OS64_TLS_PROTOCOL_DEFAULT,"example.test"); drain(c,out,sizeof out);
    const unsigned char warning[]={21,3,3,0,2,1,90};
    size_t accepted=0;
    for (size_t i=0;i<(1048576/7)+2;i++) {
        accepted+=feed(c,warning,sizeof warning);
        if (os64_tls_state(c).status!=OS64_TLS_OK) break;
    }
    CHECK(accepted==1048576); refused(c,OS64_TLS_LIMIT,0); CHECK(live==baseline);
    puts("PASS authenticated malformed messages, fragmented headers, Finished gate, KeyUpdate ordering, EOF drain and handshake cap");
}

static unsigned read16(const unsigned char *p) { return ((unsigned)p[0]<<8)|p[1]; }
static void client_hello_offer(void)
{
    os64_tls_client *c=new_client(OS64_TLS_PROTOCOL_DEFAULT,"example.test"); unsigned char b[32768];
    size_t n=drain(c,b,sizeof b); CHECK(n>100 && b[0]==22 && b[5]==1 && read16(b+9)==0x0303);
    CHECK(b[43]==32 && read16(b+76)==18 && b[96]==1 && b[97]==0 && read16(b+98)==n-100);
    const unsigned suites[]={0x1301,0x1302,0x1303,0xcca9,0xcca8,0xc02b,0xc02f,0xc02c,0xc030};
    for (size_t i=0;i<9;i++) CHECK(read16(b+78+2*i)==suites[i]);
    const unsigned exts[]={0,10,13,50,16,43,51}; size_t at=100;
    for (size_t i=0;i<7;i++) {
        CHECK(at+4<=n && read16(b+at)==exts[i]); size_t length=read16(b+at+2); const unsigned char *p=b+at+4;
        CHECK(length<=n-at-4);
        if (i==0) CHECK(length==17 && !memcmp(p,"\0\17\0\0\14example.test",17));
        if (i==1) CHECK(length==8 && !memcmp(p,"\0\6\0\35\0\27\0\30",8));
        if (i==2) CHECK(length==20 && !memcmp(p,"\0\22\4\3\5\3\6\3\10\4\10\5\10\6\4\1\5\1\6\1",20));
        if (i==3) CHECK(length==14 && !memcmp(p,"\0\14\4\3\5\3\6\3\4\1\5\1\6\1",14));
        if (i==4) CHECK(length==11 && !memcmp(p,"\0\11\10http/1.1",11));
        if (i==5) CHECK(length==5 && !memcmp(p,"\4\3\4\3\3",5));
        if (i==6) CHECK(length==107 && read16(p)==105 && read16(p+2)==29 && read16(p+4)==32 &&
            read16(p+38)==23 && read16(p+40)==65 && p[42]==4);
        at+=4+length;
    }
    CHECK(at==n); os64_tls_free(c);
    puts("PASS ClientHello suites, signatures, extension order, ALPN, versions and both key shares");
}
// OpenSSL seals arbitrary inner plaintext, including padding larger than the
// production encoder permits. Thus receive-limit tests carry a valid tag.
static void arbitrary_inner(os64_tls_client *c, const unsigned char *plain, size_t length, bool corrupt)
{
    tls13_record *r=&inner(c)->read_key; unsigned char wire[TLS13_RECV_MAX],nonce[12];
    CHECK(length+21<=sizeof wire); memcpy(nonce,r->iv,12);
    for (unsigned i=0;i<8;i++) nonce[11-i]^=(unsigned char)(r->sequence>>(8*i));
    wire[0]=23; wire[1]=wire[2]=3; put_u16(wire+3,length+16);
    const EVP_CIPHER *cipher=r->suite->chacha?EVP_chacha20_poly1305():r->suite->key_len==16?EVP_aes_128_gcm():EVP_aes_256_gcm();
    EVP_CIPHER_CTX *ctx=EVP_CIPHER_CTX_new(); CHECK(ctx); int done,tail;
    CHECK(EVP_EncryptInit_ex(ctx,cipher,NULL,r->key,nonce));
    CHECK(EVP_EncryptUpdate(ctx,NULL,&done,wire,5));
    CHECK(EVP_EncryptUpdate(ctx,wire+5,&done,plain,(int)length) && done==(int)length);
    CHECK(EVP_EncryptFinal_ex(ctx,wire+5+done,&tail) && !tail);
    CHECK(EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_AEAD_GET_TAG,16,wire+5+length)); EVP_CIPHER_CTX_free(ctx);
    if (corrupt) wire[5]^=1;
    CHECK(feed(c,wire,21+length)==21+length);
}
static void record_cases(void)
{
    const char *suites[]={"TLS_AES_128_GCM_SHA256","TLS_AES_256_GCM_SHA384","TLS_CHACHA20_POLY1305_SHA256"};
    for (unsigned suite=0;suite<3;suite++) {
        replay *r=capture("ec",suites[suite],"X25519");
        for (unsigned test=0;test<8;test++) {
            const unsigned char *p; size_t n; unsigned char b[16386]={0},out[32768];
            os64_tls_client *c=before_message(r,test==4?4:24,&p,&n);
            if (test<3) {
                b[0]='X'; b[1]=23; arbitrary_inner(c,b,test==1?16386:16385,test==2);
                if (test==0) { CHECK(os64_tls_read_plaintext(c,out,1).transferred==1 && out[0]=='X'); os64_tls_free(c); }
                else refused(c,test==1?OS64_TLS_LIMIT:OS64_TLS_PROTOCOL,test==1?BR_ERR_TOO_LARGE:BR_ERR_BAD_MAC);
            } else if (test==3) { b[0]=1; b[1]=20; arbitrary_inner(c,b,2,false); refused(c,OS64_TLS_PROTOCOL,BR_ERR_BAD_CCS); }
            else if (test==4) { protected_input(c,22,p,5); protected_input(c,23,"x",1); refused(c,OS64_TLS_PROTOCOL,BR_ERR_UNEXPECTED); }
            else if (test==5) {
                CHECK(os64_tls_write_plaintext(c,"pending",7).transferred==7);
                CHECK(os64_tls_input_eof(c)==OS64_TLS_OK); CHECK(drain(c,out,sizeof out)>0);
                refused(c,OS64_TLS_TRUNCATED,0);
            } else if (test==6) {
                tls13_record key=inner(c)->write_key; key.sequence=TLS13_UPDATE_RECORDS;
                inner(c)->write_key.sequence=TLS13_UPDATE_RECORDS;
                CHECK(drain(c,out,sizeof out)==27); unsigned type; size_t plain;
                CHECK(!tls13_record_open(&key,out,27,true,&type,&plain) && type==22 && plain==5 && out[5]==24);
                CHECK(inner(c)->write_key.sequence==0); os64_tls_free(c);
            } else {
                CHECK(os64_tls_begin_close(c)==OS64_TLS_OK); CHECK(drain(c,out,sizeof out)>0);
                protected_input(c,23,"late",4); CHECK(os64_tls_read_plaintext(c,out,4).transferred==0);
                const unsigned char close_alert[]={1,0}; protected_input(c,21,close_alert,2);
                CHECK(os64_tls_state(c).status==OS64_TLS_CLEAN_EOF); os64_tls_free(c);
            }
        }
        free(r);
    }
    // A real OpenSSL peer emits its downgrade marker after a proxy strips 1.3.
    connection *c=connect_peer("ec","TLS_AES_128_GCM_SHA256","X25519",OS64_TLS_PROTOCOL_DEFAULT,TLS1_3_VERSION,4096);
    CHECK(SSL_set_min_proto_version(c->ssl,TLS1_2_VERSION)); unsigned char wire[32768];
    size_t n=drain(c->client,wire,sizeof wire),at=100;
    while (at<n && read16(wire+at)!=43) at+=4+read16(wire+at+2);
    CHECK(at+9<=n); wire[at+6]=3; wire[at+8]=2;
    CHECK(BIO_write(c->in,wire,(int)n)==(int)n); ssl_result(c,SSL_do_handshake(c->ssl));
    int length=BIO_read(c->out,wire,sizeof wire); CHECK(length>43 && !memcmp(wire+35,"DOWNGRD\1",8));
    CHECK(feed(c->client,wire,(size_t)length)>0);
    CHECK(os64_tls_state(c->client).upstream_error==TLS13_ERR_DOWNGRADE); destroy(c);
    c=connect_peer("ec","TLS_AES_128_GCM_SHA256","X25519",OS64_TLS_PROTOCOL_DEFAULT,TLS1_3_VERSION,4096);
    SSL_clear_options(c->ssl,SSL_OP_ENABLE_MIDDLEBOX_COMPAT); SSL_set_num_tickets(c->ssl,0);
    handshake_done(c); exchange(c,16385); close_connection(c,false); destroy(c);
    c=connect_peer("rsa","TLS_AES_128_GCM_SHA256","X25519",OS64_TLS_PROTOCOL_DEFAULT,TLS1_3_VERSION,4096);
    SSL_set_verify(c->ssl,SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT,NULL);
    for (unsigned i=0;i<10000 && os64_tls_state(c->client).status==OS64_TLS_OK;i++) drive(c);
    CHECK(os64_tls_state(c->client).status==OS64_TLS_PROTOCOL && os64_tls_state(c->client).upstream_error==BR_ERR_RECV_FATAL_ALERT+116);
    destroy(c); ERR_clear_error();
    puts("PASS inner padding bounds, bad tags/CCS, ticket interleave, write exhaustion, EOF/close, real downgrade and client-cert alert");
}

static void framing_cases(void)
{
    replay *r=capture("rsa","TLS_AES_128_GCM_SHA256","P-384"); unsigned char wire[32768],out[32768];
    for (unsigned test=0;test<9;test++) {
        const unsigned char *p; size_t n;
        os64_tls_client *c=before_message(r,test<3 || test==8?11:test<5?13:4,&p,&n);
        memcpy(wire,p,n);
        if (test==8) {
            char file[1024]; path(file,"pss.der"); FILE *f=fopen(file,"rb"); CHECK(f);
            size_t der=fread(wire+11,1,4096,f); CHECK(der && !ferror(f)); fclose(f);
            wire[0]=11; wire[1]=0; put_u16(wire+2,der+9); wire[4]=0;
            wire[5]=0; put_u16(wire+6,der+5); wire[8]=0; put_u16(wire+9,der);
            wire[11+der]=wire[12+der]=0; protected_input(c,22,wire,der+13);
            CHECK(os64_tls_state(c).policy_reason==OS64_TLS_POLICY_SIGNATURE);
            refused(c,OS64_TLS_CERTIFICATE,0);
        } else if (test==0) {
            for (size_t i=0;i<n;i++) protected_input(c,22,p+i,1);
            CHECK(os64_tls_state(c).status==OS64_TLS_OK); os64_tls_free(c);
        } else if (test==1) { wire[n-1]=1; protected_input(c,22,wire,n); refused(c,OS64_TLS_PROTOCOL,BR_ERR_EXTRA_EXTENSION); }
        else if (test==2 || test==3) { wire[4]=1; protected_input(c,22,wire,n); refused(c,OS64_TLS_PROTOCOL,BR_ERR_BAD_HANDSHAKE); }
        else if (test==4) {
            const unsigned char empty_request[]={13,0,0,3,0,0,0}; protected_input(c,22,empty_request,sizeof empty_request);
            refused(c,OS64_TLS_PROTOCOL,BR_ERR_BAD_HANDSHAKE);
        } else {
            unsigned char ticket[]={4,0,0,22, 0,0,0,1,0,0,0,0,0, 0,1,42, 0,8, 0xfa,0xfa,0,0, 0xfa,0xfa,0,0};
            if (test==5) ticket[23]=0xfb;
            if (test==7) { ticket[18]=0; ticket[19]=42; }
            protected_input(c,22,ticket,sizeof ticket);
            if (test==5) { CHECK(os64_tls_state(c).status==OS64_TLS_OK); os64_tls_free(c); }
            else refused(c,OS64_TLS_PROTOCOL,BR_ERR_BAD_HANDSHAKE);
        }
    }
    // HRR commits the suite. The final ServerHello cannot change it, and its
    // last byte must also be the last byte of its plaintext record.
    for (unsigned test=0;test<2;test++) {
        entropy_serial=r->seed-1; os64_tls_client *c=new_client(OS64_TLS_PROTOCOL_DEFAULT,"example.test");
        drain(c,out,sizeof out); bool changed=false;
        for (size_t i=0;i<r->records && !changed;i++) {
            replay_record *rec=&r->record[i]; CHECK(!rec->bypass);
            memcpy(wire,r->raw+rec->at,rec->wire_len); size_t n=rec->wire_len;
            if (wire[0]==22 && inner(c)->retry) {
                if (test==0) put_u16(wire+76,0x1302);
                else { wire[n++]=0; put_u16(wire+3,n-5); }
                changed=true;
            }
            CHECK(feed(c,wire,n)==n); if (!changed) drain(c,out,sizeof out);
        }
        CHECK(changed && !inner(c)->read_encrypted);
        refused(c,OS64_TLS_PROTOCOL,test==0?BR_ERR_BAD_CIPHER_SUITE:BR_ERR_UNEXPECTED);
    }
    free(r); puts("PASS streamed certificate framing, CertificateRequest, ticket extensions and ServerHello epoch boundaries");
}

static void signature_schemes(void)
{
    const char *kinds[]={"ec384","ec521","rsa","rsa"};
    const char *schemes[]={"ecdsa_secp384r1_sha384","ecdsa_secp521r1_sha512","rsa_pss_rsae_sha384","rsa_pss_rsae_sha512"};
    for (unsigned i=0;i<4;i++) {
        connection *c=connect_peer(kinds[i],"TLS_AES_128_GCM_SHA256","X25519",OS64_TLS_PROTOCOL_DEFAULT,TLS1_3_VERSION,37);
        CHECK(SSL_set1_sigalgs_list(c->ssl,schemes[i]));
        handshake_done(c); exchange(c,257); close_connection(c,false); destroy(c);
    }
    puts("PASS ECDSA P-384/P-521 and RSA-PSS SHA-384/SHA-512 CertificateVerify with a SHA-256 cipher suite");
}

static unsigned fuzz_random(unsigned *state)
{ unsigned x=*state; x^=x<<13; x^=x>>17; x^=x<<5; return *state=x; }
static void fuzz(unsigned seconds, bool replay_case, unsigned replay_seed, size_t replay_iteration)
{
    const char *suites[]={"TLS_AES_128_GCM_SHA256","TLS_AES_256_GCM_SHA384","TLS_CHACHA20_POLY1305_SHA256"};
    replay *corpus[12]; size_t count=0;
    for (unsigned s=0;s<3;s++) for (unsigned k=0;k<2;k++) for (unsigned h=0;h<2;h++) {
        char file[1024],name[64]; snprintf(name,sizeof name,"flight-%zu.replay",count); path(file,name);
        FILE *f=fopen(file,replay_case?"rb":"wb"); CHECK(f);
        if (replay_case) {
            corpus[count]=malloc(sizeof(replay)); CHECK(corpus[count]);
            CHECK(fread(corpus[count],sizeof(replay),1,f)==1);
        } else {
            corpus[count]=capture(k?"rsa":"ec",suites[s],h?"P-384":"X25519");
            CHECK(fwrite(corpus[count],sizeof(replay),1,f)==1);
        }
        fclose(f);
        if (!replay_case) {
            snprintf(name,sizeof name,"flight-%zu.bin",count); path(file,name);
            f=fopen(file,"wb"); CHECK(f);
            CHECK(fwrite(corpus[count]->raw,1,corpus[count]->raw_len,f)==corpus[count]->raw_len); fclose(f);
        }
        count++;
    }
    unsigned random=replay_case?replay_seed:0x24113;
    size_t iterations=replay_case?replay_iteration:0,raw_cases=0,deep_cases=0,terminals=0;
    char marker[1024]; path(marker,"fuzz-current.txt");
    uint64_t coverage=0; time_t start=time(NULL); size_t baseline=live;
    unsigned char *bytes=malloc(131073),out[32768]; CHECK(bytes);
    do {
        unsigned seed=random;
        // Persist the next case before executing it, including for sanitizer
        // failures that bypass CHECK. Keep the corpus and fixture directory.
        FILE *current=fopen(marker,"w"); CHECK(current);
        CHECK(fprintf(current,"%u %zu\n",seed,iterations)>0); CHECK(!fclose(current));
        replay *r=corpus[fuzz_random(&random)%count];
        entropy_serial=r->seed-1; os64_tls_client *client=new_client(OS64_TLS_PROTOCOL_DEFAULT,"example.test");
        CHECK(drain(client,out,sizeof out)>0);
        bool deep=(iterations&1)!=0;
        size_t target=fuzz_random(&random)%r->records;
        if (!deep) {
            memcpy(bytes,r->raw,r->raw_len); size_t length=r->raw_len;
            unsigned mode=fuzz_random(&random)%3;
            if (mode==0) length=fuzz_random(&random)%(length+1);
            else for (unsigned i=0;i<1+(seed%4);i++) bytes[fuzz_random(&random)%length]^=(unsigned char)(1u<<(fuzz_random(&random)%8));
            size_t at=0;
            while (at<length && os64_tls_state(client).status==OS64_TLS_OK) {
                size_t n=length-at; if (n>4096) n=4096;
                size_t used=feed(client,bytes+at,n); at+=used;
                size_t sent=drain(client,out,sizeof out);
                if (!used && !sent) break;
            }
            raw_cases++;
        } else {
            // Mutate an authenticated handshake record after its valid prefix.
            // The bypass is compiled only into this test executable.
            while (!r->record[target].bypass) target=(target+1)%r->records;
            for (size_t i=0;i<r->records && os64_tls_state(client).status==OS64_TLS_OK;i++) {
                replay_record *rec=&r->record[i];
                if (!rec->bypass) feed(client,r->raw+rec->at,rec->wire_len);
                else {
                    size_t length=rec->plain_len; memcpy(bytes,rec->plain,length);
                    if (i==target) {
                        unsigned mode=fuzz_random(&random)%4;
                        if (mode==0) length=fuzz_random(&random)%(length+1);
                        else if (mode==1) bytes[length++]=0;
                        else for (unsigned j=0;j<1+(seed%4);j++) bytes[fuzz_random(&random)%length]^=(unsigned char)(1u<<(fuzz_random(&random)%8));
                    }
                    tls13_test_handshake(inner(client),bytes,length);
                }
                drain(client,out,sizeof out);
            }
            deep_cases++;
        }
        tls13_engine *e=inner(client); coverage|=e->test_coverage;
        os64_tls_state_t state=os64_tls_state(client);
        if (!e->authenticated) CHECK(!(state.flags&(OS64_TLS_HANDSHAKE_DONE|OS64_TLS_RECV_PLAIN|OS64_TLS_SEND_PLAIN)));
        if (state.status!=OS64_TLS_OK) terminals++;
        else CHECK(state.flags || e->header_used || e->recv_used);
        os64_tls_input_eof(client); drain(client,out,sizeof out);
        os64_tls_abort(client,OS64_TLS_CANCELLED); os64_tls_free(client); CHECK(live==baseline);
        iterations++;
        if (!(iterations%1000)) { printf("FUZZ iterations=%zu raw=%zu deep=%zu terminals=%zu coverage=0x%llx elapsed=%ld seed=%u\n",iterations,raw_cases,deep_cases,terminals,(unsigned long long)coverage,(long)(time(NULL)-start),seed); fflush(stdout); }
    } while (!replay_case && time(NULL)-start<(time_t)seconds);
    free(bytes); for (size_t i=0;i<count;i++) free(corpus[i]);
    printf("FUZZ PASS seconds=%ld iterations=%zu raw=%zu deep=%zu terminals=%zu coverage=0x%llx initial_seed=147731\n",(long)(time(NULL)-start),iterations,raw_cases,deep_cases,terminals,(unsigned long long)coverage);
}

int main(int argc, char **argv)
{
    CHECK(argc==2 || argc==3 || argc==5); directory=argv[1]; root_file("root.der");
    if (argc>=3) { fuzz((unsigned)strtoul(argv[2],NULL,10),argc==5,argc==5?(unsigned)strtoul(argv[3],NULL,10):0,argc==5?strtoul(argv[4],NULL,10):0); os64_tls_trust_free(trust); CHECK(!live); return 0; }
    const char *suites[]={"TLS_AES_128_GCM_SHA256","TLS_AES_256_GCM_SHA384","TLS_CHACHA20_POLY1305_SHA256"};
    const char *groups[]={"X25519","P-256","P-384"};
    for (unsigned s=0;s<3;s++) for (unsigned k=0;k<2;k++) for (unsigned g=0;g<3;g++) {
        connection *c=connect_peer(k?"rsa":"ec",suites[s],groups[g],OS64_TLS_PROTOCOL_DEFAULT,TLS1_3_VERSION,g==0?1:g==1?37:4096);
        handshake_done(c); exchange(c,70013);
        CHECK(SSL_key_update(c->ssl,SSL_KEY_UPDATE_REQUESTED));
        ssl_result(c,SSL_do_handshake(c->ssl)); exchange(c,8193);
        CHECK(SSL_key_update(c->ssl,SSL_KEY_UPDATE_NOT_REQUESTED));
        ssl_result(c,SSL_do_handshake(c->ssl)); exchange(c,257);
        close_connection(c,k!=0); destroy(c);
        printf("PASS %s / %s / %s: handshake, data, KeyUpdate, close\n",suites[s],k?"RSA":"ECDSA",groups[g]); fflush(stdout);
    }
    pending_output(); pending_close(); fallback(); fallback_downgrade(); failures(); client_hello_offer(); adversarial(); record_cases(); framing_cases(); signature_schemes();
    os64_tls_trust_free(trust); CHECK(!live);
    printf("TLS 1.3 public engine PASS; largest owned allocation %zu bytes\n",peak);
    return 0;
}
