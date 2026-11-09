#!/usr/bin/env python3
"""Exercise the production syscall guard, ownership gate and WM pin mutation."""
from pathlib import Path
import argparse,os,re,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--build-only',action='store_true');p.add_argument('--output',type=Path);a=p.parse_args()
out=a.output or Path(tempfile.mkdtemp(prefix='os64-pin-'));out.mkdir(parents=True,exist_ok=True)
def function(path,name):
    source=(root/path).read_text()
    match=re.search(r'(?:static )?(?:uint64_t|int64_t|void) '+name+r'\([^;]+?\)\s*\{',source)
    assert match,name
    end,depth=match.end(),1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[match.start():end]
header=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "os64/gui.h"
#include "os64/decoration.h"
#define GUI_ERR_BAD_ARGS OS64_GUI_ERR_BAD_ARGS
#define GUI_ERR_INVALID_HANDLE OS64_GUI_ERR_INVALID_HANDLE
#define GUI_ERR_NOT_OWNER OS64_GUI_ERR_NOT_OWNER
#define GUI_WINDOW_PINNED OS64_GUI_WINDOW_PINNED
#define GUI_WINDOW_DESKTOP OS64_GUI_WINDOW_DESKTOP
#define GUI_WINDOW_HAS_SETTINGS OS64_GUI_WINDOW_HAS_SETTINGS
#define GUI_WINDOW_MAXIMIZED OS64_GUI_WINDOW_MAXIMIZED
#define GUI_WINDOW_POPUP (1u<<24)
#define GUI_LOCK_WINDOW 1
#define INPUT_EVENT_WINDOW_CLOSE OS64_GUI_EVENT_WINDOW_CLOSE
#define INPUT_EVENT_SETTINGS OS64_GUI_EVENT_SETTINGS
#define INPUT_EVENT_WINDOW_PIN_CHANGED OS64_GUI_EVENT_WINDOW_PIN_CHANGED
static uint64_t kTicksSinceStart=42;
typedef struct { unsigned flags;os64_gui_rect_t frame; } window_t;
typedef struct { unsigned type;uint64_t tick; } input_event_t;
static window_t window;
static bool owned=true,locked;
static unsigned mutations,notifications,damages;
static uint64_t gui_lock(unsigned site){assert(site==GUI_LOCK_WINDOW && !locked);locked=true;return 7;}
static void gui_unlock(uint64_t token){assert(locked && token==7);locked=false;}
static window_t *handle_lookup_owned(int64_t h,int64_t *err)
{
    assert(locked);
    if(h!=1){*err=GUI_ERR_INVALID_HANDLE;return NULL;}
    if(!owned){*err=GUI_ERR_NOT_OWNER;return NULL;}
    return &window;
}
static bool wm_is_hidden(const window_t *w){(void)w;return false;}
static void wm_set_minimized(window_t *w,bool value){(void)w;(void)value;assert(0);}
static void wm_set_maximized(window_t *w,bool value){(void)w;(void)value;assert(0);}
static bool desktop_declines(window_t *w,const char *verb){(void)verb;return (w->flags&GUI_WINDOW_DESKTOP)!=0;}
static void unlink_window(window_t *w){assert(locked && w==&window);++mutations;}
static void link_on_top(window_t *w){assert(locked && w==&window);++mutations;}
static void gui_damage_add_locked(os64_gui_rect_t r){(void)r;assert(locked);++damages;}
static void wm_deliver_event(window_t *w,const input_event_t *ev)
{assert(locked && w==&window && ev->type==INPUT_EVENT_WINDOW_PIN_CHANGED && ev->tick==42);++notifications;}
'''
body=r'''
int main(void)
{
    assert(SYSCALL_GUI_WINDOW_SET_PINNED==66);
    assert(!syscall_gui_window_set_pinned(1,1,0,0,0,0));
    assert(window.flags&GUI_WINDOW_PINNED);assert(mutations==2 && notifications==1 && damages==1 && !locked);
    assert(!gui_window_set_pinned(1,1));assert(notifications==1);
    assert(!gui_window_set_pinned(1,0));assert(!window.flags && notifications==2 && mutations==4);
    // The frame action uses this same WM mutation, and emits the same hint.
    uint64_t token=gui_lock(GUI_LOCK_WINDOW);wm_decoration_action(&window,OS64_DECOR_PIN,kTicksSinceStart);gui_unlock(token);
    assert(window.flags&GUI_WINDOW_PINNED);assert(notifications==3);
    unsigned before=notifications;
    assert((int64_t)syscall_gui_window_set_pinned(1,UINT64_C(1)<<32,0,0,0,0)==GUI_ERR_BAD_ARGS);
    assert(gui_window_set_pinned(1,2)==GUI_ERR_BAD_ARGS);
    assert(gui_window_set_pinned(2,0)==GUI_ERR_INVALID_HANDLE);
    owned=false;assert(gui_window_set_pinned(1,0)==GUI_ERR_NOT_OWNER);owned=true;
    window.flags=GUI_WINDOW_DESKTOP;assert(gui_window_set_pinned(1,1)==GUI_ERR_BAD_ARGS);
    window.flags=GUI_WINDOW_POPUP|GUI_WINDOW_PINNED;assert(gui_window_set_pinned(1,0)==GUI_ERR_BAD_ARGS);
    assert(window.flags==(GUI_WINDOW_POPUP|GUI_WINDOW_PINNED));assert(notifications==before && !locked);
    puts("window pin: raw arguments, ownership, ordinary-window policy, shared WM mutation and hints PASS");
}
'''
source=header+'\n'+function('kernel/src/gui/window.c','wm_set_pinned')+'\n'+function('kernel/src/gui/window.c','wm_decoration_action')+'\n'+function('kernel/src/gui/gui_client.c','gui_window_set_pinned')+'\n'+function('kernel/src/syscall.c','syscall_gui_window_set_pinned')+'\n'+body
(out/'pin.c').write_text(source)
subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(root/'abi/include'),str(out/'pin.c'),'-o',str(out/'test_pin')],check=True)
subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all',
    '-I'+str(root/'kernel/include'),'-I'+str(root/'abi/include'),str(root/'tools/test_gui_queue_host.c'),
    str(root/'kernel/src/gui/event_queue.c'),'-o',str(out/'test_queue')],check=True)
if not a.build_only:
    for binary in ['test_pin','test_queue']:
        subprocess.run([str(out/binary)],check=True,env={**os.environ,'ASAN_OPTIONS':os.environ.get('ASAN_OPTIONS','detect_leaks=1')})
print('Artifacts:',out)
