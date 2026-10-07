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
#include "xhci_bt_host.inc"
#include "strings/strlen.h"
#include "strings/sprintf.h"
extern void *aligned_alloc(size_t,size_t);
extern void free(void *);
extern int memcmp(const void *,const void *,size_t);
extern char *strstr(const char *,const char *);
extern int puts(const char *);
extern _Noreturn void abort(void);
#define CHECK(x) do { if (!(x)) { puts(#x); abort(); } } while (0)
uintptr_t kHHDMOffset;
volatile uint64_t kTicksSinceStart;
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
void hid_keyboard_report(hid_keyboard_t *k,const uint8_t r[8]) { (void)k; (void)r; abort(); }
bool hid_mouse_decode(const hid_mouse_layout_t *l,const uint8_t *r,size_t n,hid_mouse_sample_t *s)
{ (void)l; (void)r; (void)n; (void)s; abort(); }
void input_release_pointer(input_pointer_source_t *p) { (void)p; abort(); }
void input_inject_mouse(input_pointer_source_t *p,int16_t x,int16_t y,uint8_t b,int16_t w)
{ (void)p; (void)x; (void)y; (void)b; (void)w; abort(); }
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
       DEFERRED_ZERO_OPCODE_SUCCESS, DEFERRED_SECURE_FAILURE };
static unsigned scenario, secure_commands, resets, reads, ddc_commands, pump;
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

static void test_controller_step(void)
{
    if (++pump%16==0) ++kTicksSinceStart;
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
        CHECK(kind==TRB_CONFIG_ENDPOINT || kind==TRB_DISABLE_SLOT);
        if (kind==TRB_CONFIG_ENDPOINT) {
            const uint32_t *ctx=(void *)(uintptr_t)cmd->param;
            CHECK(ctx[1]==(1u|1u<<3|1u<<4|1u<<5));
            CHECK(((ctx[4*8+1]>>3)&7)==7); // Interrupt IN DCI 3.
            CHECK(((ctx[5*8+1]>>3)&7)==2); // Bulk OUT DCI 4.
            CHECK(((ctx[6*8+1]>>3)&7)==6); // Bulk IN DCI 5.
        } else released=scenario!=DISABLE_FAIL;
        emit(seen_command,((kind==TRB_DISABLE_SLOT && !released) ||
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
            if (op==0xfc05) { CHECK(command[2]==1 && command[3]==255); version_reply(); }
            else if (op==0xfc01) {
                CHECK(firmware_offset==(size_t)(bt_ax210_sfi_end-bt_ax210_sfi));
                CHECK(command[2]==8 && read32(command+7)==0x100800);
                CHECK(command[3]==0 && command[4]==1 && command[5]==0 && command[6]==1);
                resets++; booted=true;
                const uint8_t notification[]={0xff,7,2,0,1,0,0,0,0};
                if (scenario!=NO_BOOT_EVENT) queue(notification,sizeof(notification),1);
            } else {
                CHECK(op==0xfc8b && booted);
                size_t offset=ddc_commands ? 4 : 0;
                CHECK(ddc_commands<2 && command[2]==bt_ax210_ddc[offset]+1);
                CHECK(!memcmp(command+3,bt_ax210_ddc+offset,command[2]));
                ddc_commands++; command_complete(op,scenario==BAD_DDC ? 0x12 : 0);
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
    if (p && p->listening && queued_offset<queued_bytes) {
        xhci_bt_endpoint_t *rx=&p->rx[receive_endpoint];
        unsigned n=queued_bytes-queued_offset; if (n>64) n=64;
        memcpy(rx->buffer,queued+queued_offset,n); queued_offset+=n;
        emit(rx->pending_trb,((n==64?1u:13u)<<24)|(64-n),
             TRB_TYPE(TRB_EV_TRANSFER)|(2u<<24)|((receive_endpoint==0?3u:5u)<<16));
    }
}

int main(void)
{
    const uint8_t config[]={9,2,39,0,1,1,0,0x80,50, 9,4,0,0,3,0xe0,1,1,0,
        7,5,0x81,3,64,0,1, 7,5,2,2,64,0,1, 7,5,0x82,2,64,0,1};
    for (scenario=COLD_RSA;scenario<=DEFERRED_SECURE_FAILURE;scenario++) {
        xhci_t hc={0}; s_hc=&hc;
        uint64_t dcbaa[4]={0}, runtime[16]={0}; uint32_t doorbell[4]={0};
        xhci_trb_t events[RING_TRBS]={0};
        hc.dcbaa=dcbaa; hc.rt=(void *)runtime; hc.db=doorbell; hc.evt=events;
        hc.ctx_size=32; hc.evt_cycle=1;
        allocations=frees=secure_commands=resets=reads=ddc_commands=pump=0;
        failure_log[0]=0; deferred=false;
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
        CHECK(hc.bt_probe==NULL);
        if (scenario==CONFIGURE_FAIL || scenario==ALLOCATION_FAIL) CHECK(!secure_commands && !reads);
        else if (scenario==WARM || scenario==WRONG_ID) CHECK(!secure_commands && !resets && reads==1);
        else if (scenario==REJECT_FRAGMENT || scenario==BAD_USB_OUT || scenario==NO_USB_OUT)
            CHECK(secure_commands==1 && !resets);
        else if (scenario==SECURE_FAIL || scenario==NO_SECURE_EVENT) CHECK(!resets);
        else if (scenario==NO_BOOT_EVENT) CHECK(resets==1 && reads==1);
        else if (scenario==STILL_BOOTLOADER) CHECK(resets==1 && reads==2 && !ddc_commands);
        else if (scenario==BAD_DDC) CHECK(ddc_commands==1);
        else if (scenario==DEFERRED_SECURE_FAILURE) {
            CHECK(!resets && firmware_offset==(size_t)(bt_ax210_sfi_end-bt_ax210_sfi));
            CHECK(strstr(failure_log,"wait failed kind=1") && strstr(failure_log,"secure_failed=1"));
            CHECK(strstr(failure_log,"rejected event len=7 prefix=ff 05 06 01 00 00 00"));
        }
        else CHECK(secure_commands>2900 && resets==1 && reads==2 && ddc_commands==2);
        if (scenario==DEFERRED_ZERO_OPCODE_SUCCESS) CHECK(!failure_log[0]);
        if (scenario==DISABLE_FAIL) CHECK(!released && frees==5 && dcbaa[2]);
        else CHECK(released && allocations-frees==1 && !dcbaa[2]);
        // The command ring is controller-owned. Failed Disable Slot deliberately
        // retains device DMA as well; the fixture frees it after hardware stops.
        for (unsigned i=0;i<allocations;i++) if (allocated[i]) free(allocated[i]);
    }
    puts("test_bt_loader_host: RSA/ECDSA upload, warm skip, failures and DMA cleanup passed");
    return 0;
}
