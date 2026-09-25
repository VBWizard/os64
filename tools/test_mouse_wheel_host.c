// Real PS/2 assembly, HID decoding, input queue and VT scroll state, with
// port I/O and interrupt masking replaced by deterministic host stubs.
#include <stdint.h>
#include <stdbool.h>
#define SPINLOCK_H
#define IO_H
typedef volatile uint32_t spinlock_t;
static inline uint64_t spinlock_acquire_irqsave(spinlock_t *p) { (void)p; return 0; }
static inline void spinlock_release_irqrestore(spinlock_t *p, uint64_t f) { (void)p; (void)f; }
static inline void spinlock_acquire(spinlock_t *p) { (void)p; }
static inline void spinlock_release(spinlock_t *p) { (void)p; }
static uint8_t inb(uint16_t port);
static void outb(uint16_t port, uint8_t value);
#include "../kernel/src/gui/input.c"
#include "../kernel/src/driver/system/mouse.c"
#include "../kernel/src/driver/system/hid_mouse.c"
#include "../kernel/src/driver/system/usb/xhci.c"
#include "../kernel/src/tty.c"
#include "../kernel/src/vt_select.c"
extern int puts(const char *);
extern _Noreturn void abort(void);
volatile uint64_t kTicksSinceStart;
__uint128_t kDebugLevel;
struct Framebuffer kFrameBuffer = {.width=1024, .height=768};
static bool test_gui = true;
bool gui_owns_glass(void) { return test_gui; }
uint8_t keyboard_current_modifiers(void) { return OS64_GUI_MOD_SHIFT; }
void hid_keyboard_report(hid_keyboard_t *kbd, const uint8_t report[8])
{ (void)kbd; (void)report; abort(); }
static unsigned diagnostic_lines;
void printd(__uint128_t level, const char *fmt, ...)
{ (void)level; (void)fmt; ++diagnostic_lines; }
// Clipboard and paste are outside this fixture; reaching them is a failure.
void *kmalloc(uint64_t n) { (void)n; abort(); }
void kfree(void *p) { (void)p; abort(); }
snarf_pending_t *clipboard_begin(void) { abort(); }
int clipboard_append(snarf_pending_t *p,const void *b,size_t n)
{ (void)p; (void)b; (void)n; abort(); }
void clipboard_seal(snarf_pending_t *p) { (void)p; abort(); }
void clipboard_release(snarf_entry_t *p) { (void)p; abort(); }
bool console_intr_intercept_tty(struct tty *t,char ch) { (void)t; (void)ch; abort(); }
void console_classify_tty(struct tty *t,struct keyboard_event *e) { (void)t; (void)e; abort(); }
static unsigned painted_cells;
bool gui_vt8_seated(void) { return false; }
void renderer_glass_defer_locked(void) {}
void renderer_glass_background_locked(uint32_t color) { (void)color; }
void renderer_glass_blit_locked(void) {}
uint64_t renderer_glass_begin(void) { return 0; }
void renderer_glass_end(uint64_t f,uint32_t r,uint32_t c,bool show)
{ (void)f; (void)r; (void)c; (void)show; }
void renderer_glass_putc_bg_locked(char ch,uint8_t cs,uint32_t r,uint32_t c,uint32_t fg,uint32_t bg)
{ (void)ch; (void)cs; (void)r; (void)c; (void)fg; (void)bg; ++painted_cells; }
#include "hid_mouse_fixtures.h"
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { puts(#x); abort(); } } while (0)

// A mouse model that can lose the last rate ACK after changing mode.
static uint8_t replies[64], read_at, write_at, device_id, rates_seen, pending_rate;
static bool wheel_capable, lose_ack, no_device;
static uint8_t inb(uint16_t port)
{
    if (port == 0x64) return read_at != write_at ? 0x21 : 0;
    return read_at != write_at ? replies[read_at++] : 0;
}
static void outb(uint16_t port, uint8_t value)
{
    if (port != 0x60 || no_device) return;
    if (pending_rate) {
        pending_rate = 0;
        ++rates_seen;
        if (rates_seen == 3 && wheel_capable) device_id = 3;
        if (rates_seen == 3 && lose_ack) { lose_ack = false; return; }
    } else if (value == 0xF3) pending_rate = 1;
    replies[write_at++] = 0xFA;
    if (value == 0xF2) replies[write_at++] = device_id;
}
static void reset_mouse(bool capable, bool lost)
{
    read_at=write_at=device_id=rates_seen=pending_rate=0;
    wheel_capable=capable; lose_ack=lost; no_device=false;
}
static input_event_t event(uint8_t type)
{
    input_event_t ev;
    CHECK(input_pop(&ev)); CHECK(ev.type == type);
    return ev;
}
static void empty(void) { input_event_t ev; CHECK(!input_pop(&ev)); }
static void packet(uint8_t b, uint8_t x, uint8_t y, uint8_t z)
{
    mouse_handle_byte(b); mouse_handle_byte(x); mouse_handle_byte(y);
    if (s_packet_length == 4) mouse_handle_byte(z);
}
int main(void)
{
    reset_mouse(true,false); CHECK(mouse_negotiate()); CHECK(s_packet_length==4);
    reset_mouse(false,false); CHECK(mouse_negotiate()); CHECK(s_packet_length==3);
    reset_mouse(true,true); CHECK(mouse_negotiate()); CHECK(s_packet_length==4);
    reset_mouse(false,false); no_device=true; CHECK(!mouse_negotiate());

    input_init(); s_mouse_active=true; s_packet_index=0; s_packet_length=4;
    mouse_handle_byte(0); empty(); // bad header
    mouse_handle_byte(8); mouse_handle_byte(5); mouse_handle_byte(2); empty();
    mouse_handle_byte(0xFF);
    input_event_t ev=event(INPUT_EVENT_MOUSE_MOVE);
    CHECK(ev.mouse.dx==5 && ev.mouse.dy==-2);
    ev=event(INPUT_EVENT_MOUSE_WHEEL);
    CHECK(ev.mouse.dy==-1 && ev.mouse.dx==0 && ev.mouse.x==517 && ev.mouse.y==382);
    CHECK(ev.mouse.modifiers==OS64_GUI_MOD_SHIFT); empty();
    packet(0xC9,255,255,127); // overflow must retain click and wheel
    event(INPUT_EVENT_MOUSE_BUTTON_DOWN); ev=event(INPUT_EVENT_MOUSE_WHEEL);
    CHECK(ev.mouse.dy==127 && ev.mouse.buttons==1); empty();
    packet(8,0,0,128); event(INPUT_EVENT_MOUSE_BUTTON_UP);
    CHECK(event(INPUT_EVENT_MOUSE_WHEEL).mouse.dy==-128); empty();
    mouse_handle_byte(8); mouse_handle_byte(90); kTicksSinceStart+=3;
    packet(8,0,0,1); CHECK(event(INPUT_EVENT_MOUSE_WHEEL).mouse.dy==1); empty();
    s_packet_length=3; packet(8,0,0,127); empty();
    packet(8,1,0,127); event(INPUT_EVENT_MOUSE_MOVE); empty();

    xhci_hid_t usb={0}; uint8_t report[4]={0,0,0,255};
    hid_process_mouse_report(&usb,report,3); empty(); // stale fourth byte
    CHECK(usb.mouse_lengths_seen == 0); // tracing disabled
    kDebugLevel = DEBUG_USB; diagnostic_lines = 0;
    hid_process_mouse_report(&usb,report,3); empty();
    hid_process_mouse_report(&usb,report,3); empty();
    CHECK(diagnostic_lines == 1 && usb.mouse_wheel_samples == 0);
    kDebugLevel = 0;
    hid_process_mouse_report(&usb,report,4);
    CHECK(event(INPUT_EVENT_MOUSE_WHEEL).mouse.dy==1); empty();
    report[3]=128; hid_process_mouse_report(&usb,report,4);
    CHECK(event(INPUT_EVENT_MOUSE_WHEEL).mouse.dy==128); empty();
    input_pointer_source_t remote={0};
    input_inject_pointer(&remote,50,60,1); event(INPUT_EVENT_MOUSE_MOVE);
    event(INPUT_EVENT_MOUSE_BUTTON_DOWN);
    report[3]=1; hid_process_mouse_report(&usb,report,4);
    ev=event(INPUT_EVENT_MOUSE_WHEEL); CHECK(ev.mouse.buttons==1 && ev.mouse.dy==-1);
    input_release_pointer(&remote); event(INPUT_EVENT_MOUSE_BUTTON_UP); empty();

    // Repeated movement cannot flood tracing or spend the reserved tail
    // samples. Length-three input must not sample the stale fourth byte.
    kDebugLevel = DEBUG_USB; diagnostic_lines = 0;
    for (unsigned i = 0; i < 100; ++i) {
        hid_process_mouse_report(&usb,report,4);
        CHECK(event(INPUT_EVENT_MOUSE_WHEEL).mouse.dy == -1); empty();
    }
    CHECK(diagnostic_lines == 8 && usb.mouse_wheel_samples == 8);
    kDebugLevel = 0;

    // Report IDs and a nine-byte endpoint through the real xHCI completion
    // handler: a full packet completes once, and rearming requests nine bytes.
    xhci_t hc = {0}; xhci_trb_t trbs[RING_TRBS] = {0};
    uint8_t buffers[HID_INFLIGHT * HID_REPORT_BYTES] = {0};
    uint32_t doorbells[2] = {0};
    hc.db = doorbells;
    hc.mouse = (xhci_hid_t){.present=true,.kind=HID_MOUSE,.slot=1,.dci=3,
        .reports=buffers,.reports_phys=(uintptr_t)buffers,.report_bytes=9,
        .mouse_report_protocol=true,
        .intr={.trb=trbs,.phys=(uintptr_t)trbs,.enqueue=1,.cycle=1}};
    CHECK(hid_mouse_parse(packed,sizeof(packed),&hc.mouse.mouse_layout));
    CHECK(hid_mouse_parse(basic,sizeof(basic),&usb.mouse_layout));
    s_hc = &hc; trbs[0].param = (uintptr_t)buffers;
    buffers[0]=2; buffers[1]=1; buffers[5]=255;
    xhci_trb_t completion = {.param=(uintptr_t)trbs,
        .status=TRB_CC_SUCCESS << 24,.control=(1u << 24) | (3u << 16)};
    xhci_handle_transfer_event(&completion);
    CHECK(event(INPUT_EVENT_MOUSE_BUTTON_DOWN).mouse.buttons==1);
    CHECK(event(INPUT_EVENT_MOUSE_WHEEL).mouse.dy==1); empty();
    CHECK(trbs[1].status==9 && doorbells[1]==3);
    buffers[0]=3; // other report ID must not release the held left button
    xhci_handle_transfer_event(&completion); empty();
    CHECK(hc.mouse.pointer.buttons==1);
    buffers[0]=2; completion.status=(TRB_CC_SHORT_PACKET << 24) | 1;
    xhci_handle_transfer_event(&completion); empty(); // eight bytes is truncated
    input_release_pointer(&hc.mouse.pointer); event(INPUT_EVENT_MOUSE_BUTTON_UP); empty();

    uint8_t wide_desc[sizeof(basic)]; memcpy(wide_desc,basic,sizeof(basic));
    wide_desc[sizeof(basic)-7]=16;
    xhci_hid_t wide_dev={.mouse_report_protocol=true};
    CHECK(hid_mouse_parse(wide_desc,sizeof(wide_desc),&wide_dev.mouse_layout));
    const uint8_t wide_report[]={0,0,0,0,0,0,0x80};
    hid_process_mouse_report(&wide_dev,wide_report,sizeof(wide_report));
    CHECK(event(INPUT_EVENT_MOUSE_WHEEL).mouse.dy==32767); empty();

    // Text-only boot: no GUI queue, no rendering inside input injection.
    test_gui=false; s_active=false; kTTYReady=true;
    tty_cell_t cells[24*8]={0};
    tty_t tty={.index=0,.hist_lines=20,.cells=cells,.rows=4,.cols=8,.total_lines=24};
    kTTYFocused=&tty;
    input_inject_mouse(&s_pointer,0,0,0,-1);
    CHECK(tty.view_offset==3 && s_glassStale); empty();
    s_tty=&tty; s_view=0; s_gen=tty.generation; s_have_ptr=true; s_dirty=true;
    // Reproduce the overlay painting ahead of the deferred full repaint.
    vtsel_paint(); CHECK(s_view==3 && painted_cells==8);
    kTicksSinceStart+=3; tty_flush_if_dirty(); CHECK(!s_glassStale);
    unsigned after_flush=painted_cells;
    vtsel_paint(); CHECK(painted_cells==after_flush+8 && s_gen==tty.generation);
    // Opposite steps can restore the same offset before either painter runs.
    tty_view_wheel(-1); tty_view_wheel(1); CHECK(tty.view_offset==3);
    kTicksSinceStart+=3; tty_flush_if_dirty(); after_flush=painted_cells;
    vtsel_paint(); CHECK(painted_cells==after_flush+8);
    tty_view_wheel(INT16_MIN); CHECK(tty.view_offset==20);
    tty_view_wheel(1); CHECK(tty.view_offset==17);
    tty_view_wheel(INT16_MAX); CHECK(tty.view_offset==0);
    for (uint32_t i=0;i<7;++i) {
        tty.index=i; tty.view_offset=0; tty_view_wheel(-1); CHECK(tty.view_offset==3);
    }
    tty.index=7; tty.view_offset=0; tty_view_wheel(-1); CHECK(tty.view_offset==0);
    tty.index=0; tty.hist_lines=0; tty_view_wheel(-1); CHECK(tty.view_offset==0);
    puts("test_mouse_wheel_host: all checks passed");
    return 0;
}
