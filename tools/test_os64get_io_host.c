// Exercise production URL I/O with deterministic transport and clock seams.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../userland/apps/os64get/url_io.h"
#include "os64/os64.h"

struct os64_tls_transport { unsigned flags; os64_tls_status_t status; bool closing; };
static struct os64_tls_transport transport;
static uint64_t now, waits[128];
static unsigned closes, frees, steps, writes, reads;
static bool cancel, fail_clock, create_fail, stall, interrupt_once, cancel_wait, drift, change_clock, rewind_clock;
static os64_tls_status_t read_status;
static size_t ready, sent;
static char captured[128];

bool install_cancelled(void) { return cancel; }
int64_t os64_ticks(os64_ticks_t *t)
{ *t=(os64_ticks_t){.ticks=now,.per_second=drift ? 2000 : 1000}; return fail_clock ? -1 : 0; }
int64_t os64_close(int32_t h) { assert(h==7); closes++; return 0; }
int32_t os64_hprintf(int32_t h,const char *f,...) { (void)h;(void)f;return 0; }
const char *os64_tls_status_name(os64_tls_status_t s) { (void)s;return "test"; }
static void wait_for(uint64_t ms)
{ assert(steps<128 && ms>0 && ms<=URL_IDLE_MS); waits[steps++]=ms; now+=1000; if(cancel_wait) cancel=true; if(change_clock) drift=true; if(rewind_clock) now=0; }
int64_t os64_write_for(int32_t h,const void *p,size_t n,uint64_t ms)
{
    assert(h==7); writes++;
    if(stall) { wait_for(ms); return OS64_ERR_TIMEOUT; }
    if(interrupt_once) { interrupt_once=false; wait_for(ms); return OS64_INTERRUPTED; }
    if(n>2)n=2;
    memcpy(captured+sent,p,n);sent+=n;now+=1000;return (int64_t)n;
}
int64_t os64_read_for(int32_t h,void *p,size_t n,uint64_t ms)
{
    assert(h==7);reads++;
    if(stall) { wait_for(ms); return OS64_ERR_TIMEOUT; }
    if(interrupt_once) { interrupt_once=false; wait_for(ms); return OS64_INTERRUPTED; }
    if(n>ready)n=ready;
    memset(p,'x',n);ready-=n;return (int64_t)n;
}
os64_tls_status_t os64_tls_transport_create(const os64_tls_config_t *c,int32_t h,
    const os64_tls_transport_limits_t *limits,os64_tls_transport **out)
{ assert(c && h==7 && !limits);*out=create_fail?NULL:&transport;return create_fail?OS64_TLS_CERTIFICATE:OS64_TLS_OK; }
os64_tls_state_t os64_tls_transport_state(os64_tls_transport *t)
{ return (os64_tls_state_t){.status=t->status,.flags=t->flags}; }
os64_tls_status_t os64_tls_transport_step(os64_tls_transport *t,uint64_t ms)
{
    wait_for(ms);
    if(t->closing) return t->status=OS64_TLS_CLEAN_EOF;
    if(!stall) {
        t->flags|=OS64_TLS_HANDSHAKE_DONE|OS64_TLS_SEND_PLAIN;
        t->flags&=~OS64_TLS_SEND_CIPHER;
    }
    return OS64_TLS_NEED_PROGRESS;
}
os64_tls_transfer_t os64_tls_transport_write(os64_tls_transport *t,const void *p,size_t n)
{
    writes++;
    if(stall) return (os64_tls_transfer_t){OS64_TLS_NEED_PROGRESS,0};
    if(n>2)n=2;
    memcpy(captured+sent,p,n);sent+=n;t->flags|=OS64_TLS_SEND_CIPHER;
    return (os64_tls_transfer_t){OS64_TLS_OK,n};
}
os64_tls_transfer_t os64_tls_transport_read(os64_tls_transport *t,void *p,size_t n)
{
    reads++;
    if(t->status!=OS64_TLS_OK) return (os64_tls_transfer_t){t->status,0};
    if(n>ready)n=ready;
    memset(p,'x',n);ready-=n;
    t->status=read_status;
    return (os64_tls_transfer_t){read_status,n};
}
os64_tls_status_t os64_tls_transport_flush(os64_tls_transport *t) {return t->status;}
os64_tls_status_t os64_tls_transport_begin_close(os64_tls_transport *t) {t->closing=true;return t->status;}
os64_tls_status_t os64_tls_transport_abort(os64_tls_transport *t,os64_tls_status_t s) {return t->status=s;}
void os64_tls_transport_free(os64_tls_transport *t) {assert(t==&transport);frees++;os64_close(7);}

