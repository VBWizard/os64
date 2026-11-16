// Exercise production URL I/O with deterministic transport and clock seams.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "fetch/transport.h"
#include "os64/os64.h"

struct os64_tls_transport { unsigned flags; os64_tls_status_t status; bool closing; };
static struct os64_tls_transport transport;
static uint64_t now, waits[4096];
static unsigned closes, frees, steps, writes, reads;
static bool cancel, fail_clock, create_fail, stall, interrupt_once, cancel_wait, drift, change_clock, rewind_clock;
static os64_tls_status_t read_status;
static size_t ready, sent, stall_after;
static unsigned drain_steps;
static bool cancel_poll;
static uint64_t last_accept;
static char captured[128];

static bool cancelled_p(void *ctx) { (void)ctx; return cancel; }
int64_t os64_ticks(os64_ticks_t *t)
{ *t=(os64_ticks_t){.ticks=now,.per_second=drift ? 2000 : 1000}; return fail_clock ? -1 : 0; }
int64_t os64_close(int32_t h) { assert(h==7); closes++; return 0; }
int32_t os64_hprintf(int32_t h,const char *f,...) { (void)h;(void)f;return 0; }
const char *os64_tls_status_name(os64_tls_status_t s) { (void)s;return "test"; }
static void wait_for(uint64_t ms)
{ assert(steps<4096 && ms>0 && ms<=FETCH_IDLE_MS_DEFAULT); waits[steps++]=ms; now+=ms < 1000 ? ms : 1000; if(cancel_wait) cancel=true; if(change_clock) drift=true; if(rewind_clock) now=0; }
int64_t os64_write_for(int32_t h,const void *p,size_t n,uint64_t ms)
{
    assert(h==7); writes++;
    if(stall) { wait_for(ms); return OS64_ERR_TIMEOUT; }
    if(interrupt_once) { interrupt_once=false; wait_for(ms); return OS64_INTERRUPTED; }
    if(n>2)n=2;
    memcpy(captured+sent,p,n);sent+=n;now+=1000;last_accept=now;
    if(stall_after && sent>=stall_after) stall=true;
    return (int64_t)n;
}
int64_t os64_read_for(int32_t h,void *p,size_t n,uint64_t ms)
{
    assert(h==7);reads++;
    if(!ms && cancel_poll) { cancel=true; return OS64_INTERRUPTED; }
    if(!ms && !ready) return OS64_ERR_TIMEOUT;
    if(stall) { wait_for(ms); return OS64_ERR_TIMEOUT; }
    if(interrupt_once) { interrupt_once=false; wait_for(ms); return OS64_INTERRUPTED; }
    if(n>ready)n=ready;
    memset(p,'x',n);ready-=n;return (int64_t)n;
}
os64_tls_status_t os64_tls_transport_create(const os64_tls_config_t *c,int32_t h,
    const os64_tls_transport_limits_t *limits,os64_tls_transport **out)
{ assert(c && h==7 && limits && limits->handshake_ms==FETCH_IDLE_MS_DEFAULT && limits->shutdown_ms==OS64_TLS_TRANSPORT_SHUTDOWN_MS);*out=create_fail?NULL:&transport;return create_fail?OS64_TLS_CERTIFICATE:OS64_TLS_OK; }
os64_tls_state_t os64_tls_transport_state(os64_tls_transport *t)
{ return (os64_tls_state_t){.status=t->status,.flags=t->flags,.policy_reason=6,.upstream_error=54,.alpn="http/1.1"}; }
os64_tls_status_t os64_tls_transport_step(os64_tls_transport *t,uint64_t ms)
{
    wait_for(ms);
    if(t->closing) return t->status=OS64_TLS_CLEAN_EOF;
    if(!stall) {
        t->flags|=OS64_TLS_HANDSHAKE_DONE|OS64_TLS_SEND_PLAIN;
        if(drain_steps) drain_steps--;
        if(!drain_steps) t->flags&=~OS64_TLS_SEND_CIPHER;
        return OS64_TLS_OK;
    }
    return OS64_TLS_NEED_PROGRESS;
}
os64_tls_transfer_t os64_tls_transport_write(os64_tls_transport *t,const void *p,size_t n)
{
    writes++;
    if(stall || (t->flags & OS64_TLS_SEND_CIPHER)) return (os64_tls_transfer_t){OS64_TLS_NEED_PROGRESS,0};
    if(n>2)n=2;
    memcpy(captured+sent,p,n);sent+=n;t->flags|=OS64_TLS_SEND_CIPHER;last_accept=now;
    if(stall_after && sent>=stall_after) stall=true;
    return (os64_tls_transfer_t){OS64_TLS_OK,n};
}
os64_tls_transfer_t os64_tls_transport_read(os64_tls_transport *t,void *p,size_t n)
{
    reads++;
    if(t->status!=OS64_TLS_OK) return (os64_tls_transfer_t){t->status,0};
    if(n>ready)n=ready;
    memset(p,'x',n);ready-=n;
    if(!ready) t->flags &= ~OS64_TLS_RECV_PLAIN;
    t->status=read_status;
    return (os64_tls_transfer_t){read_status,n};
}
os64_tls_status_t os64_tls_transport_flush(os64_tls_transport *t) {return t->status;}
os64_tls_status_t os64_tls_transport_begin_close(os64_tls_transport *t) {t->closing=true;return t->status;}
os64_tls_status_t os64_tls_transport_abort(os64_tls_transport *t,os64_tls_status_t s) {return t->status=s;}
void os64_tls_transport_free(os64_tls_transport *t) {assert(t==&transport);frees++;os64_close(7);}
// The release verb: NEED_PROGRESS while the wrapper owes ciphertext (which a
// step sends, one per step), then OK with the engine, the socket and three
// unread bytes; or `release_refuses`, at once.
static unsigned release_owes, releases;
static os64_tls_status_t release_refuses;
static os64_tls_client *const kEngine = (os64_tls_client *)"engine";
os64_tls_status_t os64_tls_transport_release(os64_tls_transport *t, os64_tls_client **client, int32_t *handle,
                                             void *unread, size_t room, size_t *unread_len)
{
    assert(t == &transport && room >= OS64_TLS_TRANSPORT_UNREAD_MAX);
    releases++;
    if (release_refuses != OS64_TLS_OK) return release_refuses;
    if (release_owes) { if (!stall) release_owes--; return OS64_TLS_NEED_PROGRESS; }
    *client = kEngine; *handle = 7; memcpy(unread, "abc", 3); *unread_len = 3;
    return OS64_TLS_OK;
}

