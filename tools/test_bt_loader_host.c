// Deterministic xHCI peripheral: observe submitted TRBs and deliver real event
// ring entries. Time advances without sleeping; no real MMIO or port I/O runs.
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#define SPINLOCK_H
typedef volatile uint32_t spinlock_t;
static inline uint64_t spinlock_acquire_irqsave(spinlock_t *p) { (void)p; return 0; }
static inline void spinlock_release_irqrestore(spinlock_t *p,uint64_t f) { (void)p; (void)f; }
static void test_controller_step(void);
#include "../kernel/src/driver/system/usb/bt_intel.c"
#include "../kernel/src/driver/system/usb/bt_scan.c"
#include "../kernel/src/driver/system/hid_keyboard_map.c"
#include "../kernel/src/driver/system/usb/bt_le.c"
#include "../kernel/src/driver/system/usb/bt_manager.c"
#include "../kernel/src/driver/system/usb/bt_bond.c"
#include "../kernel/src/driver/system/usb/bt_host.c"
#include "../kernel/src/driver/system/hid_mouse.c"
#include "xhci_bt_host.inc"
#include "strings/strlen.h"
#include "strings/sprintf.h"
extern void *aligned_alloc(size_t,size_t);
extern void free(void *);
extern int memcmp(const void *,const void *,size_t);
extern char *strstr(const char *,const char *);
extern int puts(const char *);
extern int fflush(void *);
extern _Noreturn void abort(void);
#define CHECK(x) do { if (!(x)) { printf("line %u: %s\n", (unsigned)__LINE__, #x); fflush(NULL); abort(); } } while (0)
static uint8_t stored_bond[BT_BOND_RECORD_BYTES];
static bool store_exists,store_fail,forget_during_save;
static unsigned bond_saves;
bool bt_bond_load_slot(unsigned slot,bt_le_bond_t *bond,bool *automatic)
{
    CHECK(!s_poll_busy && slot<BT_HOST_PEERS);
    if(slot || !store_exists) { memset(bond,0,sizeof(*bond)); *automatic=false; return true; }
    return bt_bond_decode(bond,automatic,stored_bond,sizeof(stored_bond));
}
bool bt_bond_save_slot(unsigned slot,const bt_le_bond_t *bond,bool automatic)
{
    CHECK(!s_poll_busy && slot==0); bond_saves++;
    if(forget_during_save) {
        forget_during_save=false; CHECK(xhci_bluetooth_connection("forget",6));
    }
    if(store_fail) return false;
    bt_bond_encode(stored_bond,bond,automatic); store_exists=true; return true;
}
uintptr_t kHHDMOffset;
volatile uint64_t kTicksSinceStart;
volatile uint64_t kSystemCurrentTime;
// Fixed calendar fixture: exercise captured timestamps and formatting without
// importing the kernel's scheduler-dependent time module into the transport test.
struct tm *gmtime(const time_t *stamp,struct tm *date)
{
    CHECK(*stamp>=1791475200 && *stamp<1791475260);
    *date=(struct tm){.tm_year=126,.tm_mon=9,.tm_mday=8,.tm_hour=16,
                     .tm_sec=(int)(*stamp-1791475200)};
    return date;
}
__uint128_t kDebugLevel;
static char failure_log[1024];
void printd(__uint128_t level,const char *fmt,...)
{
    (void)level;
    if (!strstr(fmt,"AX210 wait failed") && !strstr(fmt,"AX210 rejected event")) return;
    size_t used=strlen(failure_log);
    va_list ap; va_start(ap,fmt);
    vsnprintf(failure_log+used,sizeof(failure_log)-used,fmt,ap);
    va_end(ap);
}
static unsigned bt_keys, bt_releases;
void hid_keyboard_tick(hid_keyboard_t *k) { (void)k; }
void hid_keyboard_report(hid_keyboard_t *k,const uint8_t r[8])
{
    CHECK(k->name && !memcmp(k->name,"bluetooth",10));
    if (r[2]) { CHECK(r[0]==2 && r[2]==4); bt_keys++; }
    else bt_releases++;
}
static unsigned mouse_packets;
static int16_t mouse_x,mouse_y,mouse_wheel;
static uint8_t mouse_buttons;
void input_release_pointer(input_pointer_source_t *p) { (void)p; }
void input_inject_mouse(input_pointer_source_t *p,int16_t x,int16_t y,uint8_t b,int16_t w)
{ (void)p; mouse_x=x; mouse_y=y; mouse_buttons=b; mouse_wheel=w; mouse_packets++; }
void wait(uint64_t ms) { kTicksSinceStart += (ms+9)/10; }

static void *allocated[32];
static unsigned allocations, frees, fail_after;
void *kmalloc_aligned(uint64_t size)
{
    if (allocations==fail_after) return NULL;
    CHECK(allocations<32 && size==PAGE_SIZE);
    void *p=aligned_alloc(PAGE_SIZE,size); CHECK(p); memset(p,0,size);
    allocated[allocations++]=p;
    return p;
}
void kfree(void *p)
{
    for (unsigned i=0;i<allocations;i++) if (allocated[i]==p) {
        allocated[i]=NULL; ++frees; free(p); return;
    }
    abort();
}

