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
    m = re.search(r'static (?:int|void) ' + name + r'\(void\)\s*\{', source)
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
#define CHILD_RING_CAP 65536u
static ssh_engine engine;
static uint32_t input_head,credited;
static int64_t child=7,master;
static int resize_calls,resize_completed,resize_success;
static int64_t resize_return;
typedef struct {uint8_t bytes[CHILD_RING_CAP];uint32_t head,tail;int eof,error,fd;} child_stream;
static child_stream streams[2];
static unsigned scenario,reads,turn,period,phase,sends[2],disconnected;
static uint64_t clock_ms;
static int64_t write_result;
static uint8_t wire[4096];static size_t wire_len;
static uint64_t now_ms(void) {return clock_ms;}
static void os64_sleep(unsigned ms) {
 (void)ms;assert(++turn<200);clock_ms+=scenario==1?1000:1;
}
size_t ssh_output(ssh_engine *s,const uint8_t **p) {*p=s->output+s->out_head;return s->out_len;}
void ssh_output_consume(ssh_engine *s,size_t n) {assert(n<=s->out_len);s->out_head+=n;s->out_len-=n;}
static int64_t os64_write_for(int fd,const void *p,size_t n,unsigned ms) {
 assert(fd==1 && !ms);if(write_result<=0)return write_result;
 size_t count=n<(size_t)write_result?n:(size_t)write_result;
 assert(wire_len+count<=sizeof(wire));memcpy(wire+wire_len,p,count);wire_len+=count;return count;
}
static int64_t os64_read_for(int fd,void *p,size_t n,unsigned ms) {
 assert(fd==0 && n && !ms);reads++;
 if(scenario<2) {
  if(reads==1 || (scenario==0 && reads<=3)) {*(uint8_t *)p=(uint8_t)reads;return 1;}
  return OS64_ERR_TIMEOUT;
 }
 if(reads>40)return 0;
 if(reads%period==phase) engine.peer_window+=1;
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
 if(n>s->peer_window)n=s->peer_window;
 s->peer_window-=(uint32_t)n;sends[stream]+=(unsigned)n;return n;
}
static int64_t os64_reap(int32_t *status) {(void)status;return 0;}
void ssh_send_exit(ssh_engine *s,uint32_t status) {(void)s;(void)status;assert(0);}
'''
program += resize + '\n' + function('flush') + '\nstatic void loop(void) {\n' + body + '\n}\n'
program += r'''
static void reset(void) {
 memset(&engine,0,sizeof(engine));memset(streams,0,sizeof(streams));
 engine.authenticated=engine.started=1;reads=turn=disconnected=0;
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
 } else if(!strcmp(argv[1],"resize")) {
  reset();engine.cols=engine.resize_cols=103;engine.rows=engine.resize_rows=41;
  resize_return=-1;resize_terminal();assert(resize_calls==1 && resize_completed==1 && !resize_success);
  resize_return=0;resize_terminal();assert(resize_calls==2 && resize_completed==2 && resize_success);
  reset();master=-1;engine.started=0;resize_terminal();
  assert(!resize_calls && resize_completed==1 && resize_success);
  engine.started=1;resize_terminal();assert(!resize_calls && resize_completed==2 && !resize_success);
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
                    str(work / 'test.c'),
                    '-o', str(work / 'test')], check=True)
    results = [subprocess.run([str(work / 'test'), case]).returncode for case in ('flush','close','fair','resize')]
    raise SystemExit(any(results))
