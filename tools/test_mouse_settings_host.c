#include <assert.h>
#include <stdio.h>
#include <stddef.h>
extern int memcmp(const void *,const void *,size_t);
#include <stdint.h>
#include <stdbool.h>
#define SPINLOCK_H
typedef volatile uint32_t spinlock_t;
static uint64_t spinlock_acquire_irqsave(spinlock_t *p) { (void)p; return 0; }
static void spinlock_release_irqrestore(spinlock_t *p,uint64_t flags) { (void)p; (void)flags; }
#include "../kernel/src/gui/input.c"
volatile uint64_t kTicksSinceStart;
struct Framebuffer kFrameBuffer={.width=4096,.height=4096};
bool gui_owns_glass(void) { return true; }
void tty_view_wheel(int16_t delta) { (void)delta; }
uint8_t keyboard_current_modifiers(void) { return 0; }
void printd(__uint128_t level,const char *fmt,...) { (void)level; (void)fmt; }
static input_event_t take(unsigned type)
{
    input_event_t e; assert(input_pop(&e)); assert(e.type==type); return e;
}
static void empty(void) { input_event_t e; assert(!input_pop(&e)); }
static void set(const char *key,unsigned speed,unsigned right)
{
    os64_mouse_command_t c={.version=1,.count=1,.expected_generation=s_mouse_generation};
    strcpy(c.settings[0].key,key); c.settings[0].speed=speed; c.settings[0].right_primary=right;
    assert(input_mouse_apply(&c,sizeof(c))==(int)sizeof(c));
}
int main(void)
{
    // Restore while the bonded mouse is offline; discovery must inherit it.
    set("bt-1-aabbccddeeff",200,0);
    input_init();
    input_pointer_source_t bt={0},usb={0},remote={0};
    input_pointer_register(&bt,"bt-1-aabbccddeeff","Bluetooth mouse");
    input_pointer_register(&usb,"usb-01-p2","USB mouse");
    input_pointer_register(&bt,"bt-1-aabbccddeeff","Bluetooth mouse");
    assert(s_mouse_sources[bt.settings_slot-1]==1);
    input_inject_mouse(&bt,10,-5,0,1);
    input_event_t e=take(INPUT_EVENT_MOUSE_MOVE); assert(e.mouse.dx==20 && e.mouse.dy==-10);
    e=take(INPUT_EVENT_MOUSE_WHEEL); assert(e.mouse.dy==1);
    input_inject_mouse(&usb,10,-5,0,0);
    e=take(INPUT_EVENT_MOUSE_MOVE); assert(e.mouse.dx==10 && e.mouse.dy==-5);
    input_inject_pointer(&remote,100,200,0);
    e=take(INPUT_EVENT_MOUSE_MOVE); assert(e.mouse.x==100 && e.mouse.y==200);
    set("bt-1-aabbccddeeff",25,0);
    for(unsigned i=0;i<3;i++) { input_inject_mouse(&bt,1,-1,0,0); empty(); }
    input_inject_mouse(&bt,1,-1,0,0);
    e=take(INPUT_EVENT_MOUSE_MOVE); assert(e.mouse.dx==1 && e.mouse.dy==-1);
    // Setting changes discard an old scale's fractional remainder.
    input_inject_mouse(&bt,1,0,0,0); empty(); set("bt-1-aabbccddeeff",100,0);
    input_inject_mouse(&bt,1,0,0,0); assert(take(INPUT_EVENT_MOUSE_MOVE).mouse.dx==1);
    // Remap during a held left press: its release must still be left.
    input_inject_mouse(&bt,0,0,1,0); assert(take(INPUT_EVENT_MOUSE_BUTTON_DOWN).mouse.button==0);
    set("bt-1-aabbccddeeff",100,1);
    input_inject_mouse(&bt,0,0,0,0); assert(take(INPUT_EVENT_MOUSE_BUTTON_UP).mouse.button==0);
    input_inject_mouse(&bt,0,0,2,0); assert(take(INPUT_EVENT_MOUSE_BUTTON_DOWN).mouse.button==0);
    // Another mouse can share a logical press; one release cannot drop both.
    input_inject_mouse(&usb,0,0,1,0); empty();
    input_pointer_unregister(&bt); empty(); assert(buttons_held()==1);
    input_inject_mouse(&usb,0,0,0,0); assert(take(INPUT_EVENT_MOUSE_BUTTON_UP).mouse.button==0);
    input_pointer_register(&bt,"bt-1-aabbccddeeff","Bluetooth mouse");
    input_inject_mouse(&bt,0,0,1,0); assert(take(INPUT_EVENT_MOUSE_BUTTON_DOWN).mouse.button==1);
    input_pointer_unregister(&bt); assert(take(INPUT_EVENT_MOUSE_BUTTON_UP).mouse.button==1);
    // Stale, invalid, duplicated and capacity-exceeding batches are atomic.
    os64_mouse_snapshot_t before,after;
    assert(input_mouse_snapshot(&before,sizeof(before))==(int)sizeof(before));
    os64_mouse_command_t c={.version=1,.count=1,.expected_generation=before.generation-1};
    c.settings[0]=before.devices[0].setting; c.settings[0].speed=300;
    assert(input_mouse_apply(&c,sizeof(c))<0);
    c.expected_generation=before.generation; c.settings[0].speed=0;
    assert(input_mouse_apply(&c,sizeof(c))<0);
    c.settings[0].speed=200; c.count=2; c.settings[1]=c.settings[0];
    assert(input_mouse_apply(&c,sizeof(c))<0);
    c.count=16;
    for(unsigned i=0;i<c.count;i++) {
        snprintf(c.settings[i].key,64,"new-%u",i); c.settings[i].speed=100; c.settings[i].right_primary=0;
    }
    assert(input_mouse_apply(&c,sizeof(c))<0);
    input_mouse_snapshot(&after,sizeof(after)); assert(!memcmp(&before,&after,sizeof(before)));
    assert(input_mouse_apply(&c,sizeof(c)-1)<0);
    set("usb-01-p2",400,0);
    input_inject_mouse(&usb,32767,-32768,0,0); e=take(INPUT_EVENT_MOUSE_MOVE);
    assert(e.mouse.dx==32767 && e.mouse.dy==-32768 && e.mouse.x==4095 && e.mouse.y==0);
    empty();
    puts("PASS: per-device speed, fractional motion, absolute pointer isolation, held-button remapping, reconnect, CAS and atomic validation");
}