enum { COLD_RSA, COLD_ECDSA, WARM, WRONG_ID, REJECT_FRAGMENT, BAD_USB_OUT,
       NO_USB_OUT, SECURE_FAIL, NO_SECURE_EVENT, NO_BOOT_EVENT, STILL_BOOTLOADER,
       DISABLE_FAIL, BAD_DDC, CONFIGURE_FAIL, ALLOCATION_FAIL,
       DEFERRED_ZERO_OPCODE_SUCCESS, DEFERRED_SECURE_FAILURE,
       BOOT_BULK_ERROR, BOOT_INTR_ERROR, BOOT_REPEAT_ERROR, BOOT_RESET_FAIL,
       BOOT_ERROR_NO_NOTIFY, EARLY_BULK_ERROR, BOOT_STALL_ERROR, BOOT_PARTIAL_ERROR };
static unsigned scenario, secure_commands, resets, reads, ddc_commands, pump;
static unsigned endpoint_resets, halted_enqueue;
static bool rx_halted[2], repeated_error;
static int resume_endpoint;
static uint64_t halted_trb;
static uint64_t seen_control, seen_command, seen_tx;
static unsigned event_write, event_cycle, queued_bytes, queued_offset, receive_endpoint;
static uint8_t queued[257];
static bool booted, released, deferred;
static size_t firmware_offset, auth_offset;
static unsigned auth_region;