static url_io_t open_io(bool tls)
{
    now=0;closes=frees=steps=writes=reads=0;sent=ready=0;
    cancel=fail_clock=create_fail=stall=interrupt_once=cancel_wait=drift=change_clock=rewind_clock=false;
    read_status=OS64_TLS_OK;
    transport=(struct os64_tls_transport){.flags=OS64_TLS_HANDSHAKE_DONE|OS64_TLS_SEND_PLAIN};
    url_io_t io;os64_tls_config_t c={0};assert(url_io_open(&io,7,tls?&c:NULL));return io;
}
int main(void)
{
    for(unsigned tls=0;tls<2;tls++) {
        url_io_t io=open_io(tls);
        interrupt_once=!tls;
        assert(url_io_write(&io,"abcdef",6));assert(sent==6 && !memcmp(captured,"abcdef",6));
        url_io_close(&io,true);url_io_close(&io,false);assert(closes==1 && frees==tls);

        io=open_io(tls);stall=true;
        assert(!url_io_write(&io,"abc",3));assert(io.silent && now==URL_IDLE_MS && steps==30);
        assert(waits[0]==30000 && waits[29]==1000);url_io_close(&io,false);assert(closes==1);

        io=open_io(tls);stall=true;char buf[8];
        assert(url_io_read(&io,buf,sizeof buf)<0);assert(io.silent && now==URL_IDLE_MS);
        assert(waits[0]==30000 && waits[29]==1000);url_io_close(&io,false);

        io=open_io(tls);stall=true;cancel_wait=true;
        assert(url_io_read(&io,buf,sizeof buf)<0);assert(io.error.status==OS64_TLS_CANCELLED && steps==1);
        url_io_close(&io,false);assert(closes==1);

        io=open_io(tls);stall=true;change_clock=true;
        assert(url_io_read(&io,buf,sizeof buf)<0);assert(io.error.status==OS64_TLS_BAD_TIME);
        url_io_close(&io,false);

        io=open_io(tls);now=1000;stall=true;rewind_clock=true;
        assert(url_io_read(&io,buf,sizeof buf)<0);assert(io.error.status==OS64_TLS_BAD_TIME);
        url_io_close(&io,false);

        io=open_io(tls);fail_clock=true;
        assert(!url_io_write(&io,"a",1));assert(io.error.status==OS64_TLS_BAD_TIME);url_io_close(&io,false);
    }
    url_io_t io=open_io(false);char buf[8];ready=3;interrupt_once=true;
    assert(url_io_read(&io,buf,sizeof buf)==3 && steps==1 && io.error.status==OS64_TLS_OK);
    url_io_close(&io,false);

    io=open_io(true);ready=3;read_status=OS64_TLS_PROTOCOL;
    assert(url_io_read(&io,buf,sizeof buf)==3);assert(url_io_read(&io,buf,sizeof buf)<0);
    assert(!url_io_complete(&io,true));url_io_close(&io,false);

    io=open_io(true);ready=3;read_status=OS64_TLS_TRUNCATED;
    assert(url_io_read(&io,buf,sizeof buf)==3);assert(url_io_complete(&io,true));
    assert(!url_io_complete(&io,false));url_io_close(&io,false);

    io=open_io(true);read_status=OS64_TLS_CLEAN_EOF;
    assert(url_io_read(&io,buf,sizeof buf)==0);assert(url_io_complete(&io,false));url_io_close(&io,true);

    io=open_io(true);transport.flags|=OS64_TLS_RECV_PLAIN;
    assert(!url_io_write(&io,"abc",3));assert(io.error.status==OS64_TLS_LIMIT);url_io_close(&io,false);

    io=open_io(false);url_io_close(&io,false);closes=0;create_fail=true;os64_tls_config_t c={0};
    assert(!url_io_open(&io,7,&c));url_io_close(&io,false);assert(closes==1 && frees==0);
    puts("PASS URL I/O budgets, prefixes, interruption, cancellation, closure and ownership");
}
