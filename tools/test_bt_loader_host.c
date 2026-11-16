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
#include "../kernel/src/driver/system/usb/bt_acl.c"
#include "../kernel/src/driver/system/usb/bt_classic.c"
#include "../kernel/src/driver/system/usb/bt_l2cap.c"
#include "../kernel/src/driver/system/usb/bt_sdp.c"
#include "../kernel/src/driver/system/usb/bt_avdtp.c"
#include "../kernel/src/audio/sbc_frame.c"
#include "../kernel/src/driver/system/usb/bt_a2dp.c"
#include "../kernel/src/driver/system/usb/bt_avrcp.c"
#include "../kernel/src/driver/system/usb/bt_host.c"
#include "../kernel/src/driver/system/hid_mouse.c"
#include "../kernel/src/driver/system/usb/xhci_service.c"
#include "xhci_bt_host.inc"
#include "strings/strlen.h"
#include "strings/sprintf.h"
extern void *aligned_alloc(size_t,size_t);
extern void free(void *);
extern int memcmp(const void *,const void *,size_t);
extern char *strstr(const char *,const char *);
extern int strcmp(const char *,const char *);
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
// The Classic bond's file, as a record in memory: absent until saved.
static uint8_t stored_classic[BT_CLASSIC_BOND_RECORD_BYTES];
static bool classic_exists,classic_fail,classic_change_during_save;
static unsigned classic_loads,classic_saves;
bool bt_classic_bond_load(bt_classic_bond_t *bond)
{
    CHECK(!s_poll_busy); classic_loads++;
    memset(bond,0,sizeof(*bond));
    if(classic_fail) return false;
    if(!classic_exists) return true;
    return bt_classic_bond_decode(bond,stored_classic,sizeof(stored_classic));
}
bool bt_classic_bond_save(const bt_classic_bond_t *bond)
{
    CHECK(!s_poll_busy); classic_saves++;
    if(classic_change_during_save) {
        // The person forgets the bond while its old self is being written.
        classic_change_during_save=false; CHECK(xhci_bluetooth_connection("slot 2 forget",13));
    }
    if(classic_fail) return false;
    bt_classic_bond_encode(stored_classic,bond); classic_exists=true; return true;
}
uintptr_t kHHDMOffset;
volatile uint64_t kTicksSinceStart;
uint64_t kCPUCyclesPerSecond = 1000000000;
// What the driver asks of kusb's side of the kernel: a bell nobody is
// parked on, a clock that only has to move, and a sleep this harness never
// reaches (it is kworker's wait for the serializing flag, which is free).
doorbell_t kUsbDoorbell;
void doorbell_ring(doorbell_t *db) { db->rings++; }
uint64_t rdtsc(void) { static uint64_t cycles; return cycles += 1000; }
// The pump's clock, and its way to the seam and the encoder: no Classic
// link comes up in these cases, so the pump never runs, and a call to any
// of its ops is a failure.
uint64_t micros_now(void) { return kTicksSinceStart * 10000; }
static bt_a2dp_pulled_t no_pull(void *c,int16_t *o,uint32_t f) { (void)c; (void)o; (void)f; abort(); }
static bool no_sound(void *c) { (void)c; abort(); }
static void no_seam(void *c) { (void)c; abort(); }
static void no_failed(void *c,const char *w) { (void)c; (void)w; abort(); }
static void no_delay(void *c,uint32_t d) { (void)c; (void)d; abort(); }
static bool no_begin(void *c,const sbc_settings_t *s) { (void)c; (void)s; abort(); }
static bool no_encode(void *c,const int16_t *p,uint8_t *o,size_t n) { (void)c; (void)p; (void)o; (void)n; abort(); }
static uint64_t no_clock(void *c) { (void)c; abort(); }
const bt_a2dp_ops_t kBtA2dpSeamOps={.pull=no_pull,.has_sound=no_sound,.attach=no_seam,.detach=no_seam,.failed=no_failed,
                                    .delay=no_delay,.encoder_begin=no_begin,.encoder_encode=no_encode,.clock_us=no_clock};