static fetch_transport_t open_io(bool tls)
{
    now=last_accept=0;closes=frees=steps=writes=reads=drain_steps=0;sent=ready=stall_after=0;cancel_poll=false;
    cancel=fail_clock=create_fail=stall=interrupt_once=cancel_wait=drift=change_clock=rewind_clock=false;
    read_status=OS64_TLS_OK;
    transport=(struct os64_tls_transport){.flags=OS64_TLS_HANDSHAKE_DONE|OS64_TLS_SEND_PLAIN};
    fetch_transport_t io;os64_tls_config_t c={0};assert(fetch_transport_open(&io,7,tls?&c:NULL,0,cancelled_p,NULL));return io;
}
static bool write_all(fetch_transport_t *io, const void *p, size_t n)
{
    size_t accepted = 0;
    return fetch_transport_write(io, p, n, &accepted) == FETCH_WRITE_DONE;
}
static void upload_idle(void)
{
    char body[80]; memset(body,'u',sizeof(body));
    for(unsigned tls=0;tls<2;tls++) {
        // Forty seconds of small successful writes is not thirty idle seconds.
        fetch_transport_t io=open_io(tls);
        assert(write_all(&io,body,sizeof(body)));
        assert(now>FETCH_IDLE_MS_DEFAULT && sent==sizeof(body));
        assert(!memcmp(captured,body,sizeof(body)) && !io.silent);
        fetch_transport_close(&io,false);

        // Positive progress followed by a stall gets one idle budget after
        // the last accepted bytes, not a fresh budget on each empty retry.
        io=open_io(tls);stall_after=10;
        assert(!write_all(&io,body,sizeof(body)));
        assert(sent==10 && io.silent && now-last_accept==FETCH_IDLE_MS_DEFAULT);
        fetch_transport_close(&io,false);
    }
    // All plaintext is accepted, but ciphertext drains for forty seconds.
    fetch_transport_t io=open_io(true);drain_steps=40;
    assert(write_all(&io,body,2));
    assert(sent==2 && steps==40 && now==40000 && !io.silent);
    fetch_transport_close(&io,false);

    // Resetting the idle origin must not mask clock corruption during a
    // successful TLS progress step.
    io=open_io(true);change_clock=true;
    assert(!write_all(&io,body,2) && io.error.status==OS64_TLS_BAD_TIME);
    fetch_transport_close(&io,false);
    io=open_io(true);now=5000;rewind_clock=true;
    assert(!write_all(&io,body,2) && io.error.status==OS64_TLS_BAD_TIME);
    fetch_transport_close(&io,false);
    puts("PASS progressing uploads, stalled uploads, TLS drain and clock guards");
}
static void interrupted_poll(void)
{
    fetch_transport_t io=open_io(false);cancel_poll=true;
    size_t accepted=0;
    assert(fetch_transport_write(&io,"secret",6,&accepted)==FETCH_WRITE_ERROR);
    assert(io.error.status==OS64_TLS_CANCELLED && accepted==0 && sent==0 && writes==0);
    fetch_transport_close(&io,false);
    puts("PASS interrupted response poll cancels before a write");
}
int main(int argc, char **argv)