static void emit(uint64_t pointer, uint32_t status, uint32_t control)
{
    xhci_trb_t *event=&s_hc->evt[event_write];
    event->param=pointer; event->status=status;
    event->control=control | (event_cycle ? TRB_CYCLE : 0);
    if (++event_write==RING_TRBS) { event_write=0; event_cycle^=1; }
}
static void queue(const uint8_t *data,unsigned n,unsigned endpoint)
{
    CHECK(queued_bytes==queued_offset && n<=sizeof(queued));
    memcpy(queued,data,n); queued_bytes=n; queued_offset=0; receive_endpoint=endpoint;
}
static void version_reply(void)
{
    uint8_t version[]={0x0e,0,1,5,0xfc,0,
        0x10,4,0x10,4,0,0, 0x11,4,0x10,4,0,0,
        0x12,4,0,0x37,0x17,0, 0x1c,1,1, 0x2e,1,0, 0x2f,1,0,
        0x1f,4,0x3c,0x26,1,0, 0x32,4,0x58,0xc5,0xba,0x23,
        0xee,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    version[1]=sizeof(version)-2;
    version[26]=(scenario==WARM || (booted && scenario!=STILL_BOOTLOADER)) ? 3 : 1;
    version[32]=scenario==COLD_ECDSA ? 1 : 0;
    if (scenario==WRONG_ID) version[8]=0x20;
    queue(version,sizeof(version),reads%2); ++reads;
}
static void command_complete(uint16_t opcode,uint8_t status)
{
    uint8_t reply[]={0x0e,4,1,opcode&255,opcode>>8,status};
    queue(reply,sizeof(reply),secure_commands%2);
}
static void receive_error(unsigned index,unsigned cc)
{
    xhci_bt_endpoint_t *rx=&s_hc->bt_probe->rx[index];
    rx_halted[index]=true; halted_enqueue=rx->ring.enqueue; halted_trb=rx->pending_trb;
    emit(rx->pending_trb,(cc<<24)|(scenario==BOOT_PARTIAL_ERROR ? 63 : 64),
         TRB_TYPE(TRB_EV_TRANSFER)|(2u<<24)|((index==0?3u:5u)<<16));
}

static void test_controller_step(void)
{
    if (++pump%16==0) ++kTicksSinceStart;
    if (resume_endpoint>=0) {
        // Hardware cannot retry the halted TD until its doorbell is rung.
        unsigned dci=resume_endpoint==0 ? 3 : 5;
        CHECK(s_hc->db[2]==dci);
        CHECK(s_hc->bt_probe->rx[resume_endpoint].pending_trb==halted_trb);
        CHECK(s_hc->bt_probe->rx[resume_endpoint].ring.enqueue==halted_enqueue);
        rx_halted[resume_endpoint]=false;
        resume_endpoint=-1;
    }
    if (scenario==BOOT_REPEAT_ERROR && endpoint_resets && !repeated_error) {
        repeated_error=true;
        receive_error(1,4);
    }
    if (deferred && queued_bytes==queued_offset) {
        // A notification arriving after the final fragment's acknowledgment
        // exercises the final wait independently of command completion.
        const uint8_t notify[]={0xff,5,6,scenario==DEFERRED_SECURE_FAILURE,0,0,0};
        queue(notify,sizeof(notify),1);
        deferred=false;
    }
    if (s_hc->command_trb && seen_command!=s_hc->command_trb) {
        seen_command=s_hc->command_trb;
        const xhci_trb_t *cmd=(void *)(uintptr_t)seen_command;
        uint32_t kind=TRB_GET_TYPE(cmd->control);
        CHECK(kind==TRB_CONFIG_ENDPOINT || kind==TRB_DISABLE_SLOT || kind==TRB_RESET_ENDPOINT);
        if (kind==TRB_CONFIG_ENDPOINT) {
            const uint32_t *ctx=(void *)(uintptr_t)cmd->param;
            CHECK(ctx[1]==(1u|1u<<3|1u<<4|1u<<5));
            CHECK(((ctx[4*8+1]>>3)&7)==7); // Interrupt IN DCI 3.
            CHECK(((ctx[5*8+1]>>3)&7)==2); // Bulk OUT DCI 4.
            CHECK(((ctx[6*8+1]>>3)&7)==6); // Bulk IN DCI 5.
        } else if (kind==TRB_RESET_ENDPOINT) {
            unsigned index=scenario==BOOT_INTR_ERROR ? 0 : 1;
            CHECK(resets==1 && rx_halted[index]);
            CHECK(cmd->control==(TRB_TYPE(TRB_RESET_ENDPOINT)|(1u<<9)|
                                (2u<<24)|((index==0?3u:5u)<<16)|TRB_CYCLE));
            CHECK(!cmd->param && !cmd->status);
            CHECK(s_hc->bt_probe->rx[index].ring.enqueue==halted_enqueue);
            endpoint_resets++;
            if (scenario!=BOOT_RESET_FAIL) resume_endpoint=index;
        } else released=scenario!=DISABLE_FAIL;
        emit(seen_command,((kind==TRB_DISABLE_SLOT && !released) ||
                           (kind==TRB_RESET_ENDPOINT && scenario==BOOT_RESET_FAIL) ||
                           (kind==TRB_CONFIG_ENDPOINT && scenario==CONFIGURE_FAIL) ? 6u:1u)<<24,
             TRB_TYPE(TRB_EV_CMD_COMPLETE)|(2u<<24));
    }
    if (s_hc->control_status_trb && seen_control!=s_hc->control_status_trb) {
        seen_control=s_hc->control_status_trb;
        const xhci_trb_t *setup=(void *)(uintptr_t)s_hc->control_setup_trb;
        if ((setup->param & 0xffff)==0x0900) {
            CHECK((setup->param>>16 & 65535)==1);
        } else {
            CHECK((setup->param & 0xffff)==0x0020);
            const xhci_trb_t *data=(void *)(uintptr_t)s_hc->control_data_trb;
            const uint8_t *command=(void *)(uintptr_t)data->param;
            unsigned op=command[0]|command[1]<<8;
            CHECK(data->status==(uint32_t)command[2]+3);
            if (s_hc->bt_probe && s_hc->bt_probe->runtime) {
                CHECK(data->param==(uintptr_t)(s_hc->bt_probe->tx.buffer+512));
                uint8_t reply[14]={0x0e,4,1,op&255,op>>8,0};
                unsigned n=6;
                if (op==0x1003) { n=14; reply[1]=12; reply[10]=0x40; reply[12]=1; }
                if (op==0x1009) { n=12; reply[1]=10; reply[6]=0xab; }
                if (op==0x0401) {
                    const uint8_t inquiry[]={0x0f,4,0,1,1,4, 1,1,0};
                    queue(inquiry,sizeof(inquiry),0);
                } else queue(reply,n,0);
            } else if (op==0xfc05) {
                CHECK(command[2]==1 && command[3]==255); version_reply();
                if (scenario==EARLY_BULK_ERROR && reads==1) receive_error(1,4);
            }
            else if (op==0xfc01) {
                CHECK(firmware_offset==(size_t)(bt_ax210_sfi_end-bt_ax210_sfi));
                CHECK(command[2]==8 && read32(command+7)==0x100800);
                CHECK(command[3]==0 && command[4]==1 && command[5]==0 && command[6]==1);
                resets++; booted=true;
                const uint8_t notification[]={0xff,7,2,0,1,0,0,0,0};
                unsigned endpoint=scenario==BOOT_INTR_ERROR ? 0 : 1;
                if (scenario>=BOOT_BULK_ERROR && scenario!=EARLY_BULK_ERROR)
                    receive_error(endpoint,scenario==BOOT_STALL_ERROR ? 6 : 4);
                if (scenario!=NO_BOOT_EVENT && scenario!=BOOT_ERROR_NO_NOTIFY)
                    queue(notification,sizeof(notification),endpoint);
            } else {
                CHECK(op==0xfc8b && booted);
                size_t offset=ddc_commands ? 4 : 0;
                CHECK(ddc_commands<2 && command[2]==bt_ax210_ddc[offset]+1);
                CHECK(!memcmp(command+3,bt_ax210_ddc+offset,command[2]));
                ddc_commands++; command_complete(op,scenario==BAD_DDC || scenario==DISABLE_FAIL ? 0x12 : 0);
            }
        }
        emit(seen_control,1u<<24,TRB_TYPE(TRB_EV_TRANSFER)|(2u<<24)|(1u<<16));
    }
    xhci_bt_probe_t *p=s_hc->bt_probe;
    if (p && p->tx.pending_trb && seen_tx!=p->tx.pending_trb) {
        seen_tx=p->tx.pending_trb; secure_commands++;
        const xhci_trb_t *trb=(void *)(uintptr_t)seen_tx;
        CHECK(TRB_GET_TYPE(trb->control)==TRB_NORMAL && trb->param==(uintptr_t)p->tx.buffer);
        CHECK((trb->control & TRB_CYCLE)==(1u^((secure_commands-1)/255%2)));
        const uint8_t *command=p->tx.buffer;
        CHECK(command[0]==9 && command[1]==0xfc && command[2]>=1);
        size_t n=command[2]-1;
        CHECK(n<=252 && p->tx_bytes==n+4);
        const unsigned rsa_start[]={0,128,388}, rsa_size[]={128,256,256};
        const unsigned ec_start[]={644,772,868}, ec_size[]={128,96,96};
        const uint8_t types[]={0,3,2};
        if (auth_region<3) {
            const unsigned *starts=scenario==COLD_ECDSA ? ec_start : rsa_start;
            const unsigned *sizes=scenario==COLD_ECDSA ? ec_size : rsa_size;
            CHECK(command[3]==types[auth_region] && n<=sizes[auth_region]-auth_offset);
            CHECK(!memcmp(command+4,bt_ax210_sfi+starts[auth_region]+auth_offset,n));
            auth_offset+=n;
            if (auth_offset==sizes[auth_region]) { auth_region++; auth_offset=0; }
        } else {
            CHECK(command[3]==1 && !(n%4));
            CHECK(n<=(size_t)(bt_ax210_sfi_end-bt_ax210_sfi)-firmware_offset);
            CHECK(!memcmp(command+4,bt_ax210_sfi+firmware_offset,n)); firmware_offset+=n;
        }
        if (scenario!=NO_USB_OUT) {
            emit(seen_tx,(1u<<24)|(scenario==BAD_USB_OUT ? 1 : 0),
                 TRB_TYPE(TRB_EV_TRANSFER)|(2u<<24)|(4u<<16));
            command_complete(0xfc09,scenario==REJECT_FRAGMENT ? 0x0c : 0);
            if (firmware_offset==(size_t)(bt_ax210_sfi_end-bt_ax210_sfi) && scenario!=NO_SECURE_EVENT) {
                if (scenario==DEFERRED_ZERO_OPCODE_SUCCESS || scenario==DEFERRED_SECURE_FAILURE)
                    deferred=true;
                else {
                    const uint8_t notify[]={0xff,5,6,scenario==SECURE_FAIL,9,0xfc,0};
                    memcpy(queued+queued_bytes,notify,sizeof(notify)); queued_bytes+=sizeof(notify);
                }
            }
        }
    }
    if (p && p->listening && queued_offset<queued_bytes && !rx_halted[receive_endpoint]) {
        xhci_bt_endpoint_t *rx=&p->rx[receive_endpoint];
        unsigned n=queued_bytes-queued_offset; if (n>64) n=64;
        memcpy(rx->buffer,queued+queued_offset,n); queued_offset+=n;
        emit(rx->pending_trb,((n==64?1u:13u)<<24)|(64-n),
             TRB_TYPE(TRB_EV_TRANSFER)|(2u<<24)|((receive_endpoint==0?3u:5u)<<16));
    }
}

static void runtime_transport(void)
{
    xhci_bt_probe_t *p=s_hc->bt_probe;
    p->host=(bt_host_t){.initialized=true,.credits=1,.scan={.shared_controller=true}};
    for(unsigned i=0;i<BT_HOST_PEERS;i++) {
        p->host.peers[i].manager.loaded=true;
        p->host.peers[i].le.shared_controller=true;
    }
    bt_le_t *s=&p->host.peers[0].le;
    *s=(bt_le_t){.shared_controller=true,.phase=BT_LE_READY,.connected=true,.encrypted=true,
        .handle=11,.boot_value=10,.input_value=10,.now=kTicksSinceStart*1000/TICKS_PER_SECOND};
    const uint8_t key[]={0x0b,0x20,15,0,11,0,4,0,0x1b,10,0,2,0,4,0,0,0,0,0};
    memcpy(p->rx[1].buffer,key,sizeof(key));
    xhci_trb_t ev={.param=p->rx[1].pending_trb,.status=p->rx[1].endpoint.packet_bytes-sizeof(key)};
    CHECK(xhci_bt_transfer(&ev,p->device->slot,5,TRB_CC_SHORT_PACKET));
    CHECK(bt_keys==1 && p->rx[1].rearm_pending && s->reports==1);
    bt_le_t *mouse=&p->host.peers[1].le;
    *mouse=(bt_le_t){.shared_controller=true,.phase=BT_LE_READY,.connected=true,.encrypted=true,
        .handle=13,.input_value=20,.mouse=true,
        .mouse_layout={.bytes=4,.buttons={{0,1},{1,1},{2,1}},.x={8,8},.y={16,8},.wheel={24,8}}};
    const uint8_t motion[]={13,0x20,11,0,7,0,4,0,0x1b,20,0,1,0xfe,3,1};
    memcpy(p->rx[1].buffer,motion,sizeof(motion));
    ev=(xhci_trb_t){.param=p->rx[1].pending_trb,.status=p->rx[1].endpoint.packet_bytes-sizeof(motion)};
    CHECK(xhci_bt_transfer(&ev,p->device->slot,5,TRB_CC_SHORT_PACKET));
    CHECK(mouse_packets==1 && mouse_x==-2 && mouse_y==3 && mouse_buttons==1 && mouse_wheel==-1);
    uint8_t params[32],saved[35]; memset(params,0xa7,sizeof(params));
    xhci_bt_runtime_send(p,0x2017,params,sizeof(params));
    memcpy(saved,p->tx.buffer+512,sizeof(saved));
    xhci_bt_acl_send(p,key,sizeof(key));
    CHECK(!memcmp(saved,p->tx.buffer+512,sizeof(saved)) && !p->tx_done);
    ev=(xhci_trb_t){.param=p->tx.pending_trb};
    CHECK(xhci_bt_transfer(&ev,p->device->slot,4,TRB_CC_SUCCESS) && p->tx_done);
    s_hc->xfer_done=true; s_hc->xfer_cc=TRB_CC_SUCCESS; s_hc->xfer_actual=35;
    const uint8_t disconnect[]={5,4,0,11,0,0x13};
    memcpy(p->rx[0].buffer,disconnect,sizeof(disconnect));
    ev=(xhci_trb_t){.param=p->rx[0].pending_trb,.status=p->rx[0].endpoint.packet_bytes-sizeof(disconnect)};
    CHECK(xhci_bt_transfer(&ev,p->device->slot,3,TRB_CC_SHORT_PACKET));
    CHECK(s->phase==BT_LE_CLEANUP && !s->encrypted);
    unsigned enqueued=p->device->ep0.enqueue;
    xhci_bt_runtime_poll();
    CHECK(bt_releases==1 && s->phase==BT_LE_STOPPED && p->device->ep0.enqueue==enqueued);
    CHECK(mouse->phase==BT_LE_READY && mouse_packets==1 && mouse_buttons==1);
    CHECK(xhci_bluetooth_scan() && p->host.scan.phase==BT_SCAN_LE_PARAMS);
    CHECK(!xhci_bluetooth_connection("reconnect",9));
    p->host.scan=(bt_scan_t){.shared_controller=true};
    p->failed=p->host.failed=true; s->phase=BT_LE_FAILED; s->bond.valid=true;
    memset(s->bond.ltk,0x5a,sizeof(s->bond.ltk));
    CHECK(!xhci_bluetooth_connection("reconnect",9));
    CHECK(xhci_bluetooth_connection("forget",6) && !s->bond.valid);
    for(unsigned i=0;i<sizeof(s->bond.ltk);i++) CHECK(!s->bond.ltk[i]);
    CHECK(p->device->ep0.enqueue==enqueued);
    puts("PASS: production xHCI ACL receive, key release without Reset, and disjoint control/bulk DMA");
}

static void bond_maintenance(void)
{
    xhci_bt_probe_t *p=s_hc->bt_probe;
    p->failed=false; p->host=(bt_host_t){.initialized=true,.credits=1,.scan={.shared_controller=true}};
    for(unsigned i=0;i<BT_HOST_PEERS;i++) p->host.peers[i].le.shared_controller=true;
    store_exists=store_fail=forget_during_save=false; bond_saves=0;
    CHECK(!xhci_bluetooth_connection("reconnect",9));
    xhci_bluetooth_maintain(); CHECK(p->host.peers[0].manager.loaded && !p->host.peers[0].le.bond.valid);
    xhci_bluetooth_maintain(); CHECK(p->host.peers[1].manager.loaded);
    p->host.peers[0].le.bond=(bt_le_bond_t){.valid=true,.address_type=1,.last_address_type=1};
    p->host.peers[0].le.bond.peer[5]=0xc1; p->host.peers[0].le.bond.last_peer[5]=0xc1;
    memset(p->host.peers[0].le.bond.ltk,0x5a,16); p->host.peers[0].le.bond_revision++;
    // Keep this save-only fixture quiescent without starting automatic discovery.
    bt_manager_observe(&p->host.peers[0].manager,&p->host.peers[0].le,0); p->host.peers[0].manager.suppressed=true;
    store_fail=true; xhci_bluetooth_maintain();
    CHECK(p->host.peers[0].manager.dirty && p->host.peers[0].le.bond.valid && bond_saves==1);
    CHECK(xhci_bluetooth_connection("save",4)); store_fail=false;
    xhci_bluetooth_maintain(); CHECK(!p->host.peers[0].manager.dirty && store_exists && bond_saves==2);
    p->host.peers[0].manager=(bt_manager_t){0}; p->host.peers[0].le=(bt_le_t){.shared_controller=true};
    xhci_bluetooth_maintain(); CHECK(p->host.peers[0].manager.loaded && p->host.peers[0].manager.automatic && p->host.peers[0].le.bond.valid);
    CHECK(p->host.peers[0].manager.scan_owned && p->host.scan.phase==BT_SCAN_LE_PARAMS);
    // A completed scan hands off to connection in USB polling, without a worker visit.
    p->host.scan.phase=BT_SCAN_DONE; p->host.scan.count=1;
    p->host.scan.devices[0]=(bt_scan_device_t){.le=true,.address_type=1};
    memcpy(p->host.scan.devices[0].address,p->host.peers[0].le.bond.peer,6);
    s_hc->control_slot=0; p->tx_done=true;
    xhci_bt_runtime_poll();
    CHECK(p->host.peers[0].le.phase==BT_LE_ADDRESS && p->host.peers[0].le.bond_reused && !p->host.peers[0].manager.scan_owned);
    // Frequent USB polling must still honor the absent-peer retry deadline.
    p->host.peers[0].le.phase=BT_LE_IDLE; p->host.scan.phase=BT_SCAN_DONE; p->host.scan.count=0;
    p->host.peers[0].manager.scan_owned=true;
    xhci_bt_runtime_poll();
    CHECK(!p->host.peers[0].manager.scan_owned && p->host.scan.phase==BT_SCAN_DONE);
    xhci_bt_runtime_poll();
    CHECK(!p->host.peers[0].manager.scan_owned && p->host.scan.phase==BT_SCAN_DONE);
    kTicksSinceStart=(p->host.peers[0].manager.next_attempt*TICKS_PER_SECOND+999)/1000;
    xhci_bt_runtime_poll();
    CHECK(p->host.peers[0].manager.scan_owned && p->host.scan.phase==BT_SCAN_LE_PARAMS);
    // Restore a quiescent controller for the storage-only race below.
    p->host.peers[0].le.phase=BT_LE_IDLE; p->host.scan=(bt_scan_t){.shared_controller=true};
    p->host.peers[0].manager.suppressed=true;
    CHECK(xhci_bluetooth_connection("save",4)); forget_during_save=true;
    xhci_bluetooth_maintain();
    CHECK(!p->host.peers[0].le.bond.valid && p->host.peers[0].manager.dirty && !p->host.peers[0].manager.automatic);
    xhci_bluetooth_maintain(); CHECK(!p->host.peers[0].manager.dirty);
    p->host.peers[0].manager=(bt_manager_t){0}; p->host.peers[0].le=(bt_le_t){.shared_controller=true};
    xhci_bluetooth_maintain(); CHECK(!p->host.peers[0].le.bond.valid && !p->host.peers[0].manager.automatic);
    puts("PASS: worker storage outside poll lock, restore, write failure and forget racing a save");
}

static void scan_history(void)
{
    xhci_bt_probe_t *p=s_hc->bt_probe;
    p->host.owner=0; p->host.credits=1; p->host.failed=false;
    p->failed=false; p->tx_done=true; p->host.peers[0].le=(bt_le_t){.shared_controller=true}; p->host.scan=(bt_scan_t){.shared_controller=true};
    p->host.peers[0].manager=(bt_manager_t){.loaded=true};
    memset(&p->scan_history,0,sizeof(p->scan_history)); s_hc->control_slot=0;
    char out[4096];
    xhci_bluetooth_read(out,sizeof(out),0); CHECK(strstr(out,"last scan: none this boot"));
    kSystemCurrentTime=1791475200; kTicksSinceStart=1000;
    CHECK(xhci_bluetooth_scan()); xhci_bt_runtime_poll();
    xhci_bluetooth_read(out,sizeof(out),0);
    CHECK(strstr(out,"last scan: background LE; active"));
    CHECK(strstr(out,"last scan started: 2026-10-08 16:00:00 UTC (uptime ms: 10000)"));
    CHECK(strstr(out,"last scan finished: in progress"));
    kSystemCurrentTime++; kTicksSinceStart++;
    xhci_bt_runtime_poll(); CHECK(p->scan_history.started_at==1791475200);
    // Completing peer cleanup must preserve scan results and timestamps.
    p->host.scan.phase=BT_SCAN_LE_DISABLE; p->host.scan.pending=p->host.scan.command_done=true;
    p->host.scan.status=0; p->host.scan.opcode=0x200c; s_hc->control_slot=0;
    p->host.owner=1; p->host.command_done=true; p->host.credits=1;
    kSystemCurrentTime++; kTicksSinceStart++;
    xhci_bt_runtime_poll(); CHECK(p->scan_history.finished && !p->scan_history.failed);
    p->host.peers[0].le=(bt_le_t){.shared_controller=true,.phase=BT_LE_CLEANUP,
                   .deadline=kTicksSinceStart*1000/TICKS_PER_SECOND+3000};
    xhci_bt_runtime_poll(); CHECK(p->host.scan.phase==BT_SCAN_DONE);
    xhci_bluetooth_read(out,sizeof(out),0);
    CHECK(strstr(out,"last scan: background LE; complete"));
    CHECK(strstr(out,"last scan finished: 2026-10-08 16:00:02 UTC (uptime ms: 10020)"));
    CHECK(!xhci_bluetooth_read(out,sizeof(out),1)); // Device rows remain machine-readable.
    // A protocol failure followed by a late successful cleanup is also terminal
    // for scan history, but the adapter remains available for the next request.
    CHECK(bt_scan_start_le(&p->host.scan,kTicksSinceStart*1000/TICKS_PER_SECOND));
    xhci_bt_runtime_poll();
    p->host.scan.phase=BT_SCAN_CLEANUP; p->host.scan.pending=p->host.scan.command_done=true;
    p->host.scan.opcode=0x200c; p->host.scan.status=0;
    p->host.owner=1; p->host.command_done=true; p->host.credits=1; p->host.scan.error="test scan timeout";
    p->host.scan.deadline=kTicksSinceStart*1000/TICKS_PER_SECOND;
    s_hc->control_slot=0; kTicksSinceStart++; kSystemCurrentTime++;
    xhci_bt_runtime_poll(); xhci_bluetooth_read(out,sizeof(out),0);
    CHECK(p->host.scan.phase==BT_SCAN_STOPPED && p->scan_history.finished && p->scan_history.failed);
    CHECK(strstr(out,"stopped (retry available)") && strstr(out,"last scan: background LE; failed"));
    CHECK(strstr(out,"test scan timeout") && strstr(out,"discovery may be active: no"));
    kSystemCurrentTime=1791475210;
    CHECK(bt_scan_start_le(&p->host.scan,kTicksSinceStart*1000/TICKS_PER_SECOND));
    xhci_bt_runtime_poll(); p->failed=true; kSystemCurrentTime++;
    xhci_bt_runtime_poll(); xhci_bluetooth_read(out,sizeof(out),0);
    CHECK(strstr(out,"last scan: background LE; failed"));
    CHECK(strstr(out,"last scan started: 2026-10-08 16:00:10 UTC"));
    CHECK(strstr(out,"last scan finished: 2026-10-08 16:00:11 UTC"));
    kSystemCurrentTime=0; p->host.failed=false; p->host.owner=0; p->host.credits=1; p->failed=false; p->host.scan=(bt_scan_t){.shared_controller=true}; s_hc->control_slot=0;
    CHECK(xhci_bluetooth_scan()); xhci_bt_runtime_poll();
    xhci_bluetooth_read(out,sizeof(out),0); CHECK(strstr(out,"last scan started: clock unavailable"));
    char tiny[2]; CHECK(xhci_bluetooth_read(tiny,sizeof(tiny),0)==1 && !tiny[1]);
    puts("PASS: scan timestamps cover manual/background rounds, failure, cleanup retention and missing clock");
}

int main(void)
{
    const uint8_t config[]={9,2,39,0,1,1,0,0x80,50, 9,4,0,0,3,0xe0,1,1,0,
        7,5,0x81,3,64,0,1, 7,5,2,2,64,0,1, 7,5,0x82,2,64,0,1};
    for (scenario=COLD_RSA;scenario<=BOOT_PARTIAL_ERROR;scenario++) {
        xhci_t hc={0}; s_hc=&hc; s_bluetooth_hc=NULL;
        uint64_t dcbaa[4]={0}, runtime[16]={0}; uint32_t doorbell[4]={0};
        xhci_trb_t events[RING_TRBS]={0};
        hc.dcbaa=dcbaa; hc.rt=(void *)runtime; hc.db=doorbell; hc.evt=events;
        hc.ctx_size=32; hc.evt_cycle=1;
        allocations=frees=secure_commands=resets=reads=ddc_commands=pump=0;
        failure_log[0]=0; deferred=false;
        endpoint_resets=halted_enqueue=0;
        resume_endpoint=-1; halted_trb=0;
        rx_halted[0]=rx_halted[1]=repeated_error=false;
        fail_after=scenario==ALLOCATION_FAIL ? 5 : UINT32_MAX;
        seen_control=seen_command=seen_tx=0; event_write=0; event_cycle=1;
        queued_bytes=queued_offset=auth_region=auth_offset=0;
        firmware_offset=964; booted=released=false; kTicksSinceStart=0;
        CHECK(ring_init(&hc.cmd));
        xhci_device_t device={.slot=2,.port=3,.speed=1};
        CHECK(ring_init(&device.ep0));
        device.input_ctx=kmalloc_aligned(PAGE_SIZE); device.input_ctx_phys=(uintptr_t)device.input_ctx;
        device.dev_ctx=kmalloc_aligned(PAGE_SIZE); dcbaa[2]=(uintptr_t)device.dev_ctx;
        xhci_ax210_bringup(&device,config,sizeof(config));
        bool retained=scenario==COLD_RSA || scenario==COLD_ECDSA || scenario==WARM ||
            scenario==DEFERRED_ZERO_OPCODE_SUCCESS || scenario==BOOT_BULK_ERROR || scenario==BOOT_INTR_ERROR;
        CHECK((hc.bt_probe!=NULL)==retained);
        if (scenario==CONFIGURE_FAIL || scenario==ALLOCATION_FAIL) CHECK(!secure_commands && !reads);
        else if (scenario==EARLY_BULK_ERROR) CHECK(reads==1 && !secure_commands && !endpoint_resets);
        else if (scenario==WARM || scenario==WRONG_ID) CHECK(!secure_commands && !resets && reads==1);
        else if (scenario==REJECT_FRAGMENT || scenario==BAD_USB_OUT || scenario==NO_USB_OUT)
            CHECK(secure_commands==1 && !resets);
        else if (scenario==SECURE_FAIL || scenario==NO_SECURE_EVENT) CHECK(!resets);
        else if (scenario==NO_BOOT_EVENT) CHECK(resets==1 && reads==1);
        else if (scenario==STILL_BOOTLOADER) CHECK(resets==1 && reads==2 && !ddc_commands);
        else if (scenario==BAD_DDC || scenario==DISABLE_FAIL) CHECK(ddc_commands==1);
        else if (scenario==BOOT_REPEAT_ERROR || scenario==BOOT_RESET_FAIL || scenario==BOOT_ERROR_NO_NOTIFY)
            CHECK(endpoint_resets==1 && resets==1 && reads==1 && !ddc_commands);
        else if (scenario==BOOT_STALL_ERROR || scenario==BOOT_PARTIAL_ERROR)
            CHECK(!endpoint_resets && resets==1 && reads==1);
        else if (scenario==DEFERRED_SECURE_FAILURE) {
            CHECK(!resets && firmware_offset==(size_t)(bt_ax210_sfi_end-bt_ax210_sfi));
            CHECK(strstr(failure_log,"wait failed kind=1") && strstr(failure_log,"secure_failed=1"));
            CHECK(strstr(failure_log,"rejected event len=7 prefix=ff 05 06 01 00 00 00"));
        }
        else CHECK(secure_commands>2900 && resets==1 && reads==2 && ddc_commands==2);
        if (scenario==BOOT_BULK_ERROR || scenario==BOOT_INTR_ERROR)
            CHECK(endpoint_resets==1 && !failure_log[0]);
        if (scenario==DEFERRED_ZERO_OPCODE_SUCCESS) CHECK(!failure_log[0]);
        if (retained) {
            CHECK(hc.bt_probe==&hc.bluetooth && hc.bluetooth.device==&hc.bluetooth_device);
            CHECK(!released && allocations-frees==10 && dcbaa[2]);
            // The original stack device can change after enumeration returns.
            device.slot=99;
            CHECK(!xhci_bluetooth_scan());
            xhci_bluetooth_start_manager();
            xhci_bluetooth_maintain(); CHECK(hc.bluetooth.host.peers[0].manager.loaded);
            CHECK(!xhci_bluetooth_scan()); // Initial controller setup already owns discovery.
            for (unsigned poll=0;poll<5000 && hc.bluetooth.host.scan.phase!=BT_SCAN_DONE;poll++) {
                kTicksSinceStart++;
                xhci_drain_events(); xhci_bt_runtime_poll();
            }
            CHECK(hc.bluetooth.host.scan.phase==BT_SCAN_DONE && !hc.bluetooth.host.scan.radio_active);
            CHECK(!released && allocations-frees==10);
            if (scenario==WARM) { runtime_transport(); bond_maintenance(); scan_history(); }
        } else if (scenario==DISABLE_FAIL) CHECK(!released && allocations-frees==10 && dcbaa[2]);
        else CHECK(released && allocations-frees==1 && !dcbaa[2]);
        // The command ring is controller-owned. Failed Disable Slot deliberately
        // retains device DMA as well; the fixture frees it after hardware stops.
        for (unsigned i=0;i<allocations;i++) if (allocated[i]) free(allocated[i]);
    }
    puts("test_bt_loader_host: RSA/ECDSA upload, boot receive recovery, warm skip, failures and DMA cleanup passed");
    return 0;
}
