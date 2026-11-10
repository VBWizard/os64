#include "tls13_engine.h"
#include "os64/str.h"

_Static_assert(sizeof(tls13_engine) < 65536, "TLS 1.3 engine memory budget");
_Static_assert(sizeof(size_t) >= 8, "a streamed field is read as up to eight bytes");
enum { WAIT_HELLO, WAIT_EXTENSIONS, WAIT_CERTIFICATE, WAIT_CERTIFICATE_AFTER_REQUEST,
       WAIT_VERIFY, WAIT_FINISHED, CONNECTED };
enum { H_CLIENT_HELLO = 1, H_SERVER_HELLO = 2, H_TICKET = 4, H_EXTENSIONS = 8,
       H_CERTIFICATE = 11, H_CERT_REQUEST = 13, H_VERIFY = 15, H_FINISHED = 20, H_UPDATE = 24 };
static const unsigned char retry_random[32] = {
    0xcf,0x21,0xad,0x74,0xe5,0x9a,0x61,0x11,0xbe,0x1d,0x8c,0x02,0x1e,0x65,0xb8,0x91,
    0xc2,0xa2,0x11,0x16,0x7a,0xbb,0x8c,0x5e,0x07,0x9e,0x09,0xe2,0xc8,0xa8,0x33,0x9c
};
static const uint16_t old_suites[] = {0xcca9,0xcca8,0xc02b,0xc02f,0xc02c,0xc030};
static const uint16_t signatures[] = {0x0403,0x0503,0x0603,0x0804,0x0805,0x0806,0x0401,0x0501,0x0601};
static const uint16_t cert_signatures[] = {0x0403,0x0503,0x0603,0x0401,0x0501,0x0601};
static unsigned get16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }
static void put16(unsigned char *p, size_t n) { p[0] = (unsigned char)(n >> 8); p[1] = (unsigned char)n; }
static void put24(unsigned char *p, size_t n) { p[0] = (unsigned char)(n >> 16); put16(p+1,n); }
static void pump(tls13_engine *e);
static void handshake(tls13_engine *e, const unsigned char *p, size_t n);

