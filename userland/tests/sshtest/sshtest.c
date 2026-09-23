#include "../../apps/sshd/ssh_internal.h"
#ifdef SSH_HOST
#include <stdio.h>
#define report(...) printf(__VA_ARGS__)
#else
#include "os64/os64.h"
#define report(...) os64_printf(__VA_ARGS__)
#endif
static int checks, failed;
#define CHECK(x) do { checks++; if (!(x)) { failed++; report("sshtest: FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static ssh_engine engine;
static uint8_t seed[32] = {7}, private_key[32] = {[31]=1};
static int unhex(const char *s, uint8_t *out)
{
    int n=0;
    while (*s) {
        unsigned x=0;
        for(int i=0;i<2;i++) { char c=*s++; x=x*16+(unsigned)(c<='9'?c-'0':c-'a'+10); }
        out[n++]=(uint8_t)x;
    }
    return n;
}
static void primitives(void)
{
    uint8_t scalar_le[32], scalar_be[32], point[32], expected[32];
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a",scalar_le);
    unhex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a",expected);
    /* BearSSL's scalar API is big endian; RFC 7748's vector is little endian. */
    for(int i=0;i<32;i++) scalar_be[i]=scalar_le[31-i];
    CHECK(br_ec_c25519_i31.mulgen(point,scalar_be,32,BR_EC_curve25519)==32);
    CHECK(!memcmp(point,expected,32));
    uint8_t public_key[65], digest[32]={1}, signature[64];
    CHECK(ssh_host_public(private_key,public_key));
    br_ec_private_key sk={BR_EC_secp256r1,private_key,32};
    br_ec_public_key pk={BR_EC_secp256r1,public_key,65};
    CHECK(br_ecdsa_i31_sign_raw(&br_ec_p256_m31,&br_sha256_vtable,digest,&sk,signature)==64);
    CHECK(br_ecdsa_i31_vrfy_raw(&br_ec_p256_m31,digest,32,&pk,signature,64));
    signature[0]^=1;
    CHECK(!br_ecdsa_i31_vrfy_raw(&br_ec_p256_m31,digest,32,&pk,signature,64));
    uint8_t hmac_key[20], result[32]; memset(hmac_key,11,sizeof(hmac_key));
    br_hmac_key_context kh; br_hmac_context h;
    br_hmac_key_init(&kh,&br_sha256_vtable,hmac_key,sizeof(hmac_key));
    br_hmac_init(&h,&kh,32); br_hmac_update(&h,"Hi There",8); br_hmac_out(&h,result);
    unhex("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",expected);
    CHECK(!memcmp(result,expected,32));
    uint8_t aeskey[16]={0}, iv[16]={0}, data[32]={0}, reference[32];
    memset(iv+12,255,4); memcpy(reference,iv,16); memcpy(reference+16,iv,16);
    memset(reference+28,0,4); reference[27]=1;
    br_aes_ct64_ctrcbc_keys aes; br_aes_ct64_ctrcbc_init(&aes,aeskey,16);
    br_aes_ct64_ctrcbc_ctr(&aes,iv,data,sizeof(data));
    br_aes_ct64_cbcenc_keys cbc; br_aes_ct64_cbcenc_init(&cbc,aeskey,16);
    uint8_t zeroiv[16]={0}; br_aes_ct64_cbcenc_run(&cbc,zeroiv,reference,16);
    memset(zeroiv,0,16); br_aes_ct64_cbcenc_run(&cbc,zeroiv,reference+16,16);
    CHECK(!memcmp(data,reference,32)); CHECK(iv[11]==1 && iv[15]==1);
}
static void codecs(void)
{
    uint8_t b[128], value[33]={0}, point[65], key[104], decoded[104];
    ssh_writer w={b,0,sizeof(b),0}; ssh_put_mpint(&w,value,32);
    CHECK(w.n==4 && b[3]==0);
    value[31]=128; w.n=0; ssh_put_mpint(&w,value,32);
    CHECK(w.n==6 && b[3]==2 && !b[4] && b[5]==128);
    value[31]=127; w.n=0; ssh_put_mpint(&w,value,32);
    CHECK(w.n==5 && b[3]==1 && b[4]==127);
    w.n=0; ssh_put_text(&w,"abcdef");
    for(size_t i=0;i<w.n;i++) { ssh_reader r={b,i,0}; (void)ssh_string(&r); CHECK(r.bad); }
    CHECK(ssh_host_public(private_key,point));
    size_t n=ssh_public_blob(key,sizeof(key),point); CHECK(n==104);
    for(size_t i=0;i<n;i++) CHECK(!ssh_parse_public(key,i,point));
    CHECK(ssh_parse_public(key,n,point));
    char encoded[160]; CHECK(ssh_base64_encode(key,n,encoded,sizeof(encoded))==140);
    CHECK(ssh_base64_decode(encoded,140,decoded,sizeof(decoded))==104);
    CHECK(!memcmp(key,decoded,n)); CHECK(ssh_base64_decode("AB==",4,decoded,sizeof(decoded))<0);
    CHECK(ssh_base64_decode("AAA=",4,decoded,1)<0); CHECK(ssh_base64_decode("====",4,decoded,sizeof(decoded))<0);
    char line[256]; const char prefix[]=SSH_KEY_TYPE " ";
    memcpy(line,prefix,sizeof(prefix)-1); memcpy(line+sizeof(prefix)-1,encoded,141);
    CHECK(ssh_init(&engine,seed,private_key));
    CHECK(ssh_authorized_line(&engine,line,strlen(line))==1 && engine.key_count==1);
    CHECK(ssh_authorized_line(&engine,line,strlen(line))==1 && engine.key_count==1);
    CHECK(ssh_authorized_line(&engine,"restrict ecdsa-sha2-nistp256 aaa",30)==-2);
    CHECK(ssh_authorized_line(&engine,"ssh-ed25519 AAAA",16)==-1);
    CHECK(ssh_authorized_line(&engine,"from=host ecdsa-sha2-nistp256 AAAA",32)==-2);
    uint8_t zero[32]={0}; CHECK(!ssh_host_public(zero,point));
}
static size_t frame(uint8_t *out,const uint8_t *payload,size_t n)
{
    ssh_writer w={out,0,SSH_PACKET_MAX+40,0};
    size_t pad=8-(n+5)%8; if(pad<4)pad+=8;
    ssh_put_u32(&w,(uint32_t)(n+pad+1)); ssh_put_byte(&w,(uint8_t)pad);
    ssh_put_bytes(&w,payload,n); memset(out+w.n,0,pad); return w.n+pad;
}
static uint8_t wire[SSH_PACKET_MAX+40], payload[9000];
static void reset_connection(void)
{
    CHECK(ssh_init(&engine,seed,private_key)); engine.out_len=0;
    engine.identified=1; engine.kex=0; engine.established=1; engine.tx.seq=engine.rx.seq=0;
    engine.service=1; engine.authenticated=1;
}
static void framing(void)
{
    uint8_t key[16]={0}, iv[16]={0}, mkey[32]={0};
    for(size_t chunk=1;chunk<=72;chunk++) {
        reset_connection();
        br_aes_ct64_ctrcbc_init(&engine.tx.aes,key,16);
        br_hmac_key_init(&engine.tx.mac,&br_sha256_vtable,mkey,32);
        engine.tx.active=1; memcpy(engine.tx.iv,iv,16); engine.rx=engine.tx;
        uint8_t ignore[5]={2,0,0,0,0}; CHECK(ssh_packet_send(&engine,ignore,sizeof(ignore)));
        const uint8_t *p; size_t n=ssh_output(&engine,&p); memcpy(wire,p,n); engine.out_len=0;
        for(size_t off=0;off<n;) {
            size_t count=n-off; if(count>chunk)count=chunk;
            size_t used=ssh_receive(&engine,wire+off,count); CHECK(used>0); if(!used)break; off+=used;
        }
        CHECK(!engine.closed && engine.rx.seq==1);
    }
    reset_connection();
    br_aes_ct64_ctrcbc_init(&engine.tx.aes,key,16);
    br_hmac_key_init(&engine.tx.mac,&br_sha256_vtable,mkey,32);
    engine.tx.active=1; engine.rx=engine.tx;
    uint8_t ignore[5]={2,0,0,0,0}; ssh_packet_send(&engine,ignore,sizeof(ignore));
    const uint8_t *p; size_t n=ssh_output(&engine,&p); memcpy(wire,p,n); engine.out_len=0; wire[n-1]^=1;
    CHECK(ssh_receive(&engine,wire,n)==n && engine.closed);
    for(uint32_t bad=0;bad<12;bad++) {
        reset_connection(); memset(wire,0,16); ssh_writer w={wire,0,4,0}; ssh_put_u32(&w,bad);
        ssh_receive(&engine,wire,8); CHECK(engine.closed);
    }
    reset_connection(); memset(wire,0,16); ssh_writer w={wire,0,4,0}; ssh_put_u32(&w,SSH_PACKET_MAX+1);
    ssh_receive(&engine,wire,8); CHECK(engine.closed);
}
static void channels(void)
{
    reset_connection();
    ssh_writer w={payload,0,sizeof(payload),0};
    ssh_put_byte(&w,90); ssh_put_text(&w,"session"); ssh_put_u32(&w,17);
    ssh_put_u32(&w,10); ssh_put_u32(&w,3);
    size_t n=frame(wire,payload,w.n); ssh_receive(&engine,wire,n);
    CHECK(engine.channel && engine.peer_channel==17 && engine.peer_window==10);
    engine.started=1;
    CHECK(ssh_send_data(&engine,(const uint8_t *)"abcdef",6,0)==3);
    CHECK(engine.peer_window==7);
    w.n=0; ssh_put_byte(&w,94); ssh_put_u32(&w,0); ssh_put_text(&w,"input");
    n=frame(wire,payload,w.n); ssh_receive(&engine,wire,n);
    CHECK(engine.event==SSH_EVENT_INPUT && engine.event_len==5 && engine.receive_window==SSH_WINDOW-5);
    ssh_input_consumed(&engine,5); CHECK(engine.receive_window==SSH_WINDOW);
    /* Credit is owed through a key exchange (nothing enters the deferred-
     * reply budget) and a full queue, then goes out as one adjust. */
    w.n=0; ssh_put_byte(&w,94); ssh_put_u32(&w,0); ssh_put_text(&w,"abcdefgh");
    n=frame(wire,payload,w.n); ssh_receive(&engine,wire,n); CHECK(engine.receive_window==SSH_WINDOW-8);
    engine.kex=1; size_t deferred=engine.deferred_len; engine.out_len=0;
    ssh_input_consumed(&engine,3); ssh_input_consumed(&engine,2);
    CHECK(engine.credit_owed==5 && engine.deferred_len==deferred && !engine.out_len && engine.receive_window==SSH_WINDOW-8);
    engine.kex=0; engine.out_len=SSH_OUTPUT_CAP; ssh_input_consumed(&engine,3);
    CHECK(engine.credit_owed==8 && engine.receive_window==SSH_WINDOW-8);
    engine.out_len=0; ssh_input_consumed(&engine,0);
    CHECK(!engine.credit_owed && engine.receive_window==SSH_WINDOW && engine.out_len && engine.output[engine.out_head+5]==93);
    engine.out_len=0;
    /* A data packet takes what fits the free room, never all or nothing;
     * stderr's header is 4 bytes longer. */
    engine.peer_window=1000; engine.peer_packet=32768; static const uint8_t bulk[400];
    engine.out_len=SSH_OUTPUT_CAP-300; CHECK(ssh_send_data(&engine,bulk,400,0)==235 && engine.out_len<=SSH_OUTPUT_CAP);
    engine.out_len=SSH_OUTPUT_CAP-300; CHECK(ssh_send_data(&engine,bulk,400,1)==231 && engine.out_len<=SSH_OUTPUT_CAP);
    engine.out_len=SSH_OUTPUT_CAP-65; CHECK(!ssh_send_data(&engine,bulk,400,0));
    engine.out_len=SSH_OUTPUT_CAP-69; CHECK(!ssh_send_data(&engine,bulk,400,1));
    engine.out_len=0;
    w.n=0; ssh_put_byte(&w,93); ssh_put_u32(&w,0); ssh_put_u32(&w,UINT32_MAX);
    n=frame(wire,payload,w.n); ssh_receive(&engine,wire,n); CHECK(engine.closed);
    reset_connection();
    w.n=0; ssh_put_byte(&w,94); ssh_put_u32(&w,0); ssh_put_text(&w,"unauthorized");
    n=frame(wire,payload,w.n); ssh_receive(&engine,wire,n); CHECK(engine.closed);
}
static void request_dimensions(const char *name, uint32_t cols, uint32_t rows, int bad_modes)
{
    ssh_writer w={payload,0,sizeof(payload),0};
    ssh_put_byte(&w,98); ssh_put_u32(&w,0); ssh_put_text(&w,name); ssh_put_byte(&w,1);
    int pty=ssh_equal((ssh_reader){(const uint8_t *)name,strlen(name),0},"pty-req");
    if(pty) ssh_put_text(&w,"xterm");
    ssh_put_u32(&w,cols); ssh_put_u32(&w,rows); ssh_put_u32(&w,0); ssh_put_u32(&w,0);
    if(pty) { uint8_t modes=bad_modes?1:0; ssh_put_string(&w,&modes,1); }
    size_t n=frame(wire,payload,w.n); CHECK(ssh_receive(&engine,wire,n)==n);
}
static void refused_dimensions(void)
{
    reset_connection(); engine.channel=1;
    uint32_t cols=engine.cols, rows=engine.rows;
    request_dimensions("window-change",111,37,0);
    CHECK(!engine.closed && !engine.pty && engine.event!=SSH_EVENT_RESIZE);
    CHECK(engine.cols==cols && engine.rows==rows);
    request_dimensions("pty-req",123,45,1);
    CHECK(!engine.closed && !engine.pty);
    CHECK(engine.cols==cols && engine.rows==rows);
    request_dimensions("pty-req",0,0,0);
    CHECK(engine.pty && engine.cols==cols && engine.rows==rows);
    request_dimensions("window-change",101,0,0);
    CHECK(engine.event==SSH_EVENT_RESIZE && engine.cols==cols && engine.rows==rows);
    ssh_resize_result(&engine,1);
    CHECK(engine.cols==101 && engine.rows==rows);
    request_dimensions("window-change",0,43,0);
    CHECK(engine.event==SSH_EVENT_RESIZE && engine.rows==rows);
    ssh_resize_result(&engine,1);
    CHECK(engine.cols==101 && engine.rows==43);
    request_dimensions("window-change",222,65536,0);
    CHECK(!engine.closed && engine.cols==101 && engine.rows==43);
}
static void dimension_bounds(void)
{
    const uint32_t invalid[][2]={{1,24},{80,1},{513,24},{80,257},{600,300},{65535,65535},{UINT32_MAX,24}};
    for(unsigned live=0;live<2;live++) for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        reset_connection(); engine.channel=1; engine.pty=live;
        uint32_t cols=engine.cols,rows=engine.rows;
        request_dimensions(live?"window-change":"pty-req",invalid[i][0],invalid[i][1],0);
        CHECK(!engine.closed && engine.pty==(int)live && engine.event!=SSH_EVENT_RESIZE);
        CHECK(engine.cols==cols && engine.rows==rows);
        CHECK(engine.out_len>=6 && engine.output[5]==100);
    }
    reset_connection(); engine.channel=1;
    request_dimensions("pty-req",2,2,0);
    CHECK(engine.pty && engine.cols==2 && engine.rows==2);
    reset_connection(); engine.channel=1;
    request_dimensions("pty-req",512,256,0);
    CHECK(engine.pty && engine.cols==512 && engine.rows==256);
}
static void resize_results(void)
{
    reset_connection(); engine.channel=engine.pty=1;
    uint32_t cols=engine.cols,rows=engine.rows;
    request_dimensions("window-change",103,41,0);
    CHECK(engine.event==SSH_EVENT_RESIZE && !engine.out_len);
    CHECK(engine.cols==cols && engine.rows==rows && engine.resize_cols==103 && engine.resize_rows==41);
    ssh_resize_result(&engine,0);
    CHECK(engine.event==SSH_EVENT_NONE && engine.cols==cols && engine.rows==rows);
    CHECK(engine.out_len>=6 && engine.output[5]==100);
    size_t replied=engine.out_len; ssh_resize_result(&engine,1);
    CHECK(engine.out_len==replied && engine.cols==cols && engine.rows==rows);
    engine.out_len=0;
    request_dimensions("window-change",0,0,0);
    CHECK(!engine.out_len && engine.resize_cols==cols && engine.resize_rows==rows);
    ssh_resize_result(&engine,1);
    CHECK(engine.cols==cols && engine.rows==rows && engine.output[5]==99);
    engine.out_len=0;
    request_dimensions("window-change",512,256,0);
    engine.request_reply=0; ssh_resize_result(&engine,0);
    CHECK(!engine.out_len && engine.cols==cols && engine.rows==rows);
    request_dimensions("window-change",2,2,0); ssh_resize_result(&engine,1);
    CHECK(engine.cols==2 && engine.rows==2);
    request_dimensions("window-change",512,256,0); ssh_resize_result(&engine,1);
    CHECK(engine.cols==512 && engine.rows==256);
}
static void request_pty_term(const char *term)
{
    ssh_writer w={payload,0,sizeof(payload),0};
    ssh_put_byte(&w,98); ssh_put_u32(&w,0); ssh_put_text(&w,"pty-req"); ssh_put_byte(&w,0);
    ssh_put_text(&w,term); ssh_put_u32(&w,80); ssh_put_u32(&w,24); ssh_put_u32(&w,0); ssh_put_u32(&w,0);
    uint8_t modes=0; ssh_put_string(&w,&modes,1);
    size_t n=frame(wire,payload,w.n); CHECK(ssh_receive(&engine,wire,n)==n);
}
static void term_env(void)
{
    /* TERM reaches the shell only as a plain printable name. */
    reset_connection(); engine.channel=1;
    CHECK(!ssh_term_env(&engine));
    request_pty_term("xterm-256color");
    CHECK(engine.pty && ssh_term_env(&engine)==engine.term);
    CHECK(ssh_equal((ssh_reader){(const uint8_t *)engine.term,strlen(engine.term),0},"xterm-256color"));
    const char *bad[]={"","vt 100","TERM=xterm","x\x7fy","\xe9term"};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        reset_connection(); engine.channel=1;
        request_pty_term(bad[i]);
        CHECK(engine.pty && !ssh_term_env(&engine));
    }
}
static void deferred_close(void)
{
    reset_connection(); engine.channel=1; engine.peer_channel=17; engine.kex=1;
    ssh_writer w={payload,0,sizeof(payload),0};
    ssh_put_byte(&w,97); ssh_put_u32(&w,0);
    size_t n=frame(wire,payload,w.n);
    CHECK(ssh_receive(&engine,wire,n)==n);
    CHECK(engine.event==SSH_EVENT_CLOSE && engine.sent_close && engine.deferred_len && !engine.out_len);
    /* Stand at the post-ECDH seam with fixture pending keys. NEWKEYS must
     * release the real connection layer's deferred reciprocal close. */
    engine.kex=3; payload[0]=21; n=frame(wire,payload,1);
    CHECK(ssh_receive(&engine,wire,n)==n);
    CHECK(!engine.closed && !engine.kex && !engine.deferred_len && engine.rx.active);
    CHECK(engine.out_len>=10 && engine.output[5]==97 && engine.output[9]==17);
}
/* The payload of the last packet the engine queued, from plaintext output
 * (reset_connection leaves the transmit direction without keys). */
