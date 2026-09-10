#!/usr/bin/env python3
"""Exercise write's actual syscall prelude, console/TCP cases and public stubs."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/syscall.c').read_text()
def function(text, name):
    match = re.search(r'(?:static )?(?:uint64_t|int64_t) ' + name + r'\([^;]+?\)\s*\{', text)
    assert match, name
    at = match.end(); depth = 1
    while depth:
        depth += (text[at] == '{') - (text[at] == '}'); at += 1
    return text[match.start():at]
assert 'SYSCALL_WRITE_FOR' not in source
assert re.search(r'SYSCALL_DEFINE\(SYSCALL_WRITE,.*false, 0x02\)', source)
body = function(source, 'syscall_write')
# Keep the real dispatch prelude and cases under test; unrelated file/pipe
# implementations have their own harnesses and require different OS seams.
prelude = body[:body.index('switch (h->type)')]
console = body[body.index('case HANDLE_CONSOLE_OUT:'):body.index('case HANDLE_PTY_MASTER:')]
tcp = body[body.index('case HANDLE_NET_TCP:'):body.index('case HANDLE_NET_ICMP:')]
body = prelude + 'switch (h->type) {\n' + console + tcp + 'default: return SYSCALL_RESULT_INVALID;\n}}'
header = '''#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include "os64/syscall_numbers.h"
#define MS_PER_TICK 10
#define HANDLE_NET_TCP 7
#define HANDLE_CONSOLE_OUT 1
#define HANDLE_CONSOLE_ERR 2
'''
header += re.search(r'^#define OS64_INTERRUPTED .*', (root / 'abi/include/os64/signal.h').read_text(), re.M).group(0) + '\n'
for text, pattern in ((source, r'^#define (?:SYSCALL_RESULT_\w+|READ_CHUNK_SIZE|WRITE_CHUNK_SIZE) .*'),
                      ((root / 'kernel/include/driver/net/tcp.h').read_text(), r'^#define TCP_ERR_\w+ .*')):
    header += '\n'.join(re.findall(pattern, text, re.M)) + '\n'
header += '''
typedef struct { int unused; } task_t;
typedef struct { int unused; } tcp_conn_t;
typedef struct { int unused; } tty_t;
typedef struct { int type; void *object; } handle_t;
typedef struct { task_t *task; } core_local_storage_t;
static task_t task; static tcp_conn_t connection; static tty_t tty;
static handle_t handle = {HANDLE_NET_TCP, &connection};
static core_local_storage_t cls = {&task};
static uint64_t kTicksSinceStart, captured_deadline, captured_args[5];
static bool refuse_alloc, copy_ok=true, caught=true;
static size_t allocated, copied, requested, writes, console_bytes;
static unsigned fail_write, fail_copy, fail_alloc;
static long outcome;
static jmp_buf fatal;
static core_local_storage_t *get_core_local_storage(void) { return &cls; }
static handle_t *handle_get(task_t *t, int h) { assert(t==&task); return h==3 ? &handle : NULL; }
static tty_t *task_tty(task_t *t) { assert(t==&task); return &tty; }
static void tty_write(tty_t *t, const void *p, size_t n) { assert(t==&tty && p); console_bytes+=n; }
static void *kmalloc(size_t n) { if(refuse_alloc || (fail_alloc && writes>=fail_alloc)) return NULL; allocated++; return malloc(n); }
static void kfree(void *p) { assert(allocated); allocated--; free(p); }
static bool copy_user_buffer(const void *p, void *out, size_t n) {
 if (!copy_ok || !p || (fail_copy && writes>=fail_copy)) return false;
 copied=n; memcpy(out,p,n); return true;
}
static long tcp_conn_write(tcp_conn_t *c, const void *p, size_t n, uint64_t end) {
 assert(c==&connection && p && n==copied); requested=n;
 if(writes++) assert(captured_deadline==end); else captured_deadline=end;
 kTicksSinceStart+=5;
 return writes==fail_write ? outcome : (long)n;
}
static bool current_thread_will_catch(void) { return caught; }
static void raise_terminating_signal_and_die(task_t *t, void *p) { assert(t==&task && !p && !allocated); longjmp(fatal,1); }
static uint64_t os64_syscall4(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
 captured_args[0]=n; captured_args[1]=a; captured_args[2]=b; captured_args[3]=c; captured_args[4]=d; return 23;
}
'''
program = header + body + '\n'
for name in ('os64_write', 'os64_write_for'):
    program += function((root / 'userland/libos64/io.c').read_text(), name) + '\n'
program += '''int main(void) {
 unsigned char *data=calloc(1,2*READ_CHUNK_SIZE+1); assert(data);
 assert(os64_write(-2,data,99)==23 && captured_args[0]==3 && captured_args[4]==OS64_WAIT_FOREVER);
 assert(os64_write_for(-2,data,99,1234)==23);
 assert(captured_args[0]==3 && captured_args[1]==(uint64_t)(int64_t)-2 && captured_args[2]==(uintptr_t)data && captured_args[3]==99 && captured_args[4]==1234);
 assert(syscall_write(2,(uintptr_t)data,1,0,0,0)==SYSCALL_RESULT_INVALID);
 handle.type=HANDLE_CONSOLE_OUT;
 assert(syscall_write(3,(uintptr_t)data,1,0,0,0)==SYSCALL_RESULT_INVALID && !console_bytes);
 assert(syscall_write(3,(uintptr_t)data,1,OS64_WAIT_FOREVER,0,0)==1 && console_bytes==1);
 handle.type=HANDLE_NET_TCP;
 assert(syscall_write(3,(uintptr_t)data,0,0,0,0)==0 && !writes);
 assert(syscall_write(3,0,1,0,0,0)==SYSCALL_RESULT_BAD_USER_DATA && !allocated);
 refuse_alloc=true; assert(syscall_write(3,(uintptr_t)data,1,0,0,0)==SYSCALL_RESULT_INVALID); refuse_alloc=false;
 uint64_t ms[]={0,1,10,11,UINT64_MAX-1,OS64_WAIT_FOREVER};
 for(size_t i=0;i<sizeof ms/sizeof *ms;i++) {
  kTicksSinceStart=100; writes=0;
  assert(syscall_write(3,(uintptr_t)data,2*READ_CHUNK_SIZE+1,ms[i],0,0)==2*READ_CHUNK_SIZE+1 && !allocated && writes==3 && requested==1);
  assert(captured_deadline==(ms[i]==OS64_WAIT_FOREVER ? 0 : 100+ms[i]/10+(ms[i]%10!=0)));
 }
 long errors[]={TCP_ERR_TIMEOUT,TCP_ERR_INTERRUPTED,TCP_ERR_RESET};
 int64_t expected[]={OS64_ERR_TIMEOUT,OS64_INTERRUPTED,(int64_t)SYSCALL_RESULT_INVALID};
 for(unsigned i=0;i<3;i++) {
  writes=0; fail_write=1; outcome=errors[i];
  assert((int64_t)syscall_write(3,(uintptr_t)data,1,10,0,0)==expected[i] && !allocated);
  writes=0; fail_write=2;
  assert(syscall_write(3,(uintptr_t)data,READ_CHUNK_SIZE+1,10,0,0)==READ_CHUNK_SIZE && !allocated);
 }
 writes=0; fail_write=1; outcome=7;
 assert(syscall_write(3,(uintptr_t)data,99,10,0,0)==7 && writes==1);
 writes=0; fail_write=0; fail_copy=1;
 assert(syscall_write(3,(uintptr_t)data,READ_CHUNK_SIZE+1,10,0,0)==READ_CHUNK_SIZE && !allocated);
 writes=0; fail_copy=0; fail_alloc=1;
 assert(syscall_write(3,(uintptr_t)data,READ_CHUNK_SIZE+1,10,0,0)==READ_CHUNK_SIZE && !allocated);
 writes=0; fail_alloc=0; fail_write=1; outcome=TCP_ERR_INTERRUPTED; caught=false;
 if(!setjmp(fatal)) { syscall_write(3,(uintptr_t)data,1,10,0,0); assert(false); }
 free(data);
 puts("write syscall: shared ABI, finite refusal, blocking console, multi-chunk deadline, progress/error mapping and cleanup PASS");
}'''
with tempfile.TemporaryDirectory(prefix='tcp-write-syscall-') as directory:
    work = Path(directory); (work / 'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all','-I'+str(root/'abi/include'),str(work/'test.c'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True)
