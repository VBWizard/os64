# Compile current TCP bodies with deterministic platform, clock and IPv4 stubs.
# Includes alone are removed; protocol code and checksum helpers are unmodified.
from pathlib import Path
import sys
import re
r=Path(__file__).resolve().parents[1]
o=Path(sys.argv[1])
o.mkdir(parents=True, exist_ok=True)
def source(path):
 return '\n'.join(x for x in (r/path).read_text().splitlines() if not x.startswith('#include'))+'\n'
s='''#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define TICKS_PER_SECOND 100
#define DEBUG_NET 0
#define printd(...) ((void)0)
#define panic(...) abort()
#define OS64_NET_ERR_NO_RESOURCES 1
#define OS64_NET_ERR_REFUSED 2
#define OS64_NET_ERR_TIMEOUT 3
#define THREAD_STATE_ISLEEP 1
#define SIGSLEEP 0
#define IPV4_PROTO_TCP 6
typedef unsigned spinlock_t;
typedef struct { int threadState; } thread_t;
typedef struct { thread_t *currentThread; } core_local_storage_t;
typedef struct { unsigned mtu; } net_device_t;
'''
s+=re.search(r"typedef enum\s*\{[^}]+\} ipv4_tx_t;", (r/"kernel/include/driver/net/ipv4.h").read_text()).group(0)
s+='''
static uint64_t kTicksSinceStart;
static uint32_t kNetIPv4Address=0x0a00020f;
static core_local_storage_t cls;
static core_local_storage_t *get_core_local_storage(void) { return &cls; }
static void spinlock_acquire(spinlock_t *l) { assert(!*l); *l=1; }
static void spinlock_release(spinlock_t *l) { assert(*l); *l=0; }
static uint64_t spinlock_acquire_irqsave(spinlock_t *l) { spinlock_acquire(l); return 0; }
static void spinlock_release_irqrestore(spinlock_t *l,uint64_t f) { (void)f; spinlock_release(l); }
static void *kmalloc(size_t n) { return calloc(1,n); }
static void kfree(void *p) { free(p); }
static bool signal_park_must_end(thread_t *t) { (void)t; return false; }
static void signal_raise(int s,uint64_t w,thread_t *t) { (void)s;(void)t;kTicksSinceStart=w; }
static void scheduler_wake_isleep_thread_locked(thread_t *t) { (void)t; }
static void nap(unsigned n) { kTicksSinceStart+=n; }
static ipv4_tx_t disposition=IPV4_TX_SENT;
struct packet { uint64_t tick; uint32_t seq,ack; uint16_t len; uint8_t flags; ipv4_tx_t how; uint8_t payload[1460]; };
static struct packet packets[8192];
static unsigned npackets;
static int32_t ipv4_send_from_ex(net_device_t *,uint32_t,uint32_t,uint8_t,const void *,uint16_t,ipv4_tx_t *);
static int32_t ipv4_send(net_device_t *,uint32_t,uint8_t,const void *,uint16_t);
'''
s+=source('kernel/include/driver/net/net_wire.h')
s+=source('kernel/src/driver/net/net_checksum.c')
s+=source('kernel/include/driver/net/tcp.h')
s+=source('kernel/src/driver/net/tcp.c')
s+='''
static int32_t ipv4_send_from_ex(net_device_t *d,uint32_t src,uint32_t dst,uint8_t proto,const void *p,uint16_t len,ipv4_tx_t *how) {
 (void)d;(void)src;(void)dst;(void)proto;
 const uint8_t *b=p; assert(npackets < 8192);
 ipv4_tx_t fate = (unsigned)len + 20 > d->mtu ? IPV4_TX_INVALID : disposition;
 struct packet *packet=&packets[npackets++];
 *packet=(struct packet){.tick=kTicksSinceStart,.seq=net_read32(b+4),.ack=net_read32(b+8),.len=len-(b[12]>>4)*4,.flags=b[13],.how=fate};
 assert(packet->len <= sizeof(packet->payload));
 memcpy(packet->payload,b+(b[12]>>4)*4,packet->len);
 *how=fate; return fate==IPV4_TX_SENT ? 0 : -2;
}
static int32_t ipv4_send(net_device_t *d,uint32_t dst,uint8_t proto,const void *p,uint16_t n) {
 ipv4_tx_t how; return ipv4_send_from_ex(d,kNetIPv4Address,dst,proto,p,n,&how);
}
'''
s+=Path(__file__).with_name('test_tcp_host.c').read_text()
(o/'harness.c').write_text(s)