static void forget_secrets(tls13_engine *e)
{
    tls13_wipe(e->shares,sizeof e->shares);
    tls13_wipe(&e->rng,sizeof e->rng);
    tls13_wipe(e->client_hs,sizeof e->client_hs); tls13_wipe(e->server_hs,sizeof e->server_hs);
    tls13_wipe(e->master,sizeof e->master);
}
static void forget_ticket(tls13_engine *e)
{
    if (e->res.arriving) tls13_wipe(e->res.arriving,sizeof *e->res.arriving);
    tls13_wipe(e->ticket_nonce,sizeof e->ticket_nonce);
    e->nonce_len=e->ticket_len=0; e->ticket_keep=false;
}
static unsigned alert_for(int error)
{
    switch (error) {
    case BR_ERR_BAD_MAC: return BR_ALERT_BAD_RECORD_MAC;
    case BR_ERR_TOO_LARGE: return BR_ALERT_RECORD_OVERFLOW;
    case BR_ERR_BAD_SIGNATURE: case BR_ERR_BAD_FINISHED: return BR_ALERT_DECRYPT_ERROR;
    case BR_ERR_EXTRA_EXTENSION: return BR_ALERT_UNSUPPORTED_EXTENSION;
    case BR_ERR_BAD_VERSION: return BR_ALERT_PROTOCOL_VERSION;
    case TLS13_ERR_ALPN: return BR_ALERT_NO_APPLICATION_PROTOCOL;
    case BR_ERR_BAD_LENGTH: case BR_ERR_BAD_HANDSHAKE: return BR_ALERT_DECODE_ERROR;
    case TLS13_ERR_DOWNGRADE: case TLS13_ERR_SESSION_ID: case TLS13_ERR_KEY_SHARE:
    case TLS13_ERR_RETRY: case TLS13_ERR_SIGNATURE_SCHEME: case TLS13_ERR_PKCS1:
    case TLS13_ERR_PSK:
    case BR_ERR_BAD_CIPHER_SUITE: return BR_ALERT_ILLEGAL_PARAMETER;
    default:
        if (error >= BR_ERR_X509_INVALID_VALUE && error <= BR_ERR_X509_NOT_TRUSTED)
            return BR_ALERT_BAD_CERTIFICATE;
        return BR_ALERT_UNEXPECTED_MESSAGE;
    }
}
static void fail(tls13_engine *e, int error)
{
    if (e->error) return;
    e->error = (unsigned)error;
    tls13_wipe(e->send,sizeof e->send);
    e->send_at = e->send_used = 0;
    // Received alerts and version handoff do not provoke another alert.
    if (error != TLS13_ERR_PEER_TLS12 &&
        !(error >= BR_ERR_RECV_FATAL_ALERT && error < BR_ERR_RECV_FATAL_ALERT+256)) {
        e->send[5] = 2; e->send[6] = (unsigned char)alert_for(error);
        if (e->write_encrypted) {
            if (tls13_record_seal(&e->write_key,TLS13_ALERT,e->send,2,sizeof e->send,&e->send_used))
                e->send_used = 0;
        } else {
            e->send[0] = TLS13_ALERT; e->send[1] = 3; e->send[2] = 3;
            put16(e->send+3,2); e->send_used = 7;
        }
    }
    forget_secrets(e);
    tls13_wipe(e->res_master,sizeof e->res_master); forget_ticket(e);
    tls13_wipe(e->client_ap,sizeof e->client_ap); tls13_wipe(e->server_ap,sizeof e->server_ap);
    tls13_wipe(&e->read_key,sizeof e->read_key); tls13_wipe(&e->write_key,sizeof e->write_key);
    tls13_wipe(e->recv,sizeof e->recv); tls13_wipe(e->app,sizeof e->app);
    tls13_wipe(e->message,sizeof e->message);
    e->app_used = e->plain_at = e->plain_end = 0;
}
static int install(tls13_engine *e, tls13_record *record, const unsigned char *secret)
{
    unsigned char key[TLS13_KEY_MAX], iv[TLS13_IV_SIZE];
    int error = tls13_traffic_keys(e->suite,secret,key,iv);
    if (!error) error = tls13_record_init(record,e->suite,key,iv);
    tls13_wipe(key,sizeof key); tls13_wipe(iv,sizeof iv);
    return error;
}
static int new_share(tls13_engine *e, tls13_share *share, uint16_t id)
{
    tls13_wipe(share,sizeof *share);
    share->group = tls13_group_find(id);
    if (!share->group) return TLS13_ERR_KEY_SHARE;
    br_ec_private_key key;
    if (br_ec_keygen(&e->rng.vtable,&br_ec_all_m31,&key,share->scalar,share->group->curve) != share->group->secret_len)
        return BR_ERR_NO_RANDOM;
    if (br_ec_compute_pub(&br_ec_all_m31,NULL,share->point,&key) != share->group->point_len)
        return TLS13_ERR_KEY_SHARE;
    return 0;
}
// The hello builder writes into a bounded record buffer; nested vector lengths
// are backfilled only after all their children fit.
typedef struct { unsigned char *p; size_t n, cap; bool bad; } writer;
static void bytes(writer *w, const void *p, size_t n)
{
    if (n > w->cap-w->n) { w->bad = true; return; }
    if (n) os64_memcpy(w->p+w->n,p,n);
    w->n += n;
}
static void u8(writer *w, unsigned n) { unsigned char b = (unsigned char)n; bytes(w,&b,1); }
static void u16(writer *w, size_t n) { unsigned char b[2]; put16(b,n); bytes(w,b,2); }
static size_t start_vector(writer *w) { size_t at = w->n; u16(w,0); return at; }
static void end_vector(writer *w, size_t at)
{
    if (!w->bad) put16(w->p+at,w->n-at-2);
}
static size_t extension(writer *w, unsigned type) { u16(w,type); return start_vector(w); }
static void list16(writer *w, const uint16_t *values, size_t count)
{ u16(w,2*count); for (size_t i=0;i<count;i++) u16(w,values[i]); }
static void ccs(unsigned char *out)
{ const unsigned char record[] = {20,3,3,0,1,1}; os64_memcpy(out,record,sizeof record); }
// The suite whose hash the offered session's PSK is of, when this hello may
// carry the session: there is one, it is whole, and once a HelloRetryRequest
// has fixed the suite its hash is that suite's (RFC 8446 4.1.4, 4.2.11).
static const tls13_suite *offer_suite(const tls13_engine *e)
{
    const tls13_session *s=e->res.offer;
    if (!s || !s->ticket_len || s->ticket_len>TLS13_TICKET_MAX) return NULL;
    const tls13_suite *suite=tls13_suite_find(s->suite);
    if (!suite || (e->suite && e->suite->hash_len!=suite->hash_len)) return NULL;
    return suite;
}
// The binder: the client's proof that it holds the PSK, an HMAC under a key
// derived from it over the transcript so far and the hello TRUNCATED at the
// end of the identities, so over everything but the binders themselves
// (RFC 8446 4.2.11.2). The transcript is copied, never advanced: the whole
// hello enters it once the binder is in place.
static int binder(const tls13_engine *e, const tls13_suite *suite,
                  const unsigned char *hello_bytes, size_t truncated, unsigned char *out)
{
    unsigned char zeros[48]={0}, early[48], key[48];
    br_multihash_context partial=e->transcript, empty;
    tls13_transcript_init(&empty);
    br_multihash_update(&partial,hello_bytes,truncated);
    int error=tls13_extract(suite,zeros,e->res.offer->psk,suite->hash_len,early);
    if (!error) error=tls13_derive_secret(suite,early,"res binder",&empty,key);
    if (!error) error=tls13_finished(suite,key,&partial,out);
    tls13_wipe(early,sizeof early); tls13_wipe(key,sizeof key); tls13_wipe(&partial,sizeof partial);
    return error;
}
static int hello(tls13_engine *e, const unsigned char *cookie, size_t cookie_len)
{
    // In compatibility mode the second flight starts with CCS, whether that
    // flight is CH2 after HRR or the encrypted client Finished (RFC 8446 D.4).
    size_t prefix = e->retry && !e->ccs_sent ? 6 : 0;
    if (prefix) { ccs(e->send); e->ccs_sent = true; }
    unsigned char *record = e->send+prefix;
    writer w = {record+5,0,sizeof e->send-prefix-5,false};
    const unsigned char header[4] = {H_CLIENT_HELLO,0,0,0}; bytes(&w,header,4);
    u16(&w,0x0303); bytes(&w,e->random,32); u8(&w,32); bytes(&w,e->session,32);
    u16(&w,18); u16(&w,0x1301); u16(&w,0x1302); u16(&w,0x1303);
    for (size_t i=0;i<sizeof old_suites/sizeof *old_suites;i++) u16(&w,old_suites[i]);
    u8(&w,1); u8(&w,0);
    size_t all = start_vector(&w), x = extension(&w,0), names = start_vector(&w);
    u8(&w,0); u16(&w,os64_strlen(e->hostname)); bytes(&w,e->hostname,os64_strlen(e->hostname));
    end_vector(&w,names); end_vector(&w,x);
    x = extension(&w,10); u16(&w,2*tls13_group_count);
    for (size_t i=0;i<tls13_group_count;i++) u16(&w,tls13_groups[i].id);
    end_vector(&w,x);
    x = extension(&w,13); list16(&w,signatures,sizeof signatures/sizeof *signatures); end_vector(&w,x);
    x = extension(&w,50); list16(&w,cert_signatures,sizeof cert_signatures/sizeof *cert_signatures); end_vector(&w,x);
    if (e->alpn_count) {
        x = extension(&w,16); names = start_vector(&w);
        for (size_t i=0;i<e->alpn_count;i++) { size_t n=os64_strlen(e->alpn[i]); u8(&w,n); bytes(&w,e->alpn[i],n); }
        end_vector(&w,names); end_vector(&w,x);
    }
    x = extension(&w,43); u8(&w,4); u16(&w,0x0304); u16(&w,0x0303); end_vector(&w,x);
    x = extension(&w,51); names = start_vector(&w);
    for (unsigned i=0;i<e->share_count;i++) {
        const tls13_share *share=&e->shares[i];
        u16(&w,share->group->id); u16(&w,share->group->point_len); bytes(&w,share->point,share->group->point_len);
    }
    end_vector(&w,names); end_vector(&w,x);
    if (cookie) { x=extension(&w,44); u16(&w,cookie_len); bytes(&w,cookie,cookie_len); end_vector(&w,x); }
    // psk_dhe_ke alone: a resumed handshake still does the key exchange.
    if (e->res.advertise) { x=extension(&w,45); u8(&w,1); u8(&w,1); end_vector(&w,x); }
    // pre_shared_key is the LAST extension, as the RFC requires: the binder
    // covers the hello up to it. One identity, and its binder reserved.
    const tls13_suite *psk=offer_suite(e);
    size_t truncated=0, binder_at=0;
    if (psk) {
        const tls13_session *s=e->res.offer;
        uint32_t age=s->age_ms+s->age_add;   // modulo 2^32, as obfuscated_ticket_age is
        x=extension(&w,41); names=start_vector(&w);
        u16(&w,s->ticket_len); bytes(&w,s->ticket,s->ticket_len);
        u16(&w,age>>16); u16(&w,age&65535);
        end_vector(&w,names);
        truncated=w.n;
        const unsigned char reserved[TLS13_HASH_MAX]={0};
        u16(&w,1+psk->hash_len); u8(&w,psk->hash_len); binder_at=w.n; bytes(&w,reserved,psk->hash_len);
        end_vector(&w,x);
    }
    end_vector(&w,all);
    if (w.bad || w.n>TLS13_CONTENT_MAX) return BR_ERR_TOO_LARGE;
    put24(w.p+1,w.n-4);
    // Every length before the binders already counts them, so the truncated
    // hello hashed here is the one the server will hash.
    if (psk) { int error=binder(e,psk,w.p,truncated,w.p+binder_at); if (error) return error; }
    e->offered=psk!=NULL;
    br_multihash_update(&e->transcript,w.p,w.n);
    record[0]=TLS13_HANDSHAKE; record[1]=3; record[2]=3; put16(record+3,w.n);
    e->send_at=0; e->send_used=prefix+5+w.n;
    return 0;
}
// A cursor bounds every vector before its bytes are interpreted.
typedef struct { const unsigned char *p; size_t n, at; bool bad; } cursor;
static const unsigned char *take(cursor *c, size_t n)
{
    if (c->bad || n>c->n-c->at) { c->bad=true; return NULL; }
    const unsigned char *p=c->p+c->at; c->at+=n; return p;
}
static unsigned number(cursor *c, unsigned width)
{
    const unsigned char *p=take(c,width); unsigned n=0;
    if (p) for (unsigned i=0;i<width;i++) n=(n<<8)|p[i];
    return n;
}
static cursor vector(cursor *c, unsigned width)
{
    size_t n=number(c,width); const unsigned char *p=take(c,n);
    return (cursor){p,p?n:0,0,c->bad};
}
static bool ended(const cursor *c) { return !c->bad && c->at==c->n; }
// Message-sized extension vectors are small enough to check duplicates by
// walking the already validated prefix, without a peer-sized allocation.
static bool duplicate(const cursor *exts, size_t before, unsigned type)
{
    size_t at=0;
    while (at<before) { if (get16(exts->p+at)==type) return true; at+=4+get16(exts->p+at+2); }
    return false;
}
static int server_hello(tls13_engine *e)
{
    cursor c={e->message,e->message_length,0,false};
    unsigned version=number(&c,2);
    const unsigned char *random=take(&c,32);
    cursor session=vector(&c,1);
    unsigned suite_id=number(&c,2), compression=number(&c,1);
    // A TLS 1.2 ServerHello may omit the extension vector entirely.
    cursor exts = c.at==c.n ? (cursor){c.p+c.at,0,0,c.bad} : vector(&c,2);
    if (!ended(&c) || !random) return BR_ERR_BAD_HANDSHAKE;
    if (version!=0x0303) return BR_ERR_BAD_VERSION;
    if (compression) return BR_ERR_BAD_COMPRESSION;
    bool retry=tls13_equal(random,retry_random,32);
    const unsigned char *share=NULL, *cookie=NULL; size_t share_len=0,cookie_len=0;
    unsigned selected_version=0,group=0,identity=0; bool have_version=false,have_share=false,have_cookie=false;
    bool other_extension=false,have_psk=false;
    while (exts.at<exts.n && !exts.bad) {
        size_t at=exts.at; unsigned type=number(&exts,2); cursor v=vector(&exts,2);
        if (exts.bad || duplicate(&exts,at,type)) return BR_ERR_BAD_HANDSHAKE;
        if (type==43) {
            have_version=true; selected_version=number(&v,2);
            if (!ended(&v)) return BR_ERR_BAD_HANDSHAKE;
        } else if (type==51) {
            have_share=true; group=number(&v,2);
            if (!retry) { cursor p=vector(&v,2); share=p.p; share_len=p.n; }
            if (!ended(&v)) return BR_ERR_BAD_HANDSHAKE;
        } else if (type==44 && retry) {
            have_cookie=true; cursor p=vector(&v,2); cookie=p.p; cookie_len=p.n;
            if (!ended(&v) || !cookie_len) return BR_ERR_BAD_HANDSHAKE;
        } else if (type==41 && !retry && e->offered) {
            // The server accepts the session this hello offered. In a
            // HelloRetryRequest, or unasked, it is an extension we did not
            // offer there, and falls to the refusal below.
            have_psk=true; identity=number(&v,2);
            if (!ended(&v)) return BR_ERR_BAD_HANDSHAKE;
        } else other_extension=true;
    }
    if (!ended(&exts)) return BR_ERR_BAD_HANDSHAKE;
    if (!have_version) {
        if (retry || e->retry) return BR_ERR_BAD_VERSION;
        if (tls13_has_downgrade_marker(random))
            return TLS13_ERR_DOWNGRADE;
        bool offered=false;
        for (size_t i=0;i<sizeof old_suites/sizeof *old_suites;i++) if (suite_id==old_suites[i]) offered=true;
        if (!offered) return BR_ERR_BAD_CIPHER_SUITE;
        if (session.n>32) return BR_ERR_BAD_HANDSHAKE;
        e->version=0x0303; return TLS13_ERR_PEER_TLS12;
    }
    if (selected_version!=0x0304) return BR_ERR_BAD_VERSION;
    if (session.n!=32 || !tls13_equal(session.p,e->session,32)) return TLS13_ERR_SESSION_ID;
    const tls13_suite *suite=tls13_suite_find((uint16_t)suite_id);
    if (!suite || (e->suite && e->suite!=suite)) return BR_ERR_BAD_CIPHER_SUITE;
    if (other_extension) return BR_ERR_EXTRA_EXTENSION;
    e->suite=suite;
    if (retry) {
        if (e->retry) return TLS13_ERR_SECOND_HRR;
        if (!have_share && !have_cookie) return TLS13_ERR_RETRY;
        if (have_share) {
            if (!tls13_group_find((uint16_t)group)) return TLS13_ERR_KEY_SHARE;
            for (unsigned i=0;i<e->share_count;i++) if (e->shares[i].group->id==group) return TLS13_ERR_RETRY;
        }
        int error=tls13_transcript_retry(&e->transcript,suite);
        if (error) return error;
        br_multihash_update(&e->transcript,e->header,4);
        br_multihash_update(&e->transcript,e->message,e->message_length);
        e->retry=true;
        if (have_share) {
            tls13_wipe(e->shares,sizeof e->shares); e->share_count=1; e->retry_group=group;
            error=new_share(e,&e->shares[0],(uint16_t)group);
            if (error) return error;
        }
        return hello(e,cookie,cookie_len);
    }
    if (!have_share || have_cookie) return TLS13_ERR_KEY_SHARE;
    tls13_share *own=NULL;
    for (unsigned i=0;i<e->share_count;i++) if (e->shares[i].group->id==group) own=&e->shares[i];
    if (!own || (e->retry_group && e->retry_group!=group) || share_len!=own->group->point_len)
        return TLS13_ERR_KEY_SHARE;
    // One identity was offered, so the only one to select is the first, and
    // the suite chosen with it must hash as the session's did (RFC 8446
    // 4.2.11). With it the early secret is the PSK's; without, as before.
    const unsigned char *psk=NULL;
    if (have_psk) {
        const tls13_suite *issued=tls13_suite_find(e->res.offer->suite);
        if (identity || !issued || issued->hash_len!=suite->hash_len) return TLS13_ERR_PSK;
        psk=e->res.offer->psk;
    }
    unsigned char shared[48], secret[48], derived[48], zeros[48]={0};
    br_multihash_context empty; tls13_transcript_init(&empty);
    int error=tls13_shared_secret(own->group,own->scalar,own->group->secret_len,share,share_len,shared);
    if (!error) error=tls13_extract(suite,zeros,psk?psk:zeros,suite->hash_len,secret);
    if (!error) error=tls13_derive_secret(suite,secret,"derived",&empty,derived);
    if (!error) error=tls13_extract(suite,derived,shared,own->group->secret_len,secret);
    br_multihash_update(&e->transcript,e->header,4);
    br_multihash_update(&e->transcript,e->message,e->message_length);
    if (!error) error=tls13_derive_secret(suite,secret,"c hs traffic",&e->transcript,e->client_hs);
    if (!error) error=tls13_derive_secret(suite,secret,"s hs traffic",&e->transcript,e->server_hs);
    if (!error) error=tls13_derive_secret(suite,secret,"derived",&empty,derived);
    if (!error) error=tls13_extract(suite,derived,zeros,suite->hash_len,e->master);
    if (!error) error=install(e,&e->read_key,e->server_hs);
    if (!error) error=install(e,&e->write_key,e->client_hs);
    tls13_wipe(shared,sizeof shared); tls13_wipe(secret,sizeof secret); tls13_wipe(derived,sizeof derived);
    tls13_wipe(e->shares,sizeof e->shares); tls13_wipe(&e->rng,sizeof e->rng);
    if (!error) {
        e->phase=WAIT_EXTENSIONS; e->read_encrypted=e->write_encrypted=true; e->version=0x0304;
        e->resumed=psk!=NULL;
    }
    return error;
}
static int encrypted_extensions(tls13_engine *e)
{
    cursor c={e->message,e->message_length,0,false}, exts=vector(&c,2);
    if (!ended(&c)) return BR_ERR_BAD_HANDSHAKE;
    while (exts.at<exts.n && !exts.bad) {
        size_t at=exts.at; unsigned type=number(&exts,2); cursor v=vector(&exts,2);
        if (exts.bad || duplicate(&exts,at,type)) return BR_ERR_BAD_HANDSHAKE;
        if (type==0) { if (v.n) return BR_ERR_BAD_HANDSHAKE; }
        else if (type==10) {
            cursor groups=vector(&v,2);
            if (!ended(&v) || !groups.n || (groups.n&1)) return BR_ERR_BAD_HANDSHAKE;
        } else if (type==16) {
            if (!e->alpn_count) return BR_ERR_EXTRA_EXTENSION;
            cursor names=vector(&v,2), name=vector(&names,1);
            if (!ended(&v) || !ended(&names) || !name.n) return BR_ERR_BAD_HANDSHAKE;
            bool found=false;
            for (size_t i=0;i<e->alpn_count;i++)
                if (os64_strlen(e->alpn[i])==name.n && tls13_equal(e->alpn[i],name.p,name.n)) found=true;
            if (!found) return TLS13_ERR_ALPN;
            os64_memcpy(e->selected,name.p,name.n); e->selected[name.n]=0;
        } else return BR_ERR_EXTRA_EXTENSION;
    }
    return ended(&exts) ? 0 : BR_ERR_BAD_HANDSHAKE;
}
static int certificate_request(tls13_engine *e)
{
    cursor c={e->message,e->message_length,0,false}, context=vector(&c,1), exts=vector(&c,2);
    if (!ended(&c) || context.n) return BR_ERR_BAD_HANDSHAKE;
    bool signatures_found=false;
    while (exts.at<exts.n && !exts.bad) {
        size_t at=exts.at; unsigned type=number(&exts,2); cursor v=vector(&exts,2);
        if (exts.bad || duplicate(&exts,at,type)) return BR_ERR_BAD_HANDSHAKE;
        if (type==13 || type==50) {
            cursor list=vector(&v,2);
            if (!ended(&v) || !list.n || (list.n&1)) return BR_ERR_BAD_HANDSHAKE;
            if (type==13) signatures_found=true;
        }
        // Unknown CertificateRequest extensions are ignored (RFC 8446 4.3.2).
    }
    return ended(&exts) && signatures_found ? 0 : BR_ERR_BAD_HANDSHAKE;
}
static int certificate_verify(tls13_engine *e)
{
    cursor c={e->message,e->message_length,0,false}; unsigned scheme=number(&c,2); cursor sig=vector(&c,2);
    if (!ended(&c) || !sig.n) return BR_ERR_BAD_HANDSHAKE;
    if (scheme==0x0401 || scheme==0x0501 || scheme==0x0601) return TLS13_ERR_PKCS1;
    const br_hash_class *hf=NULL; int curve=0;
    switch (scheme) {
    case 0x0403: hf=&br_sha256_vtable; curve=BR_EC_secp256r1; break;
    case 0x0503: hf=&br_sha384_vtable; curve=BR_EC_secp384r1; break;
    case 0x0603: hf=&br_sha512_vtable; curve=BR_EC_secp521r1; break;
    case 0x0804: hf=&br_sha256_vtable; break;
    case 0x0805: hf=&br_sha384_vtable; break;
    case 0x0806: hf=&br_sha512_vtable; break;
    default: return TLS13_ERR_SIGNATURE_SCHEME;
    }
    unsigned usages=0;
    const br_x509_pkey *key=(*e->validator)->get_pkey(e->validator,&usages);
    if (!key || !(usages&BR_KEYTYPE_SIGN)) return BR_ERR_X509_FORBIDDEN_KEY_USAGE;
    if ((curve && (key->key_type!=BR_KEYTYPE_EC || key->key.ec.curve!=curve)) ||
        (!curve && key->key_type!=BR_KEYTYPE_RSA)) return TLS13_ERR_SIGNATURE_SCHEME;
    unsigned char content[64+34+48], hash[64];
    const char context[]="TLS 1.3, server CertificateVerify";
    os64_memset(content,32,64); os64_memcpy(content+64,context,sizeof context);
    size_t at=64+sizeof context;
    if (br_multihash_out(&e->transcript,e->suite->hash_len==32?br_sha256_ID:br_sha384_ID,content+at)!=e->suite->hash_len)
        return BR_ERR_BAD_STATE;
    br_hash_compat_context hc;
    hf->init(&hc.vtable); hf->update(&hc.vtable,content,at+e->suite->hash_len); hf->out(&hc.vtable,hash);
    size_t hash_len=(hf->desc>>BR_HASHDESC_OUT_OFF)&BR_HASHDESC_OUT_MASK;
    uint32_t valid=curve ? br_ecdsa_i31_vrfy_asn1(&br_ec_all_m31,hash,hash_len,&key->key.ec,sig.p,sig.n) :
        br_rsa_i31_pss_vrfy(sig.p,sig.n,hf,hf,hash,hash_len,&key->key.rsa);
    tls13_wipe(&hc,sizeof hc); tls13_wipe(hash,sizeof hash); tls13_wipe(content,sizeof content);
    return valid ? 0 : BR_ERR_BAD_SIGNATURE;
}
static int client_finished(tls13_engine *e)
{
    size_t prefix=0;
    if (!e->ccs_sent) { ccs(e->send); prefix=6; e->ccs_sent=true; }
    unsigned char *record=e->send+prefix, *p=record+5;
    size_t n=0;
    if (e->certificate_requested) {
        const unsigned char empty[]={H_CERTIFICATE,0,0,4,0,0,0,0};
        os64_memcpy(p,empty,sizeof empty); n=sizeof empty;
        br_multihash_update(&e->transcript,p,n);
    }
    p[n]=H_FINISHED; put24(p+n+1,e->suite->hash_len);
    int error=tls13_finished(e->suite,e->client_hs,&e->transcript,p+n+4);
    if (!error) {
        br_multihash_update(&e->transcript,p+n,4+e->suite->hash_len);
        n+=4+e->suite->hash_len;
        // The secret every ticket's PSK is derived from, over the handshake
        // through this Finished. `master` is forgotten below.
        if (e->res.arriving)
            error=tls13_derive_secret(e->suite,e->master,"res master",&e->transcript,e->res_master);
        size_t wire=0;
        if (!error)
        error=tls13_record_seal(&e->write_key,TLS13_HANDSHAKE,record,n,sizeof e->send-prefix,&wire);
        e->send_at=0; e->send_used=prefix+wire;
    }
    if (!error) error=install(e,&e->write_key,e->client_ap);
    forget_secrets(e);
    if (!error) { e->phase=CONNECTED; e->authenticated=true; }
    return error;
}
// Incremental integer fields are bounded by the enclosing handshake length.
static bool stream_number(tls13_engine *e, unsigned byte, unsigned width)
{
    e->field_value=(e->field_value<<8)|byte;
    return ++e->field_used==width;
}
static size_t stream_value(tls13_engine *e)
{ size_t n=e->field_value; e->field_value=0; e->field_used=0; return n; }
static int certificate_byte(tls13_engine *e, unsigned byte, size_t remaining)
{
    switch (e->stream_stage) {
    case 0:
        if (byte) return BR_ERR_BAD_HANDSHAKE;
        e->stream_stage=1; break;
    case 1:
        if (stream_number(e,byte,3)) {
            size_t n=stream_value(e);
            if (!n || n!=remaining) return BR_ERR_BAD_HANDSHAKE;
            e->stream_stage=2;
        }
        break;
    case 2:
        if (stream_number(e,byte,3)) {
            size_t n=stream_value(e);
            if (!n || remaining<2 || n>remaining-2) return BR_ERR_BAD_HANDSHAKE;
            e->stream_left=n; e->stream_stage=3;
            (*e->validator)->start_cert(e->validator,(uint32_t)n);
        }
        break;
    case 4:
        if (stream_number(e,byte,2)) {
            if (stream_value(e)) return BR_ERR_EXTRA_EXTENSION;
            e->stream_stage=2;
        }
        break;
    default: return BR_ERR_BAD_STATE;
    }
    return 0;
}
static int ticket_byte(tls13_engine *e, unsigned byte, size_t remaining)
{
    switch (e->stream_stage) {
    case 0:
        // ticket_lifetime, then ticket_age_add: four bytes each.
        if (stream_number(e,byte,8)) {
            uint64_t both=stream_value(e);
            e->ticket_lifetime=(uint32_t)(both>>32); e->ticket_age_add=(uint32_t)both;
            e->stream_stage=1;
        }
        break;
    case 1:
        if (byte>remaining) return BR_ERR_BAD_HANDSHAKE;
        e->nonce_len=byte;
        e->stream_left=byte; e->stream_stage=byte?2:3; break;
    case 3:
        if (stream_number(e,byte,2)) {
            size_t n=stream_value(e);
            if (!n || n>remaining) return BR_ERR_BAD_HANDSHAKE;
            // A ticket past what a session holds is read to its end like any
            // other and not kept.
            e->ticket_len=n; e->ticket_keep=e->res.arriving && n<=TLS13_TICKET_MAX;
            e->stream_left=n; e->stream_stage=4;
        }
        break;
    case 5:
        if (stream_number(e,byte,2)) {
            if (stream_value(e)!=remaining) return BR_ERR_BAD_HANDSHAKE;
            e->stream_stage=6;
        }
        break;
    case 6:
        if (stream_number(e,byte,4)) {
            size_t field=stream_value(e), type=field>>16, n=field&65535;
            if (n>remaining || (type==42 && n!=4)) return BR_ERR_BAD_HANDSHAKE;
            unsigned char mask=(unsigned char)(1u<<(type&7));
            if (e->message[type>>3]&mask) return BR_ERR_BAD_HANDSHAKE;
            e->message[type>>3]|=mask;
            e->stream_left=n; e->stream_stage=n?7:6;
        }
        break;
    default: return BR_ERR_BAD_STATE;
    }
    return 0;
}
static int streamed(tls13_engine *e, const unsigned char *p, size_t n)
{
    size_t at=0;
    while (at<n) {
        bool cert=e->message_type==H_CERTIFICATE;
        bool data=cert ? e->stream_stage==3 :
            (e->stream_stage==2 || e->stream_stage==4 || e->stream_stage==7);
        if (data) {
            size_t count=n-at<e->stream_left?n-at:e->stream_left;
            if (cert) (*e->validator)->append(e->validator,p+at,count);
            else if (e->stream_stage==2)
                os64_memcpy(e->ticket_nonce+(e->nonce_len-e->stream_left),p+at,count);
            else if (e->stream_stage==4 && e->ticket_keep)
                os64_memcpy(e->res.arriving->ticket+(e->ticket_len-e->stream_left),p+at,count);
            at+=count; e->message_used+=count; e->stream_left-=count;
            if (!e->stream_left) {
                if (cert) { (*e->validator)->end_cert(e->validator); e->stream_stage=4; }
                else e->stream_stage=e->stream_stage==7?6:e->stream_stage+1;
            }
        } else {
            unsigned byte=p[at++]; e->message_used++;
            size_t remaining=e->message_length-e->message_used;
            int error=cert?certificate_byte(e,byte,remaining):ticket_byte(e,byte,remaining);
            if (error) return error;
        }
    }
    return 0;
}
static bool expected(const tls13_engine *e, unsigned type)
{
    switch (e->phase) {
    case WAIT_HELLO: return type==H_SERVER_HELLO;
    case WAIT_EXTENSIONS: return type==H_EXTENSIONS;
    case WAIT_CERTIFICATE: return type==H_CERTIFICATE || type==H_CERT_REQUEST;
    case WAIT_CERTIFICATE_AFTER_REQUEST: return type==H_CERTIFICATE;
    case WAIT_VERIFY: return type==H_VERIFY;
    case WAIT_FINISHED: return type==H_FINISHED;
    case CONNECTED: return type==H_TICKET || type==H_UPDATE;
    default: return false;
    }
}
// A NewSessionTicket has been read whole. Its PSK is derived from the
// resumption secret and the ticket's own nonce (RFC 8446 4.6.1), so each
// ticket of a connection is a different secret. A lifetime of zero is the
// server saying "discard at once"; one past the RFC's ceiling is held to it.
static int ticket_done(tls13_engine *e)
{
    tls13_session *s=e->res.arriving;
    int error=0;
    if (s && e->ticket_keep && e->ticket_lifetime) {
        uint32_t lifetime=e->ticket_lifetime<TLS13_TICKET_LIFETIME_MAX?e->ticket_lifetime:TLS13_TICKET_LIFETIME_MAX;
        s->suite=e->suite->id; s->age_add=e->ticket_age_add; s->age_ms=0; s->ticket_len=e->ticket_len;
        error=tls13_expand_label(e->suite,e->res_master,"resumption",e->ticket_nonce,e->nonce_len,
                                 s->psk,e->suite->hash_len);
        if (!error) {
            e->tickets_kept++;
            if (e->res.received) e->res.received(e->res.context,s,lifetime);
        }
    } else if (s) e->tickets_dropped++;
    forget_ticket(e);
    return error;
}
static int complete_message(tls13_engine *e)
{
#ifdef OS64_TLS13_TEST
    e->test_coverage |= UINT64_C(1)<<e->message_type;
#endif
    if (e->message_type==H_SERVER_HELLO) return server_hello(e);
    if (e->message_type==H_CERTIFICATE) {
        if (e->stream_stage!=2 || e->field_used) return BR_ERR_BAD_HANDSHAKE;
        int error=(int)(*e->validator)->end_chain(e->validator);
        if (!error) e->phase=WAIT_VERIFY;
        return error;
    }
    if (e->message_type==H_TICKET)
        return e->stream_stage==6 && !e->field_used ? ticket_done(e) : BR_ERR_BAD_HANDSHAKE;
    if (e->message_type==H_UPDATE) {
        if (e->message_length!=1 || e->message[0]>1) return BR_ERR_BAD_HANDSHAKE;
        int error=tls13_update_secret(e->suite,e->server_ap);
        if (!error) error=install(e,&e->read_key,e->server_ap);
        if (e->message[0]) e->update_pending=true;
        return error;
    }
    int error=0;
    if (e->message_type==H_EXTENSIONS) error=encrypted_extensions(e);
    else if (e->message_type==H_CERT_REQUEST) error=certificate_request(e);
    else if (e->message_type==H_VERIFY) error=certificate_verify(e);
    else if (e->message_type==H_FINISHED) {
        unsigned char expected_mac[48];
        if (e->message_length!=e->suite->hash_len) return BR_ERR_BAD_FINISHED;
        error=tls13_finished(e->suite,e->server_hs,&e->transcript,expected_mac);
        if (!error && !tls13_equal(expected_mac,e->message,e->message_length)) error=BR_ERR_BAD_FINISHED;
        tls13_wipe(expected_mac,sizeof expected_mac);
    }
    if (error) return error;
    // CertificateVerify and Finished authenticate the transcript BEFORE their
    // own framing is added. Streamed Certificate was hashed as it arrived.
    br_multihash_update(&e->transcript,e->header,4);
    br_multihash_update(&e->transcript,e->message,e->message_length);
    // A resumed handshake has no Certificate, CertificateRequest or
    // CertificateVerify: the server's Finished is next, and anything else is
    // unexpected.
    if (e->message_type==H_EXTENSIONS) e->phase=e->resumed?WAIT_FINISHED:WAIT_CERTIFICATE;
    else if (e->message_type==H_CERT_REQUEST) { e->certificate_requested=true; e->phase=WAIT_CERTIFICATE_AFTER_REQUEST; }
    else if (e->message_type==H_VERIFY) e->phase=WAIT_FINISHED;
    else if (e->message_type==H_FINISHED) {
        error=tls13_derive_secret(e->suite,e->master,"c ap traffic",&e->transcript,e->client_ap);
        if (!error) error=tls13_derive_secret(e->suite,e->master,"s ap traffic",&e->transcript,e->server_ap);
        if (!error) error=install(e,&e->read_key,e->server_ap);
        if (!error) error=client_finished(e);
    }
    return error;
}
static void handshake(tls13_engine *e, const unsigned char *p, size_t n)
{
    while (n && !e->error) {
        if (e->header_used<4) {
            size_t count=4-e->header_used; if (count>n) count=n;
            os64_memcpy(e->header+e->header_used,p,count);
            e->header_used+=count; p+=count; n-=count;
            if (e->header_used<4) return;
            e->message_type=e->header[0];
            e->message_length=((size_t)e->header[1]<<16)|get16(e->header+2);
            e->message_used=e->stream_left=e->field_value=0; e->field_used=e->stream_stage=0;
            if (!expected(e,e->message_type)) { fail(e,BR_ERR_UNEXPECTED); return; }
            bool stream=e->message_type==H_CERTIFICATE || e->message_type==H_TICKET;
            if (!stream && e->message_length>TLS13_MESSAGE_MAX-4) { fail(e,BR_ERR_TOO_LARGE); return; }
            if (e->message_type==H_CERTIFICATE) {
                br_multihash_update(&e->transcript,e->header,4);
                (*e->validator)->start_chain(e->validator,e->hostname);
            }
            if (e->message_type==H_TICKET) { os64_memset(e->message,0,sizeof e->message); forget_ticket(e); }
        }
        size_t count=e->message_length-e->message_used; if (count>n) count=n;
        int error=0;
        if (e->message_type==H_CERTIFICATE || e->message_type==H_TICKET) {
            if (e->message_type==H_CERTIFICATE) br_multihash_update(&e->transcript,p,count);
            error=streamed(e,p,count);
        } else {
            os64_memcpy(e->message+e->message_used,p,count); e->message_used+=count;
        }
        p+=count; n-=count;
        if (error) { fail(e,error); return; }
        if (e->message_used!=e->message_length) return;
        // These messages change epochs (or start another hello flight). The
        // boundary check happens before their handler installs any new key.
        if (n && (e->message_type==H_SERVER_HELLO || e->message_type==H_FINISHED || e->message_type==H_UPDATE)) {
            fail(e,BR_ERR_UNEXPECTED); return;
        }
        error=complete_message(e);
        if (error) { fail(e,error); return; }
        e->header_used=e->message_used=e->message_length=0;
        tls13_wipe(e->message,sizeof e->message);
    }
}
static void seal_app(tls13_engine *e)
{
    if (!e->app_used) { e->flush_pending=false; return; }
    os64_memcpy(e->send+5,e->app,e->app_used);
    int error=tls13_record_seal(&e->write_key,TLS13_APPLICATION,e->send,e->app_used,sizeof e->send,&e->send_used);
    tls13_wipe(e->app,e->app_used); e->app_used=0; e->send_at=0; e->flush_pending=false;
    if (error) fail(e,error);
}
static void pump(tls13_engine *e)
{
    if (e->error || e->send_used || !e->authenticated) return;
    if (!e->local_closed && (e->update_pending || tls13_record_needs_update(&e->write_key))) {
        const unsigned char update[]={H_UPDATE,0,0,1,0};
        os64_memcpy(e->send+5,update,sizeof update);
        int error=tls13_record_seal(&e->write_key,TLS13_HANDSHAKE,e->send,sizeof update,sizeof e->send,&e->send_used);
        if (!error) error=tls13_update_secret(e->suite,e->client_ap);
        if (!error) error=install(e,&e->write_key,e->client_ap);
        e->send_at=0; e->update_pending=false;
        if (error) fail(e,error);
        return;
    }
    if (e->app_used && (e->flush_pending || e->app_used==TLS13_CONTENT_MAX || e->closing)) {
        seal_app(e); return;
    }
    if (e->closing && !e->local_closed) {
        e->send[5]=1; e->send[6]=BR_ALERT_CLOSE_NOTIFY;
        int error=tls13_record_seal(&e->write_key,TLS13_ALERT,e->send,2,sizeof e->send,&e->send_used);
        e->send_at=0;
        if (error) fail(e,error); else e->local_closed=true;
    }
}
int tls13_init(tls13_engine *e, const br_x509_class **validator, const char *hostname,
               const char *const *alpn, size_t alpn_count, const unsigned char seed[32],
               const tls13_resumption *resumption)
{
    e->validator=validator; e->hostname=hostname; e->alpn=alpn; e->alpn_count=alpn_count;
    if (resumption) e->res=*resumption;
    e->recv_need=5; tls13_transcript_init(&e->transcript);
    br_hmac_drbg_init(&e->rng,&br_sha256_vtable,seed,32);
    br_hmac_drbg_generate(&e->rng,e->random,32); br_hmac_drbg_generate(&e->rng,e->session,32);
    e->share_count=2;
    int error=new_share(e,&e->shares[0],29);
    if (!error) error=new_share(e,&e->shares[1],23);
    if (!error) error=hello(e,NULL,0);
    if (error) fail(e,error);
    return error;
}
unsigned tls13_current_state(tls13_engine *e)
{
    pump(e);
    unsigned state=e->send_used?BR_SSL_SENDREC:0;
    if (e->error || (e->local_closed && e->peer_closed && !e->send_used)) return state|BR_SSL_CLOSED;
    if (e->plain_at<e->plain_end) state|=BR_SSL_RECVAPP;
    // Authenticated input uses a separate buffer, so blocked output must not
    // hide an early upload response. Before authentication, hello/Finished
    // handlers build the next flight in send; drain the prior flight first.
    else if (!e->peer_closed && (!e->send_used || e->authenticated)) state|=BR_SSL_RECVREC;
    if (e->authenticated && !e->closing && !e->send_used && !e->update_pending && e->app_used<TLS13_CONTENT_MAX)
        state|=BR_SSL_SENDAPP;
    return state;
}
int tls13_last_error(const tls13_engine *e) { return (int)e->error; }
bool tls13_resumed(const tls13_engine *e) { return e->resumed; }
const char *tls13_selected_protocol(const tls13_engine *e) { return e->selected[0]?e->selected:NULL; }
void tls13_flush(tls13_engine *e) { e->flush_pending=true; pump(e); }
void tls13_close(tls13_engine *e) { e->closing=true; pump(e); }
unsigned char *tls13_sendrec_buf(tls13_engine *e, size_t *length)
{ *length=e->send_used-e->send_at; return *length?e->send+e->send_at:NULL; }
void tls13_sendrec_ack(tls13_engine *e, size_t n)
{
    if (!n || n>e->send_used-e->send_at) return;
    tls13_wipe(e->send+e->send_at,n); e->send_at+=n;
    if (e->send_at==e->send_used) { e->send_at=e->send_used=0; pump(e); }
}
unsigned char *tls13_sendapp_buf(tls13_engine *e, size_t *length)
{
    *length=(tls13_current_state(e)&BR_SSL_SENDAPP)?TLS13_CONTENT_MAX-e->app_used:0;
    return *length?e->app+e->app_used:NULL;
}
void tls13_sendapp_ack(tls13_engine *e, size_t n)
{
    if (!n || n>TLS13_CONTENT_MAX-e->app_used || !(tls13_current_state(e)&BR_SSL_SENDAPP)) return;
    e->app_used+=n; pump(e);
}
unsigned char *tls13_recvapp_buf(tls13_engine *e, size_t *length)
{ *length=e->plain_end-e->plain_at; return *length?e->recv+e->plain_at:NULL; }
void tls13_recvapp_ack(tls13_engine *e, size_t n)
{
    if (!n || n>e->plain_end-e->plain_at) return;
    tls13_wipe(e->recv+e->plain_at,n); e->plain_at+=n;
    if (e->plain_at==e->plain_end) { e->plain_at=e->plain_end=0; pump(e); }
}
unsigned char *tls13_recvrec_buf(tls13_engine *e, size_t *length)
{
    *length=(tls13_current_state(e)&BR_SSL_RECVREC)?e->recv_need-e->recv_used:0;
    return *length?e->recv+e->recv_used:NULL;
}
void tls13_recvrec_ack(tls13_engine *e, size_t n)
{
    if (!n || n>e->recv_need-e->recv_used || e->error) return;
    e->recv_used+=n;
    if (e->recv_used<e->recv_need) return;
    bool plaintext=e->recv[0]==TLS13_CCS || !e->read_encrypted;
    if (e->recv_need==5) {
        size_t length;
        int error=tls13_record_length(e->recv,!plaintext,&length);
        if (error) { fail(e,error); return; }
        e->recv_need=5+length;
        if (length) return;
    }
    unsigned type; size_t length;
    int error=plaintext ? tls13_plaintext_open(e->recv,e->recv_need,!e->authenticated,&type,&length) :
        tls13_record_open(&e->read_key,e->recv,e->recv_need,e->authenticated,&type,&length);
    if (error) { fail(e,error); return; }
    if (type!=TLS13_HANDSHAKE && e->header_used) { fail(e,BR_ERR_UNEXPECTED); return; }
    if (type==TLS13_HANDSHAKE) handshake(e,e->recv+5,length);
    else if (type==TLS13_ALERT) {
        unsigned level=e->recv[5], description=e->recv[6];
        if (description==BR_ALERT_CLOSE_NOTIFY) {
            if (!e->authenticated) fail(e,BR_ERR_UNEXPECTED);
            else { e->peer_closed=true; e->closing=true; }
        } else if (!(description==BR_ALERT_USER_CANCELED && level==1))
            fail(e,BR_ERR_RECV_FATAL_ALERT+(int)description);
    } else if (type==TLS13_APPLICATION && !e->closing) {
        e->plain_at=5; e->plain_end=5+length;
        if (!length) e->plain_at=e->plain_end=0;
    }
    if (!e->plain_end) tls13_wipe(e->recv,e->recv_need);
    e->recv_used=0; e->recv_need=5;
    pump(e);
}
#ifdef OS64_TLS13_TEST
void tls13_test_handshake(tls13_engine *e, const void *p, size_t n)
{ if (!e->error) { handshake(e,p,n); pump(e); } }
#endif
