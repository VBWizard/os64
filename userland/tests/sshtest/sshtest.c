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
    w.n=0; ssh_put_byte(&w,93); ssh_put_u32(&w,0); ssh_put_u32(&w,UINT32_MAX);
    n=frame(wire,payload,w.n); ssh_receive(&engine,wire,n); CHECK(engine.closed);
    reset_connection();
    w.n=0; ssh_put_byte(&w,94); ssh_put_u32(&w,0); ssh_put_text(&w,"unauthorized");
    n=frame(wire,payload,w.n); ssh_receive(&engine,wire,n); CHECK(engine.closed);
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
    CHECK(ssh_init(&engine,seed,private_key)); engine.out_len=0;
    const uint8_t ident[]="SSH-2.0-fixture\r\n";
    ssh_receive(&engine,ident,sizeof(ident)-1);
    uint8_t ignore[5]={2,0,0,0,0}; size_t n=frame(wire,ignore,sizeof(ignore));
    ssh_receive(&engine,wire,n); CHECK(engine.closed);
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
    primitives(); codecs(); framing(); channels(); authentication(); exchange_refusals();
    report("sshtest: %d checks, %d failures\n",checks,failed);
    return failed?1:0;
}