void signal_raise(signals signal, uint64_t data, void *thread) { (void)signal; (void)data; (void)thread; }
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
static unsigned classic_stop_lines, classic_refused_lines;
void printd(__uint128_t level,const char *fmt,...)
{
    (void)level;
    if (strstr(fmt,"(Classic) stopped")) classic_stop_lines++;
    if (strstr(fmt,"refused Classic setup command")) classic_refused_lines++;
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
void input_pointer_register(input_pointer_source_t *p,const char *key,const char *name)
{ CHECK(key[0] && name[0]); p->settings_slot=1; }
void input_pointer_unregister(input_pointer_source_t *p) { p->settings_slot=0; }
void input_inject_mouse(input_pointer_source_t *p,int16_t x,int16_t y,uint8_t b,int16_t w)
{ (void)p; mouse_x=x; mouse_y=y; mouse_buttons=b; mouse_wheel=w; mouse_packets++; }
void wait(uint64_t ms) { kTicksSinceStart += (ms+9)/10; }

static void *allocated[32];
static unsigned allocations, frees, fail_after;
void *kmalloc_aligned(uint64_t size)
{
    if (allocations==fail_after) return NULL;
    // A page, or the transmit endpoint's page and its send slots: ASan
    // then polices every slot's bounds for the cases below.
    CHECK(allocations<32 && (size==PAGE_SIZE || size==BT_TX_BYTES));
    void *p=aligned_alloc(PAGE_SIZE,size); CHECK(p); memset(p,0,size);
    allocated[allocations++]=p;
    return p;
}
// The probe itself (xhci_ax210_bringup allocates it rather than build it on
// the stack): one allocation a bring-up, freed exactly once whichever way
// bring-up ends, and refused when `probe_refused` says so.
static void *probe_block;
static unsigned probe_allocs, probe_frees;
static bool probe_refused;
extern void *calloc(size_t,size_t);
void *kmalloc_try(uint64_t size)
{
    CHECK(size==sizeof(xhci_bt_probe_t) && !probe_block);
    if (probe_refused) return NULL;
    probe_block=calloc(1,size); CHECK(probe_block);
    probe_allocs++;
    return probe_block;
}
void kfree(void *p)
{
    if (p && p==probe_block) { probe_block=NULL; probe_frees++; free(p); return; }
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
static unsigned scenario, secure_commands, resets, reads, ddc_commands, pump, classic_setup_seen;
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
    xhci_trb_t *event=&s_hc->evt.trb[event_write];
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
                // The Classic setup: each command whole, the name "os64" in
                // 248 bytes, within the 258 the command storage holds.
                static const uint16_t setup_ops[6]={0x0c56,0x0c13,0x0c24,0x080f,0x0c18,0x0c16};
                for (unsigned i=0;i<6;i++) if (op==setup_ops[i]) classic_setup_seen|=1u<<i;
                if (op==0x0c13) {
                    CHECK(command[2]==248 && !memcmp(command+3,"os64",4));
                    for (unsigned i=7;i<3+248;i++) CHECK(!command[i]);
                }
                uint8_t reply[14]={0x0e,4,1,op&255,op>>8,0};
                unsigned n=6;
                if (op==0x1003) { n=14; reply[1]=12; reply[10]=0x40; reply[12]=1; }
                if (op==0x1009) { n=12; reply[1]=10; reply[6]=0xab; }
                // 1021-byte ACL packets, 8 of them; LE: 251 bytes, 3.
                if (op==0x1005) { n=13; reply[1]=11; reply[6]=0xfd; reply[7]=3; reply[8]=64; reply[9]=8; reply[11]=8; }
                if (op==0x2002) { n=9; reply[1]=7; reply[6]=251; reply[8]=3; }
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

// The send slots (AUDIO_CLASSIC.md § T2): several ACL packets with the
// controller at once, each in storage of its own, freed in the order they
// were queued, each with its own deadline; a packet that cannot be carried
// fails the transport and writes nothing. Run last: it ends with the
// transport failed.
static void send_slots(void)
{
    xhci_bt_probe_t *p=s_hc->bt_probe;
    p->failed=false; p->tx_done=true; p->tx_slots=p->tx_oldest=0;
    p->host=(bt_host_t){.initialized=true,.credits=1,.scan={.shared_controller=true}};
    s_hc->control_slot=0;
    uint8_t packet[BT_TX_SLOT_BYTES], zero[BT_TX_SLOT_BYTES]={0}, page[512], command[258];
    uint64_t trb[BT_TX_SLOTS];
    xhci_trb_t ev;
    memset(command,0xc3,sizeof(command)); memcpy(p->tx.buffer+512,command,sizeof(command));
    memcpy(page,p->tx.buffer,sizeof(page));

    // Four of the largest packets, all in flight.
    for (unsigned i=0;i<BT_TX_SLOTS;i++) {
        memset(packet,0x10+i,sizeof(packet));
        xhci_bt_acl_send(p,packet,sizeof(packet));
        trb[i]=p->tx.pending_trb;
        CHECK(!p->failed && p->tx_slots==i+1 && !p->tx_done);
        const xhci_trb_t *queued=(void *)(uintptr_t)trb[i];
        CHECK(queued->param==(uintptr_t)xhci_bt_tx_slot(p,i) && (queued->status&0x1ffff)==sizeof(packet));
        CHECK(!memcmp(xhci_bt_tx_slot(p,i),packet,sizeof(packet)));
    }
    // None of them reached the command's bytes or the page bring-up sends from.
    CHECK(!memcmp(p->tx.buffer+512,command,sizeof(command)) && !memcmp(p->tx.buffer,page,sizeof(page)));
    for (unsigned i=0;i+1<BT_TX_SLOTS;i++)
        CHECK(xhci_bt_tx_slot(p,i)+BT_TX_SLOT_BYTES<=xhci_bt_tx_slot(p,i+1));

    // A completion for one that is not the oldest frees nothing, and is counted.
    unsigned unmatched=p->tx.unmatched;
    ev=(xhci_trb_t){.param=trb[2]};
    CHECK(xhci_bt_transfer(&ev,p->device->slot,4,TRB_CC_SUCCESS));
    CHECK(p->tx_slots==BT_TX_SLOTS && p->tx.unmatched==unmatched+1 && !p->failed && !p->tx_done);
    // In order, each frees its own slot and wipes it; the last makes the endpoint idle.
    for (unsigned i=0;i<BT_TX_SLOTS;i++) {
        ev=(xhci_trb_t){.param=trb[i]};
        CHECK(xhci_bt_transfer(&ev,p->device->slot,4,TRB_CC_SUCCESS));
        CHECK(p->tx_slots==BT_TX_SLOTS-1-i && p->tx_done==(i==BT_TX_SLOTS-1) && !p->failed);
        CHECK(!memcmp(xhci_bt_tx_slot(p,i),zero,sizeof(zero)));
        if (i+1<BT_TX_SLOTS) CHECK(xhci_bt_tx_slot(p,i+1)[0]==0x10+i+1);
    }
    // The slots are a ring: with two freed and two more sent, the fourth and
    // the first are in use again, in that order.
    for (unsigned i=0;i<3;i++) { packet[0]=0x40+i; xhci_bt_acl_send(p,packet,8); trb[i]=p->tx.pending_trb; }
    ev=(xhci_trb_t){.param=trb[0]}; CHECK(xhci_bt_transfer(&ev,p->device->slot,4,TRB_CC_SUCCESS));
    ev=(xhci_trb_t){.param=trb[1]}; CHECK(xhci_bt_transfer(&ev,p->device->slot,4,TRB_CC_SUCCESS));
    packet[0]=0x50; xhci_bt_acl_send(p,packet,8); trb[3]=p->tx.pending_trb;
    packet[0]=0x51; xhci_bt_acl_send(p,packet,8);
    CHECK(p->tx_slots==3 && p->tx_oldest==2 && xhci_bt_tx_slot(p,2)[0]==0x42);
    CHECK(xhci_bt_tx_slot(p,3)[0]==0x50 && xhci_bt_tx_slot(p,0)[0]==0x51);
    ev=(xhci_trb_t){.param=trb[2]}; CHECK(xhci_bt_transfer(&ev,p->device->slot,4,TRB_CC_SUCCESS));
    ev=(xhci_trb_t){.param=trb[3]}; CHECK(xhci_bt_transfer(&ev,p->device->slot,4,TRB_CC_SUCCESS));
    ev=(xhci_trb_t){.param=p->tx.pending_trb}; CHECK(xhci_bt_transfer(&ev,p->device->slot,4,TRB_CC_SUCCESS));
    CHECK(p->tx_done && !p->tx_slots && !p->failed);

    // Each slot has its own five seconds: a later send does not buy the
    // oldest more time.
    kTicksSinceStart+=10;
    uint64_t first=kTicksSinceStart;
    xhci_bt_acl_send(p,packet,8);
    kTicksSinceStart=first+3*TICKS_PER_SECOND;
    xhci_bt_acl_send(p,packet,8);
    kTicksSinceStart=first+5*TICKS_PER_SECOND-1;
    xhci_bt_runtime_poll(); CHECK(!p->host.failed && p->tx_slots==2);
    kTicksSinceStart=first+5*TICKS_PER_SECOND;
    xhci_bt_runtime_poll();
    CHECK(p->host.failed);
    // What is still queued is the controller's to read, failed transport or
    // not: neither slot is touched, and the endpoint is not called idle.
    unsigned held=p->tx_oldest;   // the ring has moved on: these two are wherever it now is
    CHECK(p->tx_slots==2 && !p->tx_done && xhci_bt_tx_slot(p,held)[0]==0x51 &&
          xhci_bt_tx_slot(p,(held+1)%BT_TX_SLOTS)[0]==0x51);
    // The same when the failure is the control endpoint's: the bulk
    // endpoint is still running.
    p->host=(bt_host_t){.initialized=true,.credits=1,.scan={.shared_controller=true}};
    kTicksSinceStart=first; s_hc->control_slot=p->device->slot; s_hc->xfer_done=true; s_hc->xfer_cc=TRB_CC_USB_TRANSACTION_ERROR;
    xhci_bt_runtime_poll();
    CHECK(p->host.failed && p->tx_slots==2 && !p->tx_done && xhci_bt_tx_slot(p,held)[0]==0x51);
    s_hc->control_slot=0; s_hc->xfer_cc=TRB_CC_SUCCESS;

    // A fifth packet with four in flight, and one too long for a slot:
    // the transport fails, nothing is queued and nothing is written.
    // (The poll above wiped the idle command's bytes, as it does; plant them again.)
    memcpy(p->tx.buffer+512,command,sizeof(command));
    for (unsigned pass=0;pass<2;pass++) {
        p->failed=false; p->tx_done=true; p->tx_slots=p->tx_oldest=0;
        memset(packet,0x77,sizeof(packet));
        if (pass==0) for (unsigned i=0;i<BT_TX_SLOTS;i++) xhci_bt_acl_send(p,packet,8);
        unsigned queued=p->tx.ring.enqueue, in_flight=p->tx_slots;
        uint8_t before[BT_TX_SLOTS*BT_TX_SLOT_STRIDE];
        memcpy(before,p->tx.buffer+PAGE_SIZE,sizeof(before));
        uint8_t long_packet[BT_TX_SLOT_BYTES+1]; memset(long_packet,0x99,sizeof(long_packet));
        if (pass==0) xhci_bt_acl_send(p,packet,8);
        else xhci_bt_acl_send(p,long_packet,sizeof(long_packet));
        CHECK(p->failed && p->tx.ring.enqueue==queued && p->tx_slots==in_flight);
        CHECK(!memcmp(before,p->tx.buffer+PAGE_SIZE,sizeof(before)));
        CHECK(!memcmp(p->tx.buffer+512,command,sizeof(command)));
    }
    // A transfer that did not succeed fails the transport.
    p->failed=false; p->tx_done=true; p->tx_slots=p->tx_oldest=0;
    xhci_bt_acl_send(p,packet,8);
    ev=(xhci_trb_t){.param=p->tx.pending_trb};
    CHECK(xhci_bt_transfer(&ev,p->device->slot,4,TRB_CC_USB_TRANSACTION_ERROR) && p->failed);
    // Bring-up is as it was: its one transfer, matched by its own pointer.
    p->failed=false; p->runtime=false; p->tx_done=false; p->tx.pending_trb=0x1234;
    ev=(xhci_trb_t){.param=0x9999}; CHECK(xhci_bt_transfer(&ev,p->device->slot,4,TRB_CC_SUCCESS) && !p->tx_done);
    ev=(xhci_trb_t){.param=0x1234}; CHECK(xhci_bt_transfer(&ev,p->device->slot,4,TRB_CC_SUCCESS) && p->tx_done && !p->failed);
    p->runtime=true; p->failed=true;
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
    CHECK(xhci_bluetooth_scan(false) && p->host.scan.phase==BT_SCAN_LE_PARAMS);
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
    uint64_t rings=kUsbDoorbell.rings;
    xhci_bluetooth_maintain(); CHECK(p->host.peers[0].manager.loaded && p->host.peers[0].manager.automatic && p->host.peers[0].le.bond.valid);
    // The worker publishes the restored bond and rings kusb; the connection
    // policy is timing, and runs in kusb's pass, not here.
    CHECK(kUsbDoorbell.rings==rings+1 && !p->host.peers[0].manager.scan_owned);
    s_hc->control_slot=0; p->tx_done=true;
    xhci_bt_runtime_poll();
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

// The Classic bond through the transport: read once the host is up with
// its slot, given to the link (a bond made as another adapter is none),
// saved when the link changes it, the dirty mark cleared only if nothing
// changed meanwhile, a failed save tried again later; the timeline and
// the log follow the link.
static void classic_maintenance(void)
{
    xhci_bt_probe_t *p=s_hc->bt_probe;
    bt_host_t *h=&p->host;
    p->failed=false; s_hc->control_slot=0; p->tx_done=true;
    *h=(bt_host_t){.initialized=true,.credits=1,.scan={.shared_controller=true}};
    for(unsigned i=0;i<BT_HOST_PEERS;i++) { h->peers[i].manager.loaded=true; h->peers[i].le.shared_controller=true; }
    const uint8_t local[6]={0xab,0,0,0,0,0};
    memcpy(h->scan.address,local,6); h->scan.features[4]=0x40;
    classic_exists=classic_fail=classic_change_during_save=false; classic_loads=classic_saves=0;
    // Not available: nothing is read.
    xhci_bluetooth_maintain(); CHECK(!classic_loads);
    bt_classic_init(&h->classic,local); h->classic_available=true;
    // No file: the link is told there is no bond, and nothing is paged.
    xhci_bluetooth_maintain(); CHECK(classic_loads==1 && h->classic.bond_loaded && !h->classic.bond.valid);
    CHECK(!h->classic_auto && !h->classic_page_at && !h->classic_storage);
    xhci_bluetooth_maintain(); CHECK(classic_loads==1 && !classic_saves);
    // A bond on disk: loaded, and paged once.
    bt_classic_bond_t bond={.valid=true,.key_type=4};
    memset(bond.peer,0xa1,6); memcpy(bond.local,local,6); memset(bond.key,0x5c,16);
    bt_classic_bond_encode(stored_classic,&bond); classic_exists=true;
    bt_classic_init(&h->classic,local);
    uint64_t rings=kUsbDoorbell.rings;
    xhci_bluetooth_maintain();
    CHECK(classic_loads==2 && h->classic.bond.valid && !memcmp(h->classic.bond.key,bond.key,16));
    CHECK(h->classic_auto && h->classic_page_at && kUsbDoorbell.rings==rings+1);
    // Another adapter's bond is none.
    bond.local[0]^=1; bt_classic_bond_encode(stored_classic,&bond); bond.local[0]^=1;
    bt_classic_init(&h->classic,local);
    xhci_bluetooth_maintain(); CHECK(h->classic.bond_loaded && !h->classic.bond.valid && !h->classic_auto);
    // An unreadable file: no bond, and the status says so.
    classic_fail=true; bt_classic_init(&h->classic,local);
    xhci_bluetooth_maintain(); CHECK(h->classic.bond_loaded && !h->classic.bond.valid);
    CHECK(h->classic_storage && !strcmp(h->classic_storage,"saved bond unavailable or invalid"));
    classic_fail=false;
    // A bond the link made is saved, and the mark cleared.
    h->classic.bond=bond; h->classic.bond_dirty=true;
    xhci_bluetooth_maintain();
    CHECK(classic_saves==1 && !h->classic.bond_dirty && !h->classic_storage && classic_exists);
    bt_classic_bond_t back; CHECK(bt_classic_bond_decode(&back,stored_classic,sizeof(stored_classic)) && back.valid);
    // A save that fails keeps the mark, says so, and waits ten seconds.
    h->classic.bond.key[0]^=1; h->classic.bond_dirty=true; classic_fail=true;
    xhci_bluetooth_maintain(); CHECK(classic_saves==2 && h->classic.bond_dirty && h->classic_storage);
    xhci_bluetooth_maintain(); CHECK(classic_saves==2);
    classic_fail=false;
    kTicksSinceStart+=10*TICKS_PER_SECOND;
    xhci_bluetooth_maintain(); CHECK(classic_saves==3 && !h->classic.bond_dirty && !h->classic_storage);
    // Forgotten while the old bond is being written: the save that was
    // under way does not clear the mark, and the next writes "no bond".
    h->classic.bond.key[0]^=1; h->classic.bond_dirty=true; classic_change_during_save=true;
    xhci_bluetooth_maintain();
    CHECK(classic_saves==4 && h->classic.bond_dirty && !h->classic.bond.valid);
    xhci_bluetooth_maintain();
    CHECK(classic_saves==5 && !h->classic.bond_dirty);
    CHECK(bt_classic_bond_decode(&back,stored_classic,sizeof(stored_classic)) && !back.valid);
    // The timeline follows the link's phase; a stop with an error is said once.
    bt_classic_bond_t saved=bond; h->classic.bond=saved;
    h->classic.phase=BT_CLASSIC_PAGING;
    uint64_t lines=s_timeline_count;
    xhci_bt_runtime_poll();
    CHECK(s_timeline_count==lines+1 && p->timeline_classic_phase==h->classic.phase);
    const xhci_timeline_entry_t *line=&s_timeline[(s_timeline_count-1)%XHCI_TIMELINE];
    CHECK(line->kind==XHCI_TL_CLASSIC_PHASE && line->slot==BT_HOST_CLASSIC_SLOT && line->to==BT_CLASSIC_PAGING);
    CHECK(!strcmp(xhci_classic_phase_name(BT_CLASSIC_PAGING),"paging") && !strcmp(xhci_classic_phase_name(99),"?"));
    xhci_bt_runtime_poll(); CHECK(s_timeline_count==lines+1);
    // The log: a stop with an error is one line, said again only when the
    // error changes or after the link was READY; a refused setup command once.
    classic_stop_lines=classic_refused_lines=0;
    h->classic.phase=BT_CLASSIC_STOPPED; h->classic.error="the page was not answered";
    xhci_bt_runtime_poll(); xhci_bt_runtime_poll(); CHECK(classic_stop_lines==1);
    h->classic.error="authentication failed"; xhci_bt_runtime_poll(); CHECK(classic_stop_lines==2);
    h->classic.phase=BT_CLASSIC_READY; xhci_bt_runtime_poll();
    h->classic.phase=BT_CLASSIC_STOPPED; xhci_bt_runtime_poll(); CHECK(classic_stop_lines==3);
    h->classic.phase=BT_CLASSIC_IDLE; h->classic.error=NULL;
    h->classic_setup_refused=true; h->classic_refused_opcode=0x0c56; h->classic_refused_status=0x0c;
    xhci_bt_runtime_poll(); xhci_bt_runtime_poll(); CHECK(classic_refused_lines==1);
    h->classic_setup_refused=false;
    // `scan classic` reaches the host: an inquiry, then the LE scan.
    h->classic=(bt_classic_t){0}; bt_classic_init(&h->classic,local);
    h->scan=(bt_scan_t){.shared_controller=true}; h->scan.features[4]=0x40; h->owner=0; h->credits=1;
    CHECK(xhci_bluetooth_scan(true) && h->scan.phase==BT_SCAN_INQUIRY_MODE && !h->scan.le_only);
    h->scan=(bt_scan_t){.shared_controller=true}; h->scan.features[4]=0x60;
    CHECK(!xhci_bluetooth_scan(true) && xhci_bluetooth_scan(false) && h->scan.phase==BT_SCAN_LE_PARAMS);
    // And `slot 2` commands reach the link through the connection file.
    h->scan=(bt_scan_t){.shared_controller=true};
    // Not before the bond file has been read: the read would undo it.
    CHECK(!xhci_bluetooth_connection("slot 2 connect 4c:87:5d:c3:b2:a1\n",33) && h->classic.phase==BT_CLASSIC_IDLE);
    h->classic.bond_loaded=true;
    CHECK(xhci_bluetooth_connection("slot 2 connect 4c:87:5d:c3:b2:a1\n",33) && h->classic.phase==BT_CLASSIC_PAGING);
    char status[8192]; xhci_bluetooth_read(status,sizeof(status),2);
    CHECK(strstr(status,"\nslot 2:\nkind: classic\nstate: paging\n"));
    h->classic=(bt_classic_t){0}; h->classic_available=false;
    puts("PASS: the Classic bond through the transport: load, another adapter's, unreadable, save, failure, forget racing a save; timeline, scan classic, slot 2");
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
    CHECK(xhci_bluetooth_scan(false)); xhci_bt_runtime_poll();
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
    CHECK(xhci_bluetooth_scan(false)); xhci_bt_runtime_poll();
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
        // The adapter's port as enumeration leaves it: connected, enabled,
        // powered, with its connect-change bit standing.
        uint32_t operational[(0x400+0x10*8)/4]={0}; hc.op=(void *)operational; hc.max_ports=8;
        uint32_t *portsc=&operational[XHCI_OP_PORTSC(3)/4];
        *portsc=PORTSC_CCS|PORTSC_PED|PORTSC_PP|(1u<<17);
        hc.dcbaa=dcbaa; hc.rt=(void *)runtime; hc.db=doorbell; hc.evt=(xhci_evt_ring_t){.trb=events,.size=RING_TRBS,.cycle=1};
        hc.ctx_size=32;
        allocations=frees=secure_commands=resets=reads=ddc_commands=pump=0;
        probe_allocs=probe_frees=0;
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
        // The probe was allocated once and is gone, whichever way it ended:
        // a retained adapter lives on in the controller's own storage.
        CHECK(probe_allocs==1 && probe_frees==1 && !probe_block);
        bool retained=scenario==COLD_RSA || scenario==COLD_ECDSA || scenario==WARM ||
            scenario==DEFERRED_ZERO_OPCODE_SUCCESS || scenario==BOOT_BULK_ERROR || scenario==BOOT_INTR_ERROR;
        CHECK((hc.bt_probe!=NULL)==retained);
        // A retained adapter has its port's change bits acknowledged, with
        // power written back and the enable bit NOT written (a one there
        // disables the port). The model is plain memory, so what is read
        // here is the value written.
        if (retained) CHECK(*portsc==(PORTSC_PP|(1u<<17)));
        else CHECK(*portsc==(PORTSC_CCS|PORTSC_PED|PORTSC_PP|(1u<<17)));
        // The reset on the way down. Nothing is asked of a host that has
        // not started; once it has, kusb's poll sends HCI_Reset and stops
        // running the host, and only the reset's own Command Complete is
        // the answer.
        if (scenario==WARM) {
            xhci_bt_probe_t *bt=hc.bt_probe;
            s_bt_quiesce=BT_QUIESCE_NONE;
            xhci_bluetooth_quiesce_begin();
            CHECK(!xhci_bluetooth_quiescing());
            bt->host.initialized=true;
            xhci_bluetooth_quiesce_begin();
            CHECK(xhci_bluetooth_quiescing() && s_bt_quiesce==BT_QUIESCE_ASKED);
            hc.control_slot=0;
            xhci_bt_runtime_poll();
            CHECK(s_bt_quiesce==BT_QUIESCE_SENT && hc.control_slot==bt->device->slot);
            CHECK(bt->tx.buffer[512]==0x03 && bt->tx.buffer[513]==0x0c && bt->tx.buffer[514]==0);
            xhci_bt_runtime_poll();
            CHECK(s_bt_quiesce==BT_QUIESCE_SENT);
            static const uint8_t other[]={0x0e,0x04,0x01,0x01,0x10,0x00};
            xhci_bt_runtime_event(bt,other,sizeof(other));
            CHECK(xhci_bluetooth_quiescing());
            static const uint8_t answer[]={0x0e,0x04,0x01,0x03,0x0c,0x00};
            xhci_bt_runtime_event(bt,answer,sizeof(answer));
            CHECK(!xhci_bluetooth_quiescing() && s_bt_quiesce==BT_QUIESCE_ANSWERED);
            CHECK(xhci_bluetooth_quiesce_verdict()[0]=='r' && xhci_bluetooth_quiesce_verdict()[2]=='s');
            CHECK(bt->recent_events==2 && bt->recent_event[1][3]==0x03);
            // A reset the controller rejects ends the wait and is not called a reset.
            s_bt_quiesce=BT_QUIESCE_SENT;
            static const uint8_t rejected[]={0x0e,0x04,0x01,0x03,0x0c,0x0c};
            xhci_bt_runtime_event(bt,rejected,sizeof(rejected));
            CHECK(!xhci_bluetooth_quiescing() && s_bt_quiesce==BT_QUIESCE_REFUSED);
            CHECK(xhci_bluetooth_quiesce_verdict()[2]=='f');
            s_bt_quiesce=BT_QUIESCE_NONE; bt->host.initialized=false;
        }
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
            // The host was given slot 2's audio sink at bring-up.
            CHECK(hc.bluetooth.host.a2dp_ops==&kBtA2dpSeamOps && !hc.bluetooth.host.a2dp.attached);
            CHECK(!released && allocations-frees==10 && dcbaa[2]);
            // The original stack device can change after enumeration returns.
            device.slot=99;
            CHECK(!xhci_bluetooth_scan(false));
            xhci_bluetooth_start_manager();
            xhci_bluetooth_maintain(); CHECK(hc.bluetooth.host.peers[0].manager.loaded);
            CHECK(!xhci_bluetooth_scan(false)); // Initial controller setup already owns discovery.
            // Past the command's 258 bytes, the rest of the transmit
            // storage's first page is nobody's: a guard pattern there must
            // survive the start-up round, Write Local Name and all.
            uint8_t *guard=hc.bluetooth.tx.buffer+512+258;
            memset(guard,0xc5,PAGE_SIZE-512-258);
            classic_setup_seen=0;
            for (unsigned poll=0;poll<5000 && hc.bluetooth.host.scan.phase!=BT_SCAN_DONE;poll++) {
                kTicksSinceStart++;
                xhci_drain_events(); xhci_bt_runtime_poll();
            }
            CHECK(hc.bluetooth.host.scan.phase==BT_SCAN_DONE && !hc.bluetooth.host.scan.radio_active);
            for (unsigned i=0;i<PAGE_SIZE-512-258;i++) CHECK(guard[i]==0xc5);
            // The controller has BR/EDR and took every Classic setup command:
            // the Classic slot is there, with this adapter's address.
            CHECK(classic_setup_seen==0x3f);
            CHECK(hc.bluetooth.host.classic_available && !hc.bluetooth.host.classic_setup_refused);
            CHECK(!memcmp(hc.bluetooth.host.classic.local,hc.bluetooth.host.scan.address,6));
            CHECK(!released && allocations-frees==10);
            // The start-up round read the controller's ACL buffers, and the host
            // took them before a later scan could clear the round's copy.
            CHECK(hc.bluetooth.host.acl.known && !bt_acl_shared(&hc.bluetooth.host.acl));
            CHECK(hc.bluetooth.host.acl.classic_size==1021 && hc.bluetooth.host.acl.classic_total==8);
            CHECK(hc.bluetooth.host.acl.le_size==251 && hc.bluetooth.host.acl.le_total==3);
            {
                char status[2048];
                bt_host_status(&hc.bluetooth.host,status,sizeof(status));
                CHECK(strstr(status,"acl pools: classic 8 x 1021 bytes, in use 0; le 3 x 251 bytes, in use 0; "
                                    "stray completions 0, bad completions 0\n")!=NULL);
            }
            if (scenario==WARM) { runtime_transport(); bond_maintenance(); classic_maintenance(); scan_history(); send_slots(); }
        } else if (scenario==DISABLE_FAIL) CHECK(!released && allocations-frees==10 && dcbaa[2]);
        else CHECK(released && allocations-frees==1 && !dcbaa[2]);
        // The command ring is controller-owned. Failed Disable Slot deliberately
        // retains device DMA as well; the fixture frees it after hardware stops.
        for (unsigned i=0;i<allocations;i++) if (allocated[i]) free(allocated[i]);
    }
    // No memory for the probe: nothing is configured or asked, the slot is
    // released, and no pointer to a probe is left behind.
    {
        xhci_t hc={0}; s_hc=&hc; s_bluetooth_hc=NULL;
        uint64_t dcbaa[4]={0}, runtime[16]={0}; uint32_t doorbell[4]={0};
        xhci_trb_t events[RING_TRBS]={0};
        uint32_t operational[(0x400+0x10*8)/4]={0}; hc.op=(void *)operational; hc.max_ports=8;
        hc.dcbaa=dcbaa; hc.rt=(void *)runtime; hc.db=doorbell; hc.evt=(xhci_evt_ring_t){.trb=events,.size=RING_TRBS,.cycle=1};
        hc.ctx_size=32;
        allocations=frees=secure_commands=resets=reads=probe_allocs=probe_frees=0;
        fail_after=UINT32_MAX; scenario=WARM; event_write=0; event_cycle=1; released=false;
        CHECK(ring_init(&hc.cmd));
        xhci_device_t device={.slot=2,.port=3,.speed=1};
        CHECK(ring_init(&device.ep0));
        device.input_ctx=kmalloc_aligned(PAGE_SIZE); device.input_ctx_phys=(uintptr_t)device.input_ctx;
        device.dev_ctx=kmalloc_aligned(PAGE_SIZE); dcbaa[2]=(uintptr_t)device.dev_ctx;
        probe_refused=true;
        xhci_ax210_bringup(&device,config,sizeof(config));
        probe_refused=false;
        CHECK(!probe_allocs && !probe_frees && !hc.bt_probe && !s_bluetooth_hc);
        CHECK(released && !dcbaa[2] && !secure_commands && !reads && allocations-frees==1);
        for (unsigned i=0;i<allocations;i++) if (allocated[i]) free(allocated[i]);
    }
    puts("test_bt_loader_host: RSA/ECDSA upload, boot receive recovery, warm skip, failures and DMA cleanup passed");
    return 0;
}