{
    if(argc==2) {
        if(!strcmp(argv[1],"upload-idle")) upload_idle();
        else if(!strcmp(argv[1],"poll-cancel")) interrupted_poll();
        else return 2;
        return 0;
    }
    upload_idle();
    interrupted_poll();
    fetch_transport_t failed = open_io(true);
    transport.status = OS64_TLS_CERTIFICATE;
    assert(!fetch_transport_complete(&failed, true));
    fetch_transport_close(&failed, false);
    assert(failed.error.status == OS64_TLS_CERTIFICATE);
    assert(failed.error.policy_reason == 6 && failed.error.upstream_error == 54);
    assert(fetch_transport_tls_failed(&failed));
    for(unsigned tls=0;tls<2;tls++) {
        fetch_transport_t io=open_io(tls);
        interrupt_once=!tls;
        assert(write_all(&io,"abcdef",6));assert(sent==6 && !memcmp(captured,"abcdef",6));
        fetch_transport_close(&io,true);fetch_transport_close(&io,false);assert(closes==1 && frees==tls);

        io=open_io(tls);stall=true;
        assert(!write_all(&io,"abc",3));assert(io.silent && now==FETCH_IDLE_MS_DEFAULT && steps==(tls ? 30u : 3000u));
        assert(waits[0]==(tls ? 30000u : 10u) && waits[steps-1]==(tls ? 1000u : 10u));fetch_transport_close(&io,false);assert(closes==1);

        io=open_io(tls);stall=true;char buf[8];
        assert(fetch_transport_read(&io,buf,sizeof buf)<0);assert(io.silent && now==FETCH_IDLE_MS_DEFAULT);
        assert(waits[0]==30000 && waits[29]==1000);fetch_transport_close(&io,false);

        io=open_io(tls);stall=true;cancel_wait=true;
        assert(fetch_transport_read(&io,buf,sizeof buf)<0);assert(io.error.status==OS64_TLS_CANCELLED && steps==1);
        fetch_transport_close(&io,false);assert(closes==1);

        io=open_io(tls);stall=true;change_clock=true;
        assert(fetch_transport_read(&io,buf,sizeof buf)<0);assert(io.error.status==OS64_TLS_BAD_TIME);
        fetch_transport_close(&io,false);

        io=open_io(tls);now=1000;stall=true;rewind_clock=true;
        assert(fetch_transport_read(&io,buf,sizeof buf)<0);assert(io.error.status==OS64_TLS_BAD_TIME);
        fetch_transport_close(&io,false);

        io=open_io(tls);fail_clock=true;
        assert(!write_all(&io,"a",1));assert(io.error.status==OS64_TLS_BAD_TIME);fetch_transport_close(&io,false);
    }
    fetch_transport_t io=open_io(false);char buf[8];ready=3;interrupt_once=true;
    assert(fetch_transport_read(&io,buf,sizeof buf)==3 && steps==1 && io.error.status==OS64_TLS_OK);
    fetch_transport_close(&io,false);

    io=open_io(true);ready=3;read_status=OS64_TLS_PROTOCOL;
    assert(fetch_transport_read(&io,buf,sizeof buf)==3);assert(fetch_transport_read(&io,buf,sizeof buf)<0);
    assert(!fetch_transport_complete(&io,true));fetch_transport_close(&io,false);

    io=open_io(true);ready=3;read_status=OS64_TLS_TRUNCATED;
    assert(fetch_transport_read(&io,buf,sizeof buf)==3);assert(fetch_transport_complete(&io,true));
    assert(!fetch_transport_complete(&io,false));fetch_transport_close(&io,false);

    io=open_io(true);read_status=OS64_TLS_CLEAN_EOF;
    assert(fetch_transport_read(&io,buf,sizeof buf)==0);assert(fetch_transport_complete(&io,false));fetch_transport_close(&io,true);

    for (unsigned tls = 0; tls < 2; tls++) {
        io=open_io(tls);ready=3;transport.flags|=OS64_TLS_RECV_PLAIN;
        size_t accepted=0;
        assert(fetch_transport_write(&io,"abc",3,&accepted)==FETCH_WRITE_RESPONSE);
        assert(accepted==0 && io.error.status==OS64_TLS_OK);
        size_t got=0;
        while(got<3) { int64_t n=fetch_transport_read(&io,buf+got,3-got); assert(n>0); got+=(size_t)n; }
        assert(!memcmp(buf,"xxx",3));
        assert(fetch_transport_write(&io,"abc",3,&accepted)==FETCH_WRITE_DONE);
        assert(accepted==3 && sent==3 && !memcmp(captured,"abc",3));
        fetch_transport_close(&io,false);
    }

    // THE HANDSHAKE'S ALPN ANSWER, and none for a plain connection.
    io=open_io(true);assert(!strcmp(fetch_transport_alpn(&io),"http/1.1"));fetch_transport_close(&io,false);
    io=open_io(false);assert(fetch_transport_alpn(&io)==NULL);fetch_transport_close(&io,false);

    // RELEASE: the engine and the socket leave the transport, which then
    // holds nothing: closing it closes nothing and frees nothing.
    os64_tls_client *engine=NULL;int32_t socket=-1;size_t unread_len=0;
    unsigned char unread[OS64_TLS_TRANSPORT_UNREAD_MAX];
    io=open_io(true);releases=release_owes=0;release_refuses=OS64_TLS_OK;
    assert(fetch_transport_release(&io,&engine,&socket,unread,sizeof unread,&unread_len));
    assert(engine==kEngine && socket==7 && unread_len==3 && !memcmp(unread,"abc",3) && releases==1 && steps==0);
    assert(io.tls==NULL && io.handle==-1);
    fetch_transport_close(&io,false);assert(frees==0 && closes==0);
    // Ciphertext still owed is stepped out first, each step a wait.
    io=open_io(true);releases=0;release_owes=2;
    assert(fetch_transport_release(&io,&engine,&socket,unread,sizeof unread,&unread_len));
    assert(releases==3 && steps==2 && io.tls==NULL);
    fetch_transport_close(&io,false);assert(frees==0);
    // Refused (the peer closed, a failure): the transport is closed as after
    // any failure, and its error says why.
    io=open_io(true);releases=release_owes=0;release_refuses=OS64_TLS_PROTOCOL;
    assert(!fetch_transport_release(&io,&engine,&socket,unread,sizeof unread,&unread_len));
    assert(io.error.status==OS64_TLS_PROTOCOL && frees==1 && closes==1 && io.tls==NULL);
    release_refuses=OS64_TLS_OK;
    // Owed and never sent: the idle limit ends it, silent.
    io=open_io(true);release_owes=1;stall=true;
    assert(!fetch_transport_release(&io,&engine,&socket,unread,sizeof unread,&unread_len));
    assert(io.error.status==OS64_TLS_TIMEOUT && io.silent && frees==1 && now>=FETCH_IDLE_MS_DEFAULT);
    // Cancelled while it waits.
    io=open_io(true);release_owes=1;stall=true;cancel_wait=true;
    assert(!fetch_transport_release(&io,&engine,&socket,unread,sizeof unread,&unread_len));
    assert(io.error.status==OS64_TLS_CANCELLED && frees==1);
    // A plain connection has no engine to hand over.
    io=open_io(false);
    assert(!fetch_transport_release(&io,&engine,&socket,unread,sizeof unread,&unread_len));
    assert(closes==1 && io.error.status==OS64_TLS_TRANSPORT);
    release_owes=0;

    io=open_io(false);fetch_transport_close(&io,false);closes=0;create_fail=true;os64_tls_config_t c={0};
    assert(!fetch_transport_open(&io,7,&c,0,cancelled_p,NULL));fetch_transport_close(&io,false);assert(closes==1 && frees==0);
    puts("PASS URL I/O budgets, prefixes, interruption, cancellation, closure, ownership and release");
}
