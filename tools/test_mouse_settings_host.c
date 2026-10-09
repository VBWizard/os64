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
    os64_mouse_command_t c={.version=OS64_MOUSE_VERSION,.count=1,.expected_generation=s_mouse_generation};
    strcpy(c.settings[0].key,key); c.settings[0].speed=speed; c.settings[0].right_primary=right;
    assert(input_mouse_apply(&c,sizeof(c))==(int)sizeof(c));
}
static void capacity_forget(void)
{
    for(unsigned i=0;i<OS64_MOUSE_DEVICES;i++) {
        s_mouse_devices[i]=(os64_mouse_device_t){0}; s_mouse_sources[i]=0;
    }
    s_mouse_generation=0; input_init();
    for(unsigned i=0;i<OS64_MOUSE_DEVICES;i++) {
        char key[32]; snprintf(key,sizeof(key),"old-%u",i); set(key,200,0);
    }
    input_pointer_source_t kept={0},shared={0},shared2={0},waiting={0};
    input_pointer_register(&kept,"old-3","Still connected");
    input_pointer_register(&shared,"old-8","Two sources");
    input_pointer_register(&shared2,"old-8","Two sources");
    input_pointer_register(&waiting,"new-mouse","New mouse");
    assert(!waiting.settings_slot);
    input_inject_mouse(&waiting,1,0,0,0); assert(take(INPUT_EVENT_MOUSE_MOVE).mouse.dx==1);
    os64_mouse_snapshot_t before,after; input_mouse_snapshot(&before,sizeof(before));
    assert(before.count==OS64_MOUSE_DEVICES);
    os64_mouse_command_t c={.version=OS64_MOUSE_VERSION,.count=1,
        .expected_generation=before.generation,.operation=OS64_MOUSE_FORGET};
    c.settings[0]=before.devices[3].setting; // Connected identities cannot be forgotten.
    assert(input_mouse_apply(&c,sizeof(c))<0);
    c.settings[0]=before.devices[0].setting; c.expected_generation--;
    assert(input_mouse_apply(&c,sizeof(c))<0); c.expected_generation++;
    c.operation=OS64_MOUSE_FORGET+1; assert(input_mouse_apply(&c,sizeof(c))<0);
    c.operation=OS64_MOUSE_FORGET; c.count=2; c.settings[1]=before.devices[1].setting;
    assert(input_mouse_apply(&c,sizeof(c))<0); c.count=1;
    c.version=1; assert(input_mouse_apply(&c,sizeof(c))<0); c.version=OS64_MOUSE_VERSION;
    input_mouse_snapshot(&after,sizeof(after)); assert(!memcmp(&before,&after,sizeof(before)));
    assert(input_mouse_apply(&c,sizeof(c))==(int)sizeof(c));
    input_mouse_snapshot(&after,sizeof(after)); assert(after.count==15 && after.generation==before.generation+1);
    assert(kept.settings_slot==4 && shared.settings_slot==9 && shared2.settings_slot==9);
    input_inject_mouse(&waiting,1,0,0,0); assert(take(INPUT_EVENT_MOUSE_MOVE).mouse.dx==1);
    assert(waiting.settings_slot==1);
    input_mouse_snapshot(&after,sizeof(after)); assert(after.count==16);
    assert(!strcmp(after.devices[0].setting.key,"new-mouse") && after.devices[0].connected);
    assert(after.devices[0].setting.speed==100 && !after.devices[0].setting.right_primary);
    set("new-mouse",300,1);
    input_inject_mouse(&waiting,1,0,0,0); assert(take(INPUT_EVENT_MOUSE_MOVE).mouse.dx==3);
    input_inject_mouse(&kept,1,0,0,0); assert(take(INPUT_EVENT_MOUSE_MOVE).mouse.dx==2);
    // One of two sources disconnecting cannot expose the other's slot to Forget.
    input_pointer_unregister(&shared);
    c.expected_generation=s_mouse_generation; c.settings[0]=before.devices[8].setting;
    assert(input_mouse_apply(&c,sizeof(c))<0);
    input_pointer_unregister(&shared2);
    assert(input_mouse_apply(&c,sizeof(c))==(int)sizeof(c));
    c.expected_generation=s_mouse_generation; c.settings[0]=before.devices[0].setting;
    assert(input_mouse_apply(&c,sizeof(c))<0); // Missing key cannot delete the replacement.
    puts("PASS: full registry Forget, protected live slots, packed snapshots and waiting mouse admission without reboot");
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
    os64_mouse_command_t c={.version=OS64_MOUSE_VERSION,.count=1,.expected_generation=before.generation-1};
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
    capacity_forget();
    puts("PASS: per-device speed, fractional motion, absolute pointer isolation, held-button remapping, reconnect, CAS and atomic validation");
}
