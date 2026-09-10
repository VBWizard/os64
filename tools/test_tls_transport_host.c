#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tls/transport.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "os64/proc.h"
#include "os64/signal.h"

struct os64_tls_client { int unused; } client;
static os64_tls_state_t engine;
static os64_ticks_t clock_value;
static bool bad_clock, fail_alloc, refuse_create, eof, block_write, block_read, clean_after_take;
static unsigned closed, freed, aborts, allocations, write_waits, read_waits;
static int64_t write_error, read_error;
static unsigned char offered[9000], sent[9000], received[9000], fed[9000];
static size_t offered_at, offered_end, sent_end, received_at, received_end, fed_end, write_cap, feed_cap;
static uint64_t create_ticks, last_read_ms, last_write_ms;
static unsigned read_calls, write_calls;
static bool interrupt_read_wait, interrupt_write_wait, freeze_wait;
static void elapsed(uint64_t ms) {
 if (!freeze_wait) clock_value.ticks += ms / 1000 * clock_value.per_second +
     ((ms % 1000) * clock_value.per_second + 999) / 1000;
}
static size_t allocation_size;
void *os64_malloc(size_t n) { if (fail_alloc) return NULL; allocations++; allocation_size=n; return calloc(1,n); }
void os64_free(void *p) {
 if (p) {
  assert(allocations==1);
  for(size_t i=0;i<allocation_size;i++) assert(!((unsigned char*)p)[i]);
  allocations--; free(p);
 }
}
void *os64_memset(void *p, int c, size_t n) { return memset(p,c,n); }
int64_t os64_ticks(os64_ticks_t *out) { *out=clock_value; return bad_clock ? -1 : 0; }
int64_t os64_close(int32_t h) { assert(h==7); closed++; return 0; }
int64_t os64_write_for(int32_t h,const void *p,size_t n,uint64_t ms) {
 assert(h==7);
 assert(n);
 assert(!closed && ms!=UINT64_MAX); write_calls++;
 if(ms) {
  write_waits++; last_write_ms=ms;
  if(interrupt_write_wait) { interrupt_write_wait=false; return OS64_INTERRUPTED; }
  elapsed(ms);
 }
 if(write_error) return write_error;
 if(block_write) return OS64_ERR_TIMEOUT;
 if(n>write_cap) n=write_cap;
 assert(sent_end+n<=sizeof sent); memcpy(sent+sent_end,p,n); sent_end+=n; return n;
}
int64_t os64_read_for(int32_t h,void *p,size_t n,uint64_t ms) {
 assert(h==7 && !closed && ms!=UINT64_MAX); read_calls++;
 if(ms) {
  read_waits++; last_read_ms=ms;
  if(interrupt_read_wait) { interrupt_read_wait=false; return OS64_INTERRUPTED; }
  elapsed(ms);
 }
 if(read_error) return read_error;
 if(eof) return 0;
 if(block_read || received_at==received_end) return OS64_ERR_TIMEOUT;
 if(n>received_end-received_at) n=received_end-received_at;
 memcpy(p,received+received_at,n); received_at+=n; return n;
}
os64_tls_status_t os64_tls_client_create(const os64_tls_config_t *cfg,os64_tls_client **out) {
 assert(cfg); *out=NULL; clock_value.ticks+=create_ticks;
 if(refuse_create) return OS64_TLS_ENTROPY_UNAVAILABLE;
 *out=&client; return OS64_TLS_OK;
}
void os64_tls_free(os64_tls_client *c) { if(c) { assert(c==&client); freed++; } }
os64_tls_state_t os64_tls_state(os64_tls_client *c) { assert(c==&client); return engine; }
os64_tls_status_t os64_tls_abort(os64_tls_client *c,os64_tls_status_t reason) {
 assert(c==&client); aborts++;
 if(engine.status==OS64_TLS_OK) engine.status=reason;
 engine.flags &= OS64_TLS_HANDSHAKE_DONE | OS64_TLS_CLOSING; return engine.status;
}
os64_tls_transfer_t os64_tls_take_ciphertext(os64_tls_client *c,void *p,size_t n) {
 assert(c==&client && (engine.flags&OS64_TLS_SEND_CIPHER));
 if(n>offered_end-offered_at) n=offered_end-offered_at;
 memcpy(p,offered+offered_at,n); offered_at+=n;
 if(offered_at==offered_end) { engine.flags&=~OS64_TLS_SEND_CIPHER; if(clean_after_take) engine.status=OS64_TLS_CLEAN_EOF; }
 return (os64_tls_transfer_t){engine.status,n};
}
os64_tls_transfer_t os64_tls_feed_ciphertext(os64_tls_client *c,const void *p,size_t n) {
 assert(c==&client && (engine.flags&OS64_TLS_RECV_CIPHER));
 if(n>feed_cap) n=feed_cap;
 assert(fed_end+n<=sizeof fed); memcpy(fed+fed_end,p,n); fed_end+=n;
 return (os64_tls_transfer_t){engine.status,n};
}
os64_tls_transfer_t os64_tls_write_plaintext(os64_tls_client *c,const void *p,size_t n) {
 assert(c==&client && (p||!n)); return (os64_tls_transfer_t){engine.status,n};
}
os64_tls_transfer_t os64_tls_read_plaintext(os64_tls_client *c,void *p,size_t n) {
 assert(c==&client && (p||!n));
 if(n && (engine.flags&OS64_TLS_RECV_PLAIN)) { *(char*)p='x'; engine.flags&=~OS64_TLS_RECV_PLAIN; return (os64_tls_transfer_t){engine.status,1}; }
 return (os64_tls_transfer_t){OS64_TLS_NEED_PROGRESS,0};
}
os64_tls_status_t os64_tls_flush(os64_tls_client *c) { assert(c==&client); return engine.status; }
os64_tls_status_t os64_tls_begin_close(os64_tls_client *c) {
 assert(c==&client); engine.flags|=OS64_TLS_CLOSING;
 if(!(engine.flags&OS64_TLS_HANDSHAKE_DONE)) return os64_tls_abort(c,OS64_TLS_CANCELLED);
 return engine.status;
}
os64_tls_status_t os64_tls_input_eof(os64_tls_client *c) {
 assert(c==&client); engine.status=OS64_TLS_TRUNCATED; return engine.status;
}
static void reset(void) {
 assert(!allocations);
 engine=(os64_tls_state_t){.status=OS64_TLS_OK,.flags=OS64_TLS_RECV_CIPHER};
 clock_value=(os64_ticks_t){100,100};
 bad_clock=fail_alloc=refuse_create=eof=block_write=block_read=clean_after_take=false;
 closed=freed=aborts=write_waits=read_waits=0;
 write_error=read_error=0; create_ticks=last_read_ms=last_write_ms=0;
 read_calls=write_calls=0; interrupt_read_wait=interrupt_write_wait=freeze_wait=false;
 offered_at=offered_end=sent_end=received_at=received_end=fed_end=0;
 write_cap=3; feed_cap=2;
 for(size_t i=0;i<sizeof offered;i++) offered[i]=received[i]=(unsigned char)(i*13);
}
static os64_tls_transport *create(void) {
 os64_tls_transport *t=NULL; os64_tls_config_t cfg={0};
 assert(os64_tls_transport_create(&cfg,7,NULL,&t)==OS64_TLS_OK && t); return t;
}
static void destroy(os64_tls_transport *t) { os64_tls_transport_free(t); assert(closed==1 && freed==1 && !allocations); }
static void authenticated(void) { engine.flags|=OS64_TLS_HANDSHAKE_DONE|OS64_TLS_SEND_PLAIN; }
int main(void) {
 os64_tls_config_t cfg={0}; os64_tls_transport *t=(void*)1;
 reset();
 assert(os64_tls_transport_create(&cfg,-1,NULL,&t)==OS64_TLS_BAD_ARGUMENT && !t && !closed);
 reset(); fail_alloc=true;
 assert(os64_tls_transport_create(&cfg,7,NULL,&t)==OS64_TLS_NO_MEMORY && !t && !closed);
 reset(); refuse_create=true;
 assert(os64_tls_transport_create(&cfg,7,NULL,&t)==OS64_TLS_ENTROPY_UNAVAILABLE && !t && !closed && !allocations);
 reset(); create_ticks=3000;
 assert(os64_tls_transport_create(&cfg,7,NULL,&t)==OS64_TLS_TIMEOUT && !t && !closed && freed==1);
 reset(); os64_tls_transport_limits_t limits={UINT64_MAX,2000};
 assert(os64_tls_transport_create(&cfg,7,&limits,&t)==OS64_TLS_BAD_ARGUMENT);
 limits.handshake_ms=0; assert(os64_tls_transport_create(&cfg,7,&limits,&t)==OS64_TLS_BAD_ARGUMENT);
 limits.handshake_ms=UINT64_MAX-1; clock_value.ticks=UINT64_MAX-100;
 assert(os64_tls_transport_create(&cfg,7,&limits,&t)==OS64_TLS_OK);
 assert(os64_tls_transport_step(t,0)==OS64_TLS_NEED_PROGRESS); destroy(t);

 reset(); offered_end=6000; received_end=97; engine.flags|=OS64_TLS_SEND_CIPHER; t=create();
 block_write=true;
 assert(os64_tls_transport_step(t,0)==OS64_TLS_OK && sent_end==0 && fed_end>0);
 block_write=false;
 for(unsigned i=0;i<10000 && (sent_end<offered_end || fed_end<received_end);i++)
  assert(os64_tls_transport_step(t,0)==OS64_TLS_OK);
 assert(sent_end==6000 && !memcmp(sent,offered,6000) && fed_end==97 && !memcmp(fed,received,97)); destroy(t);

 reset(); offered_end=5; engine.flags|=OS64_TLS_SEND_CIPHER; block_write=block_read=true; t=create();
 assert(os64_tls_transport_step(t,0)==OS64_TLS_OK); // Took ownership of pending output.
 assert(os64_tls_transport_step(t,1000)==OS64_TLS_NEED_PROGRESS);
 assert(os64_tls_transport_step(t,1000)==OS64_TLS_NEED_PROGRESS);
 assert(write_waits==1 && read_waits==1 && last_write_ms==10 && last_read_ms==10 && !closed); destroy(t);

 reset(); t=create();
 for(unsigned i=0;i<2999;i++) {
  received_at=0; received_end=1; fed_end=0; clock_value.ticks++;
  assert(os64_tls_transport_step(t,0)==OS64_TLS_OK);
 }
 clock_value.ticks++; assert(os64_tls_transport_step(t,0)==OS64_TLS_TIMEOUT && closed==1); destroy(t);

 reset(); offered_end=4; engine.flags|=OS64_TLS_SEND_CIPHER; block_write=true; t=create(); authenticated();
 assert(os64_tls_transport_step(t,0)==OS64_TLS_OK);
 assert(!(os64_tls_transport_state(t).flags&OS64_TLS_HANDSHAKE_DONE));
 assert(os64_tls_transport_write(t,"x",1).transferred==0);
 clock_value.ticks+=3000; assert(os64_tls_transport_step(t,0)==OS64_TLS_TIMEOUT); destroy(t);

 reset(); t=create(); authenticated(); engine.status=OS64_TLS_CLEAN_EOF;
 offered_end=7; engine.flags|=OS64_TLS_SEND_CIPHER; clock_value.ticks+=3000;
 assert(os64_tls_transport_state(t).status==OS64_TLS_TIMEOUT); destroy(t);

 reset(); authenticated(); t=create(); assert(os64_tls_transport_state(t).flags&OS64_TLS_HANDSHAKE_DONE);
 clock_value.ticks+=5000; assert(os64_tls_transport_step(t,10)==OS64_TLS_NEED_PROGRESS && !closed);
 engine.flags|=OS64_TLS_RECV_PLAIN;
 assert(os64_tls_transport_begin_close(t)==OS64_TLS_OK);
 clock_value.ticks+=100; assert(os64_tls_transport_begin_close(t)==OS64_TLS_OK);
 assert(os64_tls_transport_step(t,10)==OS64_TLS_NEED_PROGRESS);
 char byte; assert(os64_tls_transport_read(t,&byte,1).transferred==1 && byte=='x');
 clock_value.ticks+=100; assert(os64_tls_transport_state(t).status==OS64_TLS_TIMEOUT); destroy(t);

 reset(); authenticated(); t=create(); os64_tls_transport_state(t);
 offered_end=7; engine.flags|=OS64_TLS_SEND_CIPHER; clean_after_take=block_write=true;
 assert(os64_tls_transport_step(t,0)==OS64_TLS_OK && !closed);
 assert(os64_tls_transport_state(t).status==OS64_TLS_OK);
 block_write=false;
 while(!closed) os64_tls_transport_step(t,0);
 assert(sent_end==7 && os64_tls_transport_state(t).status==OS64_TLS_CLEAN_EOF); destroy(t);
 reset(); authenticated(); t=create(); os64_tls_transport_state(t);
 offered_end=7; engine.flags|=OS64_TLS_SEND_CIPHER; clean_after_take=block_write=true;
 os64_tls_transport_step(t,0); clock_value.ticks+=200;
 assert(os64_tls_transport_step(t,0)==OS64_TLS_TIMEOUT); destroy(t);
 reset(); authenticated(); t=create(); os64_tls_transport_state(t);
 offered_end=7; engine.flags|=OS64_TLS_SEND_CIPHER; clean_after_take=true; write_error=-1;
 assert(os64_tls_transport_step(t,0)==OS64_TLS_TRANSPORT); destroy(t);

 // A single direction gets the caller's patience, not a 10ms wake loop.
 reset(); authenticated(); t=create();
 assert(os64_tls_transport_step(t,5000)==OS64_TLS_NEED_PROGRESS);
 assert(read_waits==1 && last_read_ms==5000 && !write_waits && clock_value.ticks==600); destroy(t);
 reset(); authenticated(); t=create(); os64_tls_transport_state(t);
 engine.flags &= ~OS64_TLS_RECV_CIPHER; engine.flags |= OS64_TLS_SEND_CIPHER;
 offered_end=7; block_write=true;
 assert(os64_tls_transport_step(t,0)==OS64_TLS_OK);
 assert(os64_tls_transport_step(t,5000)==OS64_TLS_NEED_PROGRESS);
 assert(write_waits==1 && last_write_ms==5000 && !read_waits && !closed); destroy(t);

 // The protocol deadline still bounds a long one-direction wait, including
 // a multi-second tail, a fractional millisecond and a saturated duration.
 reset(); t=create(); clock_value.ticks+=2800;
 assert(os64_tls_transport_step(t,5000)==OS64_TLS_TIMEOUT && last_read_ms==2000); destroy(t);
 reset(); clock_value.per_second=333; t=create(); clock_value.ticks+=9989;
 assert(os64_tls_transport_step(t,5000)==OS64_TLS_TIMEOUT && last_read_ms==4); destroy(t);
 reset(); authenticated(); t=create(); os64_tls_transport_state(t);
 assert(os64_tls_transport_begin_close(t)==OS64_TLS_OK);
 assert(os64_tls_transport_step(t,5000)==OS64_TLS_TIMEOUT && last_read_ms==2000); destroy(t);
 reset(); limits=(os64_tls_transport_limits_t){UINT64_MAX-1,2000}; freeze_wait=true;
 assert(os64_tls_transport_create(&cfg,7,&limits,&t)==OS64_TLS_OK);
 assert(os64_tls_transport_step(t,UINT64_MAX-1)==OS64_TLS_NEED_PROGRESS && last_read_ms==UINT64_MAX-1); destroy(t);

 // Interrupt both poll and blocking paths. The step must return without
 // another I/O attempt, and a later step must resume the exact retained data.
 for(unsigned i=0;i<4;i++) {
  reset(); authenticated(); t=create(); os64_tls_transport_state(t);
  bool writing=(i&1)!=0, waiting=(i&2)!=0;
  if(writing) {
   engine.flags &= ~OS64_TLS_RECV_CIPHER; engine.flags |= OS64_TLS_SEND_CIPHER;
   offered_end=7;
   assert(os64_tls_transport_step(t,0)==OS64_TLS_OK && sent_end==3);
   if(waiting) { block_write=true; interrupt_write_wait=true; }
   else write_error=OS64_INTERRUPTED;
  } else {
   if(waiting) interrupt_read_wait=true;
   else read_error=OS64_INTERRUPTED;
  }
  unsigned calls=writing ? write_calls : read_calls;
  assert(os64_tls_transport_step(t,5000)==OS64_TLS_NEED_PROGRESS);
  assert((writing ? write_calls : read_calls)==calls+(waiting ? 2u : 1u));
  assert(!closed && !aborts && !freed && os64_tls_transport_state(t).status==OS64_TLS_OK);
  read_error=write_error=0; block_write=false;
  if(writing) {
   while(sent_end<offered_end) assert(os64_tls_transport_step(t,0)==OS64_TLS_OK);
   assert(sent_end==7 && !memcmp(sent,offered,7));
  } else {
   received_end=7;
   while(fed_end<received_end) assert(os64_tls_transport_step(t,0)==OS64_TLS_OK);
   assert(fed_end==7 && !memcmp(fed,received,7));
  }
  assert(os64_tls_transport_abort(t,OS64_TLS_CANCELLED)==OS64_TLS_CANCELLED && closed==1); destroy(t);
 }
 // A signal does not retire or restart the original handshake budget.
 reset(); t=create(); read_error=OS64_INTERRUPTED;
 assert(os64_tls_transport_step(t,5000)==OS64_TLS_NEED_PROGRESS && !closed);
 clock_value.ticks+=3000;
 assert(os64_tls_transport_step(t,5000)==OS64_TLS_TIMEOUT); destroy(t);

 for(unsigned i=0;i<5;i++) {
  reset(); t=create();
  if(i==0) read_error=-1;
  if(i==1) eof=true;
  if(i==2) bad_clock=true;
  if(i==3) clock_value.ticks--;
  if(i==4) clock_value.per_second++;
  os64_tls_status_t expected[]={OS64_TLS_TRANSPORT,OS64_TLS_TRUNCATED,OS64_TLS_BAD_TIME,OS64_TLS_BAD_TIME,OS64_TLS_BAD_TIME};
  assert(os64_tls_transport_step(t,10)==expected[i]);
  assert(os64_tls_transport_abort(t,OS64_TLS_CANCELLED)==expected[i]); destroy(t);
 }
 reset(); t=create(); engine.status=OS64_TLS_CERTIFICATE; engine.policy_reason=OS64_TLS_POLICY_SAN; engine.upstream_error=99;
 os64_tls_state_t state=os64_tls_transport_state(t);
 assert(state.status==OS64_TLS_CERTIFICATE && state.policy_reason==OS64_TLS_POLICY_SAN && state.upstream_error==99); destroy(t);
 puts("TLS transport seams PASS: ownership, short I/O, both directions, caller patience, interrupted/resumed I/O, deadlines, final flights, EOF, explicit cancellation and clock failure");
}
