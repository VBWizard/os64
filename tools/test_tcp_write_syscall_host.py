#!/usr/bin/env python3
"""Exercise the current write_for syscall body and libos64 stub with host seams."""
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
assert re.search(r'SYSCALL_DEFINE\(SYSCALL_WRITE_FOR,.*false, 0x00\)', source)
assert re.search(r'SYSCALL_DEFINE\(SYSCALL_WRITE,.*false, 0x02\)', source)
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
'''
header += re.search(r'^#define OS64_INTERRUPTED .*', (root / 'abi/include/os64/signal.h').read_text(), re.M).group(0) + '\n'
for text, pattern in ((source, r'^#define (?:SYSCALL_RESULT_\w+|TCP_WRITE_FOR_CHUNK_SIZE) .*'),
                      ((root / 'kernel/include/driver/net/tcp.h').read_text(), r'^#define TCP_ERR_\w+ .*')):
    header += '\n'.join(re.findall(pattern, text, re.M)) + '\n'
header += '''
typedef struct { int unused; } task_t;
typedef struct { int unused; } tcp_conn_t;
typedef struct { int type; void *object; } handle_t;
typedef struct { task_t *task; } core_local_storage_t;
static task_t task;
static tcp_conn_t connection;
static handle_t handle = {HANDLE_NET_TCP, &connection};
static core_local_storage_t cls = {&task};
static uint64_t kTicksSinceStart, captured_deadline, captured_args[5];
static bool captured_bound, refuse_alloc, copy_ok=true, caught=true;
static size_t allocated, copied, requested, writes;
static long outcome;
static jmp_buf fatal;
static core_local_storage_t *get_core_local_storage(void) { return &cls; }
static handle_t *handle_get(task_t *t, int h) { assert(t==&task); return h==3 ? &handle : NULL; }
static void *kmalloc(size_t n) { if(refuse_alloc) return NULL; allocated++; return malloc(n); }
static void kfree(void *p) { assert(allocated); allocated--; free(p); }
static bool copy_user_buffer(const void *p, void *out, size_t n) {
 if (!copy_ok || !p) return false;
 copied=n; memcpy(out,p,n); return true;
}
static long tcp_conn_write_for(tcp_conn_t *c, const void *p, size_t n, bool bounded, uint64_t end) {
 assert(c==&connection && p && n==copied); writes++; requested=n; captured_bound=bounded; captured_deadline=end; return outcome;
}
static bool current_thread_will_catch(void) { return caught; }
static void raise_terminating_signal_and_die(task_t *t, void *p) { assert(t==&task && !p && !allocated); longjmp(fatal,1); }
static uint64_t os64_syscall4(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
 captured_args[0]=n; captured_args[1]=a; captured_args[2]=b; captured_args[3]=c; captured_args[4]=d; return 23;
}
'''
program = header + function(source, 'syscall_write_for') + '\n'
program += function((root / 'userland/libos64/io.c').read_text(), 'os64_write_for') + '\n'
program += '''int main(void) {
 unsigned char data[4096]={0};
 assert(os64_write_for(-2,data,99,1234)==23);
 assert(captured_args[0]==55 && captured_args[1]==(uint64_t)(int64_t)-2 && captured_args[2]==(uintptr_t)data && captured_args[3]==99 && captured_args[4]==1234);
 assert(syscall_write_for(2,0,0,0,0,0)==SYSCALL_RESULT_INVALID);
 handle.type=8; assert(syscall_write_for(3,0,0,0,0,0)==SYSCALL_RESULT_INVALID); handle.type=HANDLE_NET_TCP;
 assert(syscall_write_for(3,0,0,0,0,0)==0 && !writes);
 assert(syscall_write_for(3,0,1,0,0,0)==SYSCALL_RESULT_BAD_USER_DATA && !allocated);
 copy_ok=false;
 assert(syscall_write_for(3,(uintptr_t)data,1,0,0,0)==SYSCALL_RESULT_BAD_USER_DATA && !allocated && !writes);
 copy_ok=true;
 refuse_alloc=true; assert(syscall_write_for(3,(uintptr_t)data,1,0,0,0)==SYSCALL_RESULT_INVALID); refuse_alloc=false;
 uint64_t ms[]={0,1,10,11,UINT64_MAX-1,OS64_WAIT_FOREVER};
 for(size_t i=0;i<sizeof ms/sizeof *ms;i++) {
  kTicksSinceStart=100; outcome=7;
  assert(syscall_write_for(3,(uintptr_t)data,UINT64_MAX,ms[i],0,0)==7 && !allocated && requested==4096);
  assert(captured_bound==(ms[i]!=OS64_WAIT_FOREVER));
  if(captured_bound) assert(captured_deadline==100+ms[i]/10+(ms[i]%10!=0));
 }
 kTicksSinceStart=UINT64_MAX-3; outcome=1;
 assert(syscall_write_for(3,(uintptr_t)data,1,100,0,0)==1 && captured_deadline==UINT64_MAX && captured_bound);
 kTicksSinceStart=0; outcome=TCP_ERR_TIMEOUT;
 assert((int64_t)syscall_write_for(3,(uintptr_t)data,1,0,0,0)==OS64_ERR_TIMEOUT && captured_bound && !captured_deadline);
 outcome=TCP_ERR_INTERRUPTED;
 assert((int64_t)syscall_write_for(3,(uintptr_t)data,1,10,0,0)==OS64_INTERRUPTED && !allocated);
 caught=false; if(!setjmp(fatal)) { syscall_write_for(3,(uintptr_t)data,1,10,0,0); assert(false); }
 outcome=TCP_ERR_RESET;
 assert(syscall_write_for(3,(uintptr_t)data,1,10,0,0)==SYSCALL_RESULT_INVALID && !allocated);
 puts("write_for syscall: ABI, supported handles, bounded copy, deadlines, overflow, errors and cleanup PASS");
}'''
with tempfile.TemporaryDirectory(prefix='tcp-write-syscall-') as directory:
    work = Path(directory); (work / 'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all','-I'+str(root/'abi/include'),str(work/'test.c'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True)
