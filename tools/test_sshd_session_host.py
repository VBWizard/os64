#!/usr/bin/env python3
"""Exercise the production daemon loop with controlled I/O and engine events.

The real loop and flush function are compiled unchanged. Engine stubs isolate
adapter scheduling: close before rekey completion, sparse shared credit and
interrupted writes, and PTY resize results. The separate SSH suite tests the
real transport engine. An optional source path can check an earlier daemon.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import sys

root = Path(__file__).resolve().parents[1]
source = Path(sys.argv[1]).read_text() if len(sys.argv)>1 else (root / 'userland/apps/sshd/sshd.c').read_text()

def function(name):
    m = re.search(r'static [\w ]+ ' + name + r'\([^)]*\)\s*\{', source)
    assert m, name
    end, depth = m.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[m.start():end]

body = function('session')
body = body[body.index('    uint8_t net[8192]'):body.index('    if (engine.error[0])')]
if 'static void resize_terminal(' in source:
    resize = function('resize_terminal')
else:
    event_body = function('event')
    start = event_body.index('case SSH_EVENT_RESIZE:') + len('case SSH_EVENT_RESIZE:')
    resize = 'static void resize_terminal(void) {' + event_body[start:event_body.index('break;', start)] + '}'

program = r'''
#include "ssh_engine.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "os64/syscall_numbers.h"
#include "os64/signal.h"
#include <stdbool.h>
#include "os64/str.h"
#include "os64/conf.h"
#define CHILD_RING_CAP 65536u
/* The configuration ladder, scripted: whether sshd.conf resolves, what the
 * read returns, and the port line it delivers. */
static int conf_found; static int64_t conf_rc; static const char *conf_value, *last_log;
bool os64_streq(const char *a,const char *b) {return !strcmp(a,b);}
int64_t os64_conf_find(const char *name,char *out,size_t cap) {
 assert(!strcmp(name,"sshd.conf") && cap>=OS64_CONF_PATH_MAX);
 if(!conf_found)return OS64_CONF_NO_FILE;
 strcpy(out,"/home/sshd.conf");return 0;
}
static const char *conf_forward;
int64_t os64_conf_read(const char *path,os64_conf_fn fn,void *user) {
 assert(!strcmp(path,"/home/sshd.conf"));
 if(conf_rc<0)return conf_rc;
 if(conf_value && !fn("port",conf_value,user))return 1;
 if(conf_forward && !fn("forward",conf_forward,user))return 1;
 return conf_value||conf_forward?1:0;
}
static void log_line(const char *text) {last_log=text;}
static ssh_engine engine;
static uint32_t input_head,credited;
static int64_t child=7,master;
static int resize_calls,resize_completed,resize_success;
static int64_t resize_return;
typedef struct {uint8_t bytes[CHILD_RING_CAP];uint32_t head,tail;int eof,error,fd;} child_stream;
static child_stream streams[2];
static unsigned scenario,reads,turn,period,phase,sends[2],disconnected;
static unsigned chunk=1,credits=40; /* window bytes per replenishment, and how many reads grant one */
static uint64_t clock_ms;
static int64_t write_result;
static uint8_t wire[4096];static size_t wire_len;
static uint64_t now_ms(void) {return clock_ms;}
static unsigned naps,yields,live_turns;
static void os64_sleep(unsigned ms) {
 if(ms)naps++;else yields++;
 assert(++turn<200);clock_ms+=scenario==1?1000:1;
}
/* Forwards, scripted: live for live_turns passes, moving bytes on each. */
uint32_t ssh_forwards_live(const ssh_engine *s) {(void)s;return live_turns?1:0;}
/* The real relay's collaborators, for forwardfair: a shared output budget
 * per pass stands in for the engine's output queue. */
static size_t fair_budget,fair_sent[SSH_FORWARDS];
/* mixedfair: the output room both kinds of channel draw from, refilled once
 * per pass, sized by the engine's rule (what fits, less framing: 65 for a
 * stdout or forward packet, 69 for stderr). */
static size_t room,room_refill;
size_t ssh_forward_send(ssh_engine *s,uint32_t f,const uint8_t *p,size_t n) {
 (void)s;(void)p;
 if(scenario==3) {if(room<=65)return 0;if(n>room-65)n=room-65;room-=n+65;fair_sent[f]+=n;return n;}
 if(n>fair_budget) {n=fair_budget;}
 fair_budget-=n;fair_sent[f]+=n;return n;
}
void ssh_forward_consumed(ssh_engine *s,uint32_t f,uint32_t n) {(void)s;(void)f;(void)n;}
int ssh_forward_finish(ssh_engine *s,uint32_t f) {(void)s;(void)f;return 1;}
static unsigned releases;
void ssh_forward_release(ssh_engine *s,uint32_t f) {(void)s;(void)f;releases++;}
static int64_t os64_close(int32_t h) {(void)h;return 0;}
static void os64_free(void *p) {(void)p;}
static int real_forwards_pass(void);
static int forwards_pass(void) {if(scenario==3)return real_forwards_pass();if(!live_turns)return 0;live_turns--;return 1;}
size_t ssh_output(ssh_engine *s,const uint8_t **p) {*p=s->output+s->out_head;return s->out_len;}
void ssh_output_consume(ssh_engine *s,size_t n) {assert(n<=s->out_len);s->out_head+=n;s->out_len-=n;}
/* A forward's local end for closedrain: takes local_accept bytes a write,
 * or fails when it is negative. */
static int64_t local_accept;static unsigned delivered,local_reads;
static int64_t os64_write_for(int fd,const void *p,size_t n,unsigned ms) {
 if(fd>=10) {
  assert(!ms);if(local_accept<0)return -1;
  size_t c=n<(size_t)local_accept?n:(size_t)local_accept;delivered+=(unsigned)c;
  return c?(int64_t)c:OS64_ERR_TIMEOUT;
 }
 assert(fd==1 && !ms);if(write_result<=0)return write_result;
 size_t count=n<(size_t)write_result?n:(size_t)write_result;
 assert(wire_len+count<=sizeof(wire));memcpy(wire+wire_len,p,count);wire_len+=count;return count;
}
static int64_t os64_read_for(int fd,void *p,size_t n,unsigned ms) {
 if(fd>=10) {assert(!ms);local_reads++;memset(p,fd,n);return (int64_t)n;} /* a forward's local end: an endless sender */
 assert(fd==0 && n && !ms);reads++;
 if(scenario<2) {
  if(reads==1 || (scenario==0 && reads<=3)) {*(uint8_t *)p=(uint8_t)reads;return 1;}
  return OS64_ERR_TIMEOUT;
 }
 if(reads>credits)return 0;
 if(scenario==3) {room=room_refill;return OS64_ERR_TIMEOUT;}
 if(reads%period==phase) engine.peer_window+=chunk;
 return OS64_ERR_TIMEOUT;
}
size_t ssh_receive(ssh_engine *s,const uint8_t *p,size_t n) {
 assert(n==1);s->event=SSH_EVENT_NONE;
 if(*p==1) {s->sent_close=1;s->event=SSH_EVENT_CLOSE;s->deferred_len=5;}
 if(*p==2)s->kex=2;
 if(*p==3) {
  s->kex=0;s->deferred_len=0;s->out_head=0;s->out_len=5;memcpy(s->output,"CLOSE",5);
 }
 return n;
}
static int64_t os64_pty_resize(int32_t fd,uint16_t cols,uint16_t rows) {
 assert(fd==master && cols==103 && rows==41);resize_calls++;return resize_return;
}
void ssh_resize_result(ssh_engine *s,int success) {
 assert(s==&engine);resize_completed++;resize_success=success;
}
static void event(void) {}
void ssh_disconnect(ssh_engine *s,uint32_t reason,const char *text) {
 (void)text;disconnected=reason;s->closed=1;
}
void ssh_rekey(ssh_engine *s) {(void)s;assert(0 && "no new rekey during close");}
void ssh_input_consumed(ssh_engine *s,uint32_t n) {(void)s;(void)n;}
size_t ssh_send_data(ssh_engine *s,const uint8_t *p,size_t n,int stream) {
 (void)p;if(s->kex || s->sent_close || s->closed)return 0;
 if(scenario==3) {size_t f=stream?69:65;if(room<=f)return 0;if(n>room-f)n=room-f;room-=n+f;sends[stream]+=(unsigned)n;return n;}
 if(n>s->peer_window)n=s->peer_window;
 s->peer_window-=(uint32_t)n;sends[stream]+=(unsigned)n;return n;
}
static int64_t os64_reap(int32_t *status) {(void)status;return 0;}
void ssh_send_exit(ssh_engine *s,uint32_t status) {(void)s;(void)status;assert(0);}
'''
program += resize + '\n' + function('flush') + '\nstatic void loop(void) {\n' + body + '\n}\n'
settings_type = source[source.index('typedef struct { uint32_t port;'):source.index('sshd_settings;') + len('sshd_settings;')]
link_type = source[source.index('typedef struct {\n    int32_t handle;'):source.index('forward_link;') + len('forward_link;')]
relay = function('forwards_pass').replace('forwards_pass(', 'real_forwards_pass(')
program += link_type + '\nstatic forward_link links[SSH_FORWARDS];\nstatic uint32_t next_forward;\n'
program += function('link_close') + '\n' + relay + '\n'
program += settings_type + '\n' + function('setting') + '\n' + function('configured') + '\n'
program += r'''
static void reset(void) {
 memset(&engine,0,sizeof(engine));memset(streams,0,sizeof(streams));
 engine.authenticated=engine.started=1;reads=turn=disconnected=0;naps=yields=live_turns=0;
 clock_ms=1;wire_len=0;write_result=2;sends[0]=sends[1]=0;
 master=3;resize_calls=resize_completed=resize_success=0;resize_return=0;
}
int main(int argc,char **argv) {
 assert(argc==2);
 if(!strcmp(argv[1],"flush")) {
  reset();memcpy(engine.output,"abcdef",6);engine.out_len=6;
  write_result=OS64_INTERRUPTED;assert(flush() && engine.out_len==6 && !engine.out_head);
  write_result=OS64_ERR_TIMEOUT;assert(flush() && engine.out_len==6);
  write_result=2;assert(flush() && engine.out_len==4 && engine.out_head==2);
  assert(flush() && flush() && !engine.out_len && wire_len==6 && !memcmp(wire,"abcdef",6));
  engine.out_len=1;write_result=0;assert(!flush());write_result=-99;assert(!flush());
 } else if(!strcmp(argv[1],"close")) {
  scenario=0;reset();engine.kex=1;loop();
  assert(reads==3 && !engine.kex && !engine.deferred_len && !disconnected);
  assert(wire_len==5 && !memcmp(wire,"CLOSE",5));
  scenario=1;reset();engine.kex=1;loop();
  assert(disconnected==3 && clock_ms>120000 && clock_ms<125000);
 } else if(!strcmp(argv[1],"fair")) {
  scenario=2;
  for(period=1;period<=4;period++) for(phase=0;phase<period;phase++) {
   reset();streams[0].tail=streams[1].tail=CHILD_RING_CAP;
   loop();assert(sends[0]>0 && sends[1]>0);
   assert(sends[0]<=sends[1]+1 && sends[1]<=sends[0]+1);
  }
  /* Credit just over one 4096-byte staging buffer: the stream that sends
   * the big block must not be first again on the next replenishment. */
  chunk=4097;credits=8;period=1;phase=0;
  reset();streams[0].tail=streams[1].tail=CHILD_RING_CAP;
  loop();assert(sends[0]+sends[1]==8*4097);
  assert(sends[0]<=sends[1]+4096 && sends[1]<=sends[0]+4096);
 } else if(!strcmp(argv[1],"resize")) {
  reset();engine.cols=engine.resize_cols=103;engine.rows=engine.resize_rows=41;
  resize_return=-1;resize_terminal();assert(resize_calls==1 && resize_completed==1 && !resize_success);
  resize_return=0;resize_terminal();assert(resize_calls==2 && resize_completed==2 && resize_success);
  reset();master=-1;engine.started=0;resize_terminal();
  assert(!resize_calls && resize_completed==1 && resize_success);
  engine.started=1;resize_terminal();assert(!resize_calls && resize_completed==2 && !resize_success);
 } else if(!strcmp(argv[1],"port")) {
  /* Only genuine absence permits the defaults. */
  sshd_settings c;
  conf_found=0;conf_rc=OS64_CONF_NO_FILE;conf_value=0;conf_forward=0;last_log=0;
  assert(configured(&c) && c.port==22 && c.forward==SSH_FORWARD_LOOPBACK && !last_log);
  conf_found=1;conf_rc=OS64_CONF_NO_FILE;
  assert(!configured(&c) && last_log && strstr(last_log,"unreadable"));
  conf_found=1;conf_rc=OS64_CONF_TRUNCATED;last_log=0;
  assert(!configured(&c) && last_log);
  conf_found=1;conf_rc=0;conf_value="2222";last_log=0;
  assert(configured(&c) && c.port==2222 && !last_log);
  conf_value=0;assert(configured(&c) && c.port==22);
  conf_value="70000";assert(!configured(&c));
  conf_value="22x";assert(!configured(&c));
  conf_value="0";assert(!configured(&c));
 } else if(!strcmp(argv[1],"forward")) {
  /* The forwarding key: two words, anything else refuses the file. */
  sshd_settings c;
  conf_found=1;conf_rc=0;conf_value=0;conf_forward="none";last_log=0;
  assert(configured(&c) && c.port==22 && c.forward==SSH_FORWARD_NONE && !last_log);
  conf_forward="loopback";assert(configured(&c) && c.forward==SSH_FORWARD_LOOPBACK);
  conf_value="2200";conf_forward="none";assert(configured(&c) && c.port==2200 && c.forward==SSH_FORWARD_NONE);
  conf_value=0;conf_forward="yes";last_log=0;assert(!configured(&c) && last_log);
  conf_forward="";assert(!configured(&c));
 } else if(!strcmp(argv[1],"forwardfair")) {
  /* Three forwards, each with more to send than the output can take: over
   * many passes of a budget smaller than one staging buffer, no forward
   * leads every time, so none gets more than one buffer ahead. */
  reset();
  for(uint32_t i=0;i<SSH_FORWARDS;i++) links[i].handle=-1;
  static uint8_t to[3][SSH_FORWARD_WINDOW],from[3][SSH_DATA_MAX];
  for(uint32_t i=0;i<3;i++) {links[i].handle=(int32_t)(10+i);links[i].to_local=to[i];links[i].from_local=from[i];}
  for(unsigned pass=0;pass<300;pass++) {fair_budget=40000;assert(real_forwards_pass());}
  for(uint32_t i=0;i<3;i++) for(uint32_t j=0;j<3;j++) assert(fair_sent[i]<=fair_sent[j]+SSH_DATA_MAX);
  assert(fair_sent[0]+fair_sent[1]+fair_sent[2]==300u*40000u);
 } else if(!strcmp(argv[1],"closedrain")) {
  /* The client's CLOSE crossed ours with bytes still queued for the local
   * end: nothing more is read from it, the queue is delivered first, and
   * only then is the slot released, once. A local end that fails ends the
   * wait at once. */
  reset();
  for(uint32_t i=0;i<SSH_FORWARDS;i++) links[i].handle=-1;
  static uint8_t to[SSH_FORWARD_WINDOW],from[SSH_DATA_MAX];
  links[0]=(forward_link){.handle=10,.to_local=to,.from_local=from,.to_len=1000,.peer_closed=1};
  local_accept=0;assert(!real_forwards_pass() && links[0].handle==10 && !releases && !local_reads);
  local_accept=300;
  assert(real_forwards_pass() && links[0].to_len==700 && links[0].handle==10 && !releases);
  while(links[0].handle>=0) assert(real_forwards_pass());
  assert(delivered==1000 && releases==1 && !local_reads);
  links[0]=(forward_link){.handle=11,.to_local=to,.from_local=from,.to_len=1000,.peer_closed=1};
  local_accept=-1;assert(real_forwards_pass() && links[0].handle<0 && releases==2 && !local_reads);
 } else if(!strcmp(argv[1],"mixedfair")) {
  /* A session and a forward that both always have more to send than a
   * pass's room, so whoever goes first every time takes the room. Taking
   * turns, neither gets more than twice the other, at a room that fits a
   * whole 4 KiB staging buffer and at one that fits neither kind's whole
   * staged packet. */
  static uint8_t to[SSH_FORWARD_WINDOW],from[SSH_DATA_MAX];
  size_t refills[]={5000,3000};
  for(unsigned k=0;k<2;k++) {
   scenario=3;credits=150;room_refill=refills[k];reset();fair_sent[0]=0;
   streams[0].tail=streams[1].tail=UINT32_MAX/2; /* never runs dry */
   for(uint32_t i=0;i<SSH_FORWARDS;i++) links[i].handle=-1;
   links[0]=(forward_link){.handle=10,.to_local=to,.from_local=from};
   loop();
   size_t session=(size_t)sends[0]+sends[1],forward=fair_sent[0];
   printf("mixedfair %zu: session %zu bytes, forward %zu bytes over %u passes\n",refills[k],session,forward,reads-1);fflush(stdout);
   assert(session && forward && session<=2*forward && forward<=2*session);
  }
 } else if(!strcmp(argv[1],"linger")) {
  /* A closed session with a live forward keeps the connection; the loop
   * yields instead of napping while the forward moves bytes, then ends
   * once the forward is gone and the output is drained. */
  scenario=0;reset();live_turns=20;
  loop();
  assert(!live_turns && yields>=20 && wire_len==5 && !disconnected);
 } else assert(0);
 printf("sshd session %s PASS\n",argv[1]);
}
'''
with tempfile.TemporaryDirectory(prefix='sshd-session-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-no-pie',
                    '-I'+str(root / 'userland/apps/sshd'), '-I'+str(root / 'abi/include'),
                    '-I'+str(root / 'userland/libos64/include'),
                    str(work / 'test.c'),
                    '-o', str(work / 'test')], check=True)
    results = [subprocess.run([str(work / 'test'), case]).returncode for case in ('flush','close','fair','resize','port','forward','forwardfair','closedrain','mixedfair','linger')]
    raise SystemExit(any(results))