static const uint8_t *last_sent(size_t *len)
{
    const uint8_t *p=engine.output+engine.out_head, *last=0; size_t left=engine.out_len; *len=0;
    while(left>=5) {
        uint32_t total=((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
        if(total+4>left || total<1u+p[4]) break;
        last=p+5; *len=total-1-p[4]; p+=total+4; left-=total+4;
    }
    return last;
}
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static void send_open(uint32_t id, const char *host, uint32_t port, uint32_t window, uint32_t max, int trailing)
{
    ssh_writer w={payload,0,sizeof(payload),0};
    ssh_put_byte(&w,90); ssh_put_text(&w,"direct-tcpip"); ssh_put_u32(&w,id);
    ssh_put_u32(&w,window); ssh_put_u32(&w,max);
    ssh_put_text(&w,host); ssh_put_u32(&w,port); ssh_put_text(&w,"127.0.0.1"); ssh_put_u32(&w,50000);
    if(trailing) ssh_put_byte(&w,0);
    size_t n=frame(wire,payload,w.n); CHECK(ssh_receive(&engine,wire,n)==n);
}
static void send_channel(uint8_t type, uint32_t id, const char *text, uint32_t value)
{
    ssh_writer w={payload,0,sizeof(payload),0};
    ssh_put_byte(&w,type); ssh_put_u32(&w,id);
    if(type==93) ssh_put_u32(&w,value);
    if(type==94) ssh_put_text(&w,text);
    if(type==98) { ssh_put_text(&w,text); ssh_put_byte(&w,(uint8_t)value); }
    size_t n=frame(wire,payload,w.n); ssh_receive(&engine,wire,n);
}
/* An open the caller confirms: forward 0, local channel 1, peer channel `id`. */
static void open_forward(uint32_t id, uint32_t window)
{
    send_open(id,"localhost",5900,window,32768,0);
    CHECK(engine.event==SSH_EVENT_FORWARD_OPEN);
    uint32_t f=engine.event_forward;
    ssh_forward_result(&engine,f,1,0,"");
    CHECK(engine.forwards[f].state==SSH_FORWARD_OPEN && engine.event==SSH_EVENT_NONE);
}
static void forwards(void)
{
    size_t len; const uint8_t *p;
    /* Policy none: refused with the reason, nothing handed to the caller. */
    reset_connection(); engine.out_len=0;
    send_open(40,"localhost",5900,1000,32768,0);
    p=last_sent(&len);
    CHECK(engine.event==SSH_EVENT_NONE && p && p[0]==92 && be32(p+1)==40 && be32(p+5)==1);
    CHECK(!ssh_forwards_live(&engine));
    /* Loopback policy: the destination reaches the caller, and success
     * confirms with our channel 1, the forward window and packet size. */
    reset_connection(); engine.forward_policy=SSH_FORWARD_LOOPBACK;
    send_open(41,"localhost",5900,1000,32768,0);
    CHECK(engine.event==SSH_EVENT_FORWARD_OPEN && engine.event_forward==0);
    CHECK(!memcmp(engine.forwards[0].host,"localhost",10) && engine.forwards[0].port==5900);
    CHECK(engine.forwards[0].state==SSH_FORWARD_OPENING && ssh_forwards_live(&engine)==1);
    engine.out_len=0; ssh_forward_result(&engine,0,1,0,""); p=last_sent(&len);
    CHECK(p && len==17 && p[0]==91 && be32(p+1)==41 && be32(p+5)==1 && be32(p+9)==SSH_FORWARD_WINDOW && be32(p+13)==SSH_DATA_MAX);
    /* Data in: an event with the bytes, the window spent, then credited. */
    send_channel(94,1,"keys",0);
    CHECK(engine.event==SSH_EVENT_FORWARD_DATA && engine.event_forward==0 && engine.event_len==4 && !memcmp(engine.event_data,"keys",4));
    CHECK(engine.forwards[0].receive_window==SSH_FORWARD_WINDOW-4);
    engine.out_len=0; ssh_forward_consumed(&engine,0,4); p=last_sent(&len);
    CHECK(engine.forwards[0].receive_window==SSH_FORWARD_WINDOW && p && p[0]==93 && be32(p+1)==41 && be32(p+5)==4);
    /* A credit the output queue has no room for is kept, not lost, and a
     * later call (n = 0 retries alone) sends it all in one adjust. The
     * window check counts what is owed. */
    send_channel(94,1,"abcdefgh",0); CHECK(engine.forwards[0].receive_window==SSH_FORWARD_WINDOW-8);
    engine.out_len=SSH_OUTPUT_CAP; ssh_forward_consumed(&engine,0,3); ssh_forward_consumed(&engine,0,2);
    CHECK(!engine.closed && engine.forwards[0].credit_owed==5 && engine.forwards[0].receive_window==SSH_FORWARD_WINDOW-8);
    engine.out_len=0; ssh_forward_consumed(&engine,0,0); p=last_sent(&len);
    CHECK(p && p[0]==93 && be32(p+5)==5 && !engine.forwards[0].credit_owed && engine.forwards[0].receive_window==SSH_FORWARD_WINDOW-3);
    send_channel(94,1,"wxyz",0); engine.kex=1; size_t deferred=engine.deferred_len;
    ssh_forward_consumed(&engine,0,2); ssh_forward_consumed(&engine,0,2);
    CHECK(engine.forwards[0].credit_owed==4 && engine.deferred_len==deferred);
    engine.kex=0; engine.out_len=0; ssh_forward_consumed(&engine,0,0); p=last_sent(&len);
    CHECK(p && p[0]==93 && be32(p+5)==4 && !engine.forwards[0].credit_owed && engine.forwards[0].receive_window==SSH_FORWARD_WINDOW-3);
    engine.out_len=SSH_OUTPUT_CAP; ssh_forward_consumed(&engine,0,3); CHECK(!engine.closed && engine.forwards[0].credit_owed==3);
    engine.out_len=0; ssh_forward_consumed(&engine,0,1); CHECK(engine.closed);
    reset_connection(); engine.forward_policy=SSH_FORWARD_LOOPBACK; open_forward(41,1000);
    /* Data out is clamped by the peer's window, and an adjust restores it. */
    CHECK(ssh_forward_send(&engine,0,(const uint8_t *)"0123456789abcdef",16)==16);
    CHECK(engine.forwards[0].peer_window==984);
    uint8_t big[2000]; memset(big,'x',sizeof(big));
    CHECK(ssh_forward_send(&engine,0,big,sizeof(big))==984 && !engine.forwards[0].peer_window);
    CHECK(ssh_forward_send(&engine,0,big,1)==0);
    send_channel(93,1,0,500); CHECK(!engine.closed && engine.forwards[0].peer_window==500);
    /* ...and by the output queue's free room, never refused whole. */
    engine.out_len=SSH_OUTPUT_CAP-300; CHECK(ssh_forward_send(&engine,0,big,400)==235 && engine.out_len<=SSH_OUTPUT_CAP);
    engine.out_len=SSH_OUTPUT_CAP-65; CHECK(ssh_forward_send(&engine,0,big,400)==0);
    CHECK(ssh_output_full(&engine)); engine.out_len=SSH_OUTPUT_CAP-66; CHECK(!ssh_output_full(&engine));
    engine.out_len=0; CHECK(engine.forwards[0].peer_window==265);
    /* A request on a forward is refused when it asks for an answer. */
    engine.out_len=0; send_channel(98,1,"env",1); p=last_sent(&len);
    CHECK(!engine.closed && p && p[0]==100 && be32(p+1)==41);
    /* Local end first: EOF and CLOSE go out; the slot waits for theirs. */
    engine.out_len=0; CHECK(ssh_forward_finish(&engine,0)); p=last_sent(&len);
    CHECK(engine.forwards[0].sent_close && p && p[0]==97 && be32(p+1)==41);
    CHECK(engine.out_len && engine.output[5]==96);
    send_channel(94,1,"late",0); CHECK(!engine.closed && engine.event==SSH_EVENT_NONE);
    engine.out_len=0; send_channel(97,1,0,0);
    CHECK(engine.event==SSH_EVENT_FORWARD_CLOSE && engine.event_forward==0 && !engine.out_len);
    /* The channel is dead but the slot stays taken until the caller has
     * delivered what the client sent, and says so. */
    CHECK(engine.forwards[0].state==SSH_FORWARD_CLOSED && ssh_forwards_live(&engine)==1);
    engine.out_len=0; CHECK(!ssh_forward_send(&engine,0,big,1) && ssh_forward_finish(&engine,0) && !engine.out_len);
    ssh_forward_consumed(&engine,0,4); CHECK(!engine.out_len && !engine.closed);
    ssh_forward_release(&engine,0); CHECK(engine.forwards[0].state==SSH_FORWARD_FREE && !ssh_forwards_live(&engine));
    /* A dead channel number is a protocol violation. */
    send_channel(94,1,"ghost",0); CHECK(engine.closed);
    reset_connection(); engine.forward_policy=SSH_FORWARD_LOOPBACK; open_forward(42,1000);
    engine.out_len=0; send_channel(97,1,0,0);
    send_channel(94,1,"ghost",0); CHECK(engine.closed);
    /* Peer closes first: we answer with our CLOSE; the slot waits for release. */
    reset_connection(); engine.forward_policy=SSH_FORWARD_LOOPBACK; open_forward(42,1000);
    engine.out_len=0; send_channel(97,1,0,0); p=last_sent(&len);
    CHECK(engine.event==SSH_EVENT_FORWARD_CLOSE && p && p[0]==97 && be32(p+1)==42 && ssh_forwards_live(&engine)==1);
    ssh_forward_release(&engine,0); CHECK(!ssh_forwards_live(&engine));
    /* A released slot's peer number may be used again; a live one may not. */
    open_forward(42,1000); CHECK(!engine.closed && ssh_forwards_live(&engine)==1);
    send_open(42,"localhost",5900,1000,32768,0); CHECK(engine.closed);
    /* ...and a CLOSED one's may, before release: the number died when the
     * CLOSEs crossed, and the slot is still taken, so it gets another. */
    reset_connection(); engine.forward_policy=SSH_FORWARD_LOOPBACK; open_forward(42,1000);
    send_channel(97,1,0,0); send_open(42,"localhost",5900,1000,32768,0);
    CHECK(!engine.closed && engine.event==SSH_EVENT_FORWARD_OPEN && engine.event_forward==1);
    /* EOF from the client is an event, and data after it is a violation. */
    reset_connection(); engine.forward_policy=SSH_FORWARD_LOOPBACK; open_forward(43,1000);
    send_channel(96,1,0,0); CHECK(engine.event==SSH_EVENT_FORWARD_EOF && engine.forwards[0].input_eof);
    send_channel(94,1,"after",0); CHECK(engine.closed);
    /* Window violations disconnect: overflowing an adjust, overrunning ours. */
    reset_connection(); engine.forward_policy=SSH_FORWARD_LOOPBACK; open_forward(44,1000);
    send_channel(93,1,0,UINT32_MAX); CHECK(engine.closed);
    reset_connection(); engine.forward_policy=SSH_FORWARD_LOOPBACK; open_forward(45,1000);
    engine.forwards[0].receive_window=3; send_channel(94,1,"four",0); CHECK(engine.closed);
    /* The caller's refusal carries its reason and frees the slot. */
    reset_connection(); engine.forward_policy=SSH_FORWARD_LOOPBACK;
    send_open(46,"example.com",80,1000,32768,0); CHECK(engine.event==SSH_EVENT_FORWARD_OPEN);
    engine.out_len=0; ssh_forward_result(&engine,0,0,1,"forwarding reaches this machine's loopback only"); p=last_sent(&len);
    CHECK(p && p[0]==92 && be32(p+1)==46 && be32(p+5)==1 && !ssh_forwards_live(&engine));
    /* A result that answers no pending open changes nothing. */
    engine.out_len=0; ssh_forward_result(&engine,0,1,0,""); CHECK(!engine.out_len && !ssh_forwards_live(&engine));
    /* Destinations the engine refuses on sight, without troubling the caller. */
    const char *bad_hosts[]={"","two words","tab\there"};
    for(unsigned i=0;i<3;i++) {
        reset_connection(); engine.forward_policy=SSH_FORWARD_LOOPBACK;
        send_open(47,bad_hosts[i],5900,1000,32768,0); p=last_sent(&len);
        CHECK(engine.event==SSH_EVENT_NONE && p && p[0]==92 && be32(p+5)==1 && !ssh_forwards_live(&engine));
    }
    char long_host[SSH_FORWARD_HOST_MAX+2]; memset(long_host,'a',sizeof(long_host)-1); long_host[sizeof(long_host)-1]=0;
    reset_connection(); engine.forward_policy=SSH_FORWARD_LOOPBACK;
    send_open(48,long_host,5900,1000,32768,0); CHECK(engine.event==SSH_EVENT_NONE && !ssh_forwards_live(&engine));
    long_host[SSH_FORWARD_HOST_MAX]=0;
    send_open(48,long_host,5900,1000,32768,0); CHECK(engine.event==SSH_EVENT_FORWARD_OPEN);
    uint32_t bad_ports[]={0,65536};
    for(unsigned i=0;i<2;i++) {
        reset_connection(); engine.forward_policy=SSH_FORWARD_LOOPBACK;
        send_open(49,"localhost",bad_ports[i],1000,32768,0);
        CHECK(engine.event==SSH_EVENT_NONE && !ssh_forwards_live(&engine));
    }
    reset_connection(); engine.forward_policy=SSH_FORWARD_LOOPBACK;
    send_open(50,"localhost",5900,1000,0,0); CHECK(engine.event==SSH_EVENT_NONE && !ssh_forwards_live(&engine));
    reset_connection(); engine.forward_policy=SSH_FORWARD_LOOPBACK;
    send_open(51,"localhost",5900,1000,32768,1); CHECK(engine.closed);
    /* Seven forwards, then RESOURCE_SHORTAGE; a freed slot is reused. */
    reset_connection(); engine.forward_policy=SSH_FORWARD_LOOPBACK;
    for(uint32_t i=0;i<SSH_FORWARDS;i++) { open_forward(60+i,1000); CHECK(engine.forwards[i].peer_channel==60+i); }
    engine.out_len=0; send_open(70,"localhost",5900,1000,32768,0); p=last_sent(&len);
    CHECK(engine.event==SSH_EVENT_NONE && p && p[0]==92 && be32(p+1)==70 && be32(p+5)==4);
    send_channel(97,4,0,0); CHECK(engine.event==SSH_EVENT_FORWARD_CLOSE && engine.event_forward==3);
    engine.out_len=0; send_open(71,"localhost",5900,1000,32768,0); p=last_sent(&len);
    CHECK(engine.event==SSH_EVENT_NONE && p && p[0]==92 && be32(p+5)==4);
    ssh_forward_release(&engine,3);
    send_open(71,"localhost",5900,1000,32768,0); CHECK(engine.event==SSH_EVENT_FORWARD_OPEN && engine.event_forward==3);
    ssh_forward_result(&engine,3,1,0,""); CHECK(ssh_forwards_live(&engine)==SSH_FORWARDS);
    send_channel(94,8,"none",0); CHECK(engine.closed);
    /* The session and a forward share the connection without crossing. */
    reset_connection(); engine.forward_policy=SSH_FORWARD_LOOPBACK;
    ssh_writer w={payload,0,sizeof(payload),0};
    ssh_put_byte(&w,90); ssh_put_text(&w,"session"); ssh_put_u32(&w,17); ssh_put_u32(&w,1000); ssh_put_u32(&w,32768);
    size_t n=frame(wire,payload,w.n); ssh_receive(&engine,wire,n); CHECK(engine.channel);
    engine.started=1; open_forward(18,1000);
    send_channel(94,0,"shell",0); CHECK(engine.event==SSH_EVENT_INPUT && engine.event_len==5);
    send_channel(94,1,"vnc",0); CHECK(engine.event==SSH_EVENT_FORWARD_DATA && engine.event_len==3);
    CHECK(engine.receive_window==SSH_WINDOW-5 && engine.forwards[0].receive_window==SSH_FORWARD_WINDOW-3);
    /* One number per live channel, whichever kind asks: a forward may not
     * take the session's, nor a second session a forward's. */
    { reset_connection(); engine.forward_policy=SSH_FORWARD_LOOPBACK;
      ssh_writer v={payload,0,sizeof(payload),0};
      ssh_put_byte(&v,90); ssh_put_text(&v,"session"); ssh_put_u32(&v,17); ssh_put_u32(&v,1000); ssh_put_u32(&v,32768);
      size_t m=frame(wire,payload,v.n); ssh_receive(&engine,wire,m); CHECK(engine.channel);
      send_open(17,"localhost",5900,1000,32768,0); CHECK(engine.closed);
      reset_connection(); engine.forward_policy=SSH_FORWARD_LOOPBACK; open_forward(19,1000);
      v.n=0; ssh_put_byte(&v,90); ssh_put_text(&v,"session"); ssh_put_u32(&v,19); ssh_put_u32(&v,1000); ssh_put_u32(&v,32768);
      m=frame(wire,payload,v.n); ssh_receive(&engine,wire,m); CHECK(engine.closed && !engine.channel); }
    reset_connection(); engine.forward_policy=SSH_FORWARD_LOOPBACK;
    n=frame(wire,payload,w.n); ssh_receive(&engine,wire,n); CHECK(engine.channel);
    engine.started=1; open_forward(18,1000);
    send_channel(97,0,0,0); CHECK(engine.event==SSH_EVENT_CLOSE && ssh_forwards_live(&engine)==1);
    /* The session's number is free once its CLOSEs have crossed. */
    send_open(17,"localhost",5900,1000,32768,0); CHECK(!engine.closed && engine.event==SSH_EVENT_FORWARD_OPEN);
    ssh_forward_result(&engine,engine.event_forward,0,1,"");
    /* A key exchange holds a forward's close, as it holds exit-status. */
    engine.kex=1; CHECK(!ssh_forward_finish(&engine,0) && !engine.forwards[0].sent_close);
    engine.kex=0; CHECK(ssh_forward_finish(&engine,0) && engine.forwards[0].sent_close);
    /* Out-of-range and non-open forwards are inert to the caller's calls. */
    CHECK(!ssh_forward_send(&engine,SSH_FORWARDS,big,1) && ssh_forward_finish(&engine,SSH_FORWARDS));
}
static void authentication(void)
{
    uint8_t point[65], blob[104], digest[32], raw[64], pair[80], sig[128], sid[36];
    ssh_host_public(private_key,point); size_t blob_len=ssh_public_blob(blob,sizeof(blob),point);
    for(int tamper=0;tamper<3;tamper++) {
        reset_connection(); engine.authenticated=0;
        engine.key_count=1; memcpy(engine.keys[0].blob,blob,blob_len); engine.keys[0].len=blob_len;
        ssh_writer w={payload,0,sizeof(payload),0};
        ssh_put_byte(&w,50); ssh_put_text(&w,"os64"); ssh_put_text(&w,"ssh-connection");
        ssh_put_text(&w,"publickey"); ssh_put_byte(&w,1); ssh_put_text(&w,SSH_KEY_TYPE); ssh_put_string(&w,blob,blob_len);
        ssh_writer id={sid,0,sizeof(sid),0}; ssh_put_string(&id,engine.session_id,32);
        br_sha256_context h; br_sha256_init(&h); br_sha256_update(&h,sid,id.n); br_sha256_update(&h,payload,w.n); br_sha256_out(&h,digest);
        br_ec_private_key sk={BR_EC_secp256r1,private_key,32};
        CHECK(br_ecdsa_i31_sign_raw(&br_ec_p256_m31,&br_sha256_vtable,digest,&sk,raw)==64);
        if(tamper==1) raw[7]^=1;
        if(tamper==2) payload[5]^=1;
        ssh_writer iw={pair,0,sizeof(pair),0}; ssh_put_mpint(&iw,raw,32); ssh_put_mpint(&iw,raw+32,32);
        ssh_writer sw={sig,0,sizeof(sig),0}; ssh_put_text(&sw,SSH_KEY_TYPE); ssh_put_string(&sw,pair,iw.n);
        ssh_put_string(&w,sig,sw.n); size_t n=frame(wire,payload,w.n); ssh_receive(&engine,wire,n);
        CHECK(engine.authenticated==(tamper==0));
        CHECK(tamper==0 ? engine.event==SSH_EVENT_AUTH : engine.failures==1);
    }
    reset_connection(); engine.authenticated=0;
    ssh_writer w={payload,0,sizeof(payload),0};
    ssh_put_byte(&w,50); ssh_put_text(&w,"os64"); ssh_put_text(&w,"ssh-connection"); ssh_put_text(&w,"none");
    size_t n=frame(wire,payload,w.n);
    for(int i=0;i<5;i++) ssh_receive(&engine,wire,n);
    CHECK(engine.closed && engine.failures==5);
}
static void preauth_messages(void)
{
    /* RFC 4253 section 11.4: before authentication every message but a
     * userauth request earns UNIMPLEMENTED with its sequence number, known
     * type or not. One value from every RFC 4250 range and the 83-89 gap;
     * a second service request is the transport's, refused there. */
    const uint8_t other[]={21,31,49,52,60,61,79,80,83,89,90,100,101,127,192,255};
    for(size_t i=0;i<sizeof(other);i++) {
        reset_connection(); engine.authenticated=0; engine.rx.seq=6;
        payload[0]=other[i]; payload[1]=0; size_t n=frame(wire,payload,2);
        CHECK(ssh_receive(&engine,wire,n)==n && !engine.closed && !engine.authenticated);
        CHECK(engine.out_len==16 && engine.output[5]==3 && engine.output[9]==6);
    }
}
static void exchange_refusals(void)
{
    for(size_t len=31;len<=33;len++) {
        CHECK(ssh_init(&engine,seed,private_key));
        engine.out_len=0;
        const uint8_t ident[]="SSH-2.0-fixture\r\n";
        CHECK(ssh_receive(&engine,ident,sizeof(ident)-1)==sizeof(ident)-1);
        memcpy(payload,engine.server_kex,engine.server_kex_len);
        size_t n=frame(wire,payload,engine.server_kex_len);
        CHECK(ssh_receive(&engine,wire,n)==n && engine.kex==2);
        uint8_t zero[33]={0}; ssh_writer w={payload,0,sizeof(payload),0};
        ssh_put_byte(&w,30); ssh_put_string(&w,zero,len);
        n=frame(wire,payload,w.n); ssh_receive(&engine,wire,n);
        CHECK(engine.closed && !engine.established);
    }
}
static void fresh_client(void)
{
    CHECK(ssh_init(&engine,seed,private_key)); engine.out_len=0;
    const uint8_t ident[]="SSH-2.0-fixture\r\n";
    CHECK(ssh_receive(&engine,ident,sizeof(ident)-1)==sizeof(ident)-1 && engine.identified);
}
static void client_kexinit(int strict)
{
    ssh_writer w={payload,0,sizeof(payload),0}; uint8_t cookie[16]={0};
    ssh_put_byte(&w,20); ssh_put_bytes(&w,cookie,16);
    ssh_put_text(&w,strict?"curve25519-sha256,kex-strict-c-v00@openssh.com":"curve25519-sha256");
    ssh_put_text(&w,SSH_KEY_TYPE);
    ssh_put_text(&w,"aes128-ctr"); ssh_put_text(&w,"aes128-ctr");
    ssh_put_text(&w,"hmac-sha2-256"); ssh_put_text(&w,"hmac-sha2-256");
    ssh_put_text(&w,"none"); ssh_put_text(&w,"none");
    ssh_put_text(&w,""); ssh_put_text(&w,"");
    ssh_put_byte(&w,0); ssh_put_u32(&w,0);
    size_t n=frame(wire,payload,w.n); CHECK(ssh_receive(&engine,wire,n)==n);
}
static void transport_message(uint8_t type, int malformed)
{
    ssh_writer w={payload,0,sizeof(payload),0}; ssh_put_byte(&w,type);
    if(type==3) ssh_put_u32(&w,9);
    else { if(type==4) ssh_put_byte(&w,malformed?2:1); ssh_put_text(&w,"note"); if(type==4) ssh_put_text(&w,""); }
    if(malformed && type!=4) ssh_put_byte(&w,0);
    size_t n=frame(wire,payload,w.n); CHECK(ssh_receive(&engine,wire,n)==n);
}
static void transport_messages(void)
{
    /* RFC 4253 section 11: IGNORE, UNIMPLEMENTED and DEBUG are legal at any
     * time after identification. Strict KEX forbids them during the initial
     * exchange only, which is OpenSSH's KEX_INITIAL rule. */
    fresh_client(); transport_message(2,0); CHECK(!engine.closed && engine.rx.seq==1);
    client_kexinit(0); CHECK(!engine.closed && engine.kex==2 && !engine.strict);
    transport_message(2,0); transport_message(3,0); transport_message(4,0);
    CHECK(!engine.closed && engine.kex==2);
    transport_message(4,1); CHECK(engine.closed);
    fresh_client(); client_kexinit(0); transport_message(2,1); CHECK(engine.closed);
    fresh_client(); client_kexinit(1); CHECK(!engine.closed && engine.strict && engine.kex==2);
    transport_message(2,0); CHECK(engine.closed);
    fresh_client(); client_kexinit(1); transport_message(4,0); CHECK(engine.closed);
    /* A strict client's first packet must be KEXINIT; an IGNORE ahead of it
     * is accepted on arrival and condemned by the sequence check. */
    fresh_client(); transport_message(2,0); CHECK(!engine.closed);
    client_kexinit(1); CHECK(engine.closed);
    /* A rekey runs under live keys: strict or not, the messages stay legal
     * before and after the peer's KEXINIT. */
    for(int strict=0;strict<2;strict++) {
        reset_connection(); engine.strict=strict; engine.kex=1;
        transport_message(2,0); transport_message(4,0); CHECK(!engine.closed && engine.kex==1);
        client_kexinit(strict); CHECK(!engine.closed && engine.kex==2);
        transport_message(2,0); transport_message(3,0); CHECK(!engine.closed && engine.kex==2);
    }
}
static void unknown_during_kex(void)
{
    /* RFC 4253 section 11.4 during an exchange: any message this state does
     * not handle earns UNIMPLEMENTED, known type or not, unless strict KEX
     * is guarding the initial exchange. One value from every RFC 4250 range
     * and the 83-89 gap; KEXINIT and ECDH_INIT are the handled ones. */
    const uint8_t unknown[]={5,6,7,8,19,21,25,29,31,49,50,51,52,60,79,80,83,89,90,100,101,127,192,255};
    for(size_t i=0;i<sizeof(unknown);i++) {
        fresh_client(); client_kexinit(0); engine.out_len=0; engine.rx.seq=9;
        payload[0]=unknown[i]; size_t n=frame(wire,payload,1);
        CHECK(ssh_receive(&engine,wire,n)==n && !engine.closed && engine.kex==2);
        CHECK(engine.out_len==16 && engine.output[5]==3 && engine.output[9]==9);
        fresh_client(); client_kexinit(1); payload[0]=unknown[i]; n=frame(wire,payload,1);
        ssh_receive(&engine,wire,n); CHECK(engine.closed && engine.output[5]==1);
        for(int strict=0;strict<2;strict++) {
            reset_connection(); engine.strict=strict; engine.kex=2; engine.rx.seq=9;
            CHECK(ssh_receive(&engine,wire,n)==n && !engine.closed && engine.kex==2);
            CHECK(engine.out_len==16 && engine.output[5]==3 && engine.output[9]==9);
        }
    }
}
static void identification(void)
{
    /* RFC 4253 section 4.2: 255 bytes including CRLF, so 253 of content. */
    char line[258]; memset(line,'x',sizeof(line)); memcpy(line,"SSH-2.0-",8);
    line[253]='\r'; line[254]='\n';
    CHECK(ssh_init(&engine,seed,private_key)); engine.out_len=0;
    CHECK(ssh_receive(&engine,(const uint8_t *)line,255)==255);
    CHECK(engine.identified && !engine.closed && engine.ident_len==253 && !engine.client_ident[253]);
    line[253]='\n';
    CHECK(ssh_init(&engine,seed,private_key)); engine.out_len=0;
    CHECK(ssh_receive(&engine,(const uint8_t *)line,254)==254 && engine.identified && engine.ident_len==253);
    line[253]='x'; line[254]='\r'; line[255]='\n';
    CHECK(ssh_init(&engine,seed,private_key)); engine.out_len=0;
    ssh_receive(&engine,(const uint8_t *)line,256); CHECK(engine.closed && !engine.identified);
    line[253]='\r'; line[254]='\r';
    CHECK(ssh_init(&engine,seed,private_key)); engine.out_len=0;
    ssh_receive(&engine,(const uint8_t *)line,256); CHECK(engine.closed && !engine.identified);
}
static void deferred_reservation(void)
{
    /* 910 five-byte replies fill the 8 KiB deferred budget at 9 bytes each
     * yet cost 48 wire bytes each once framed and authenticated. NEWKEYS
     * must not be taken until the queue can hold all of them. */
    uint8_t key[16]={0}, mkey[32]={0}, reply[5]={100,0,0,0,17};
    reset_connection(); engine.kex=1;
    br_aes_ct64_ctrcbc_init(&engine.tx.aes,key,16);
    br_hmac_key_init(&engine.tx.mac,&br_sha256_vtable,mkey,32); engine.tx.active=1;
    for(int i=0;i<910;i++) CHECK(ssh_packet_send(&engine,reply,sizeof(reply)));
    CHECK(!engine.closed && !engine.out_len && engine.deferred_len==8190 && engine.deferred_wire==43680);
    engine.kex=3; payload[0]=21; size_t n=frame(wire,payload,1);
    engine.out_len=SSH_OUTPUT_CAP-(SSH_PACKET_MAX+1024);
    CHECK(ssh_receive(&engine,wire,n)==0 && !engine.closed && engine.kex==3 && engine.deferred_len==8190);
    engine.out_len=SSH_OUTPUT_CAP-(SSH_PACKET_MAX+1024+engine.deferred_wire);
    size_t queued=engine.out_len;
    CHECK(ssh_receive(&engine,wire,n)==n && !engine.closed && !engine.kex && engine.established);
    CHECK(!engine.deferred_len && !engine.deferred_wire && engine.out_len==queued+43680);
    CHECK(engine.tx.seq==910);
    reset_connection(); engine.kex=1;
    for(int i=0;i<910;i++) ssh_packet_send(&engine,reply,sizeof(reply));
    CHECK(!engine.closed); CHECK(!ssh_packet_send(&engine,reply,sizeof(reply)) && engine.closed);
}
int main(int argc, char **argv)
{
#ifndef SSH_HOST
    if(argc==2 && os64_streq(argv[1],"-streams")) {
        os64_write(1,"stdout-marker\n",14); os64_write(2,"stderr-marker\n",14); return 7;
    }
    if(argc==2 && os64_streq(argv[1],"-stderr")) {
        uint8_t data[4096]; int64_t got;
        while((got=os64_read(0,data,sizeof(data)))>0) {
            size_t off=0;
            while(off<(size_t)got) {
                int64_t n=os64_write(2,data+off,(size_t)got-off);
                if(n<=0)return 2;
                off+=(size_t)n;
            }
        }
        return got<0?2:0;
    }
#else
    (void)argc; (void)argv;
#endif
    primitives(); codecs(); framing(); channels(); refused_dimensions(); dimension_bounds(); resize_results(); term_env(); deferred_close(); deferred_reservation(); forwards(); authentication(); preauth_messages(); exchange_refusals(); transport_messages(); unknown_during_kex(); identification();
    report("sshtest: %d checks, %d failures\n",checks,failed);
    return failed?1:0;
}
