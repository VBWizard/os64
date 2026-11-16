#!/usr/bin/env python3
"""Exercise the production exact-size syscall and its WM resize engine."""
import argparse,os,re,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--build-only',action='store_true');p.add_argument('--output',type=Path);args=p.parse_args()
out=args.output or Path(tempfile.mkdtemp(prefix='os64-window-size-'));out.mkdir(parents=True,exist_ok=True)
def function(path,name):
    source=(root/path).read_text();m=re.search(r'(?:static )?(?:uint64_t|int64_t|bool|rect_t) '+name+r'\([^;]+?\)\s*\{',source);assert m,name
    end=m.end();depth=1
    while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[m.start():end]
header=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "os64/gui.h"
#define GUI_ERR_BAD_ARGS OS64_GUI_ERR_BAD_ARGS
#define GUI_ERR_INVALID_HANDLE OS64_GUI_ERR_INVALID_HANDLE
#define GUI_ERR_NOT_OWNER OS64_GUI_ERR_NOT_OWNER
#define GUI_ERR_BELOW_MINIMUM OS64_GUI_ERR_BELOW_MINIMUM
#define GUI_ERR_ABOVE_CAPACITY OS64_GUI_ERR_ABOVE_CAPACITY
#define GUI_ERR_SCREEN_FIT OS64_GUI_ERR_SCREEN_FIT
#define GUI_ERR_NO_RESOURCES OS64_GUI_ERR_NO_RESOURCES
#define GUI_WINDOW_DESKTOP OS64_GUI_WINDOW_DESKTOP
#define GUI_WINDOW_MAXIMIZED OS64_GUI_WINDOW_MAXIMIZED
#define GUI_WINDOW_POPUP (1u<<24)
#define GUI_LOCK_WINDOW 1
#define INPUT_EVENT_WINDOW_RESIZE OS64_GUI_EVENT_WINDOW_RESIZE
#define DEBUG_GUI 0
#define WINDOW_CONTENT_INITIAL 0
#define printd(...) ((void)0)
typedef os64_gui_rect_t rect_t;
typedef os64_gui_surface_t surface_t;
typedef os64_gui_event_t input_event_t;
typedef struct {unsigned flags,id;rect_t frame,restoreFrame;uint32_t min_content_w,min_content_h,canvas_cap_w,canvas_cap_h;surface_t content,canvas;} window_t;
static struct {uint32_t width,height;} kFrameBuffer={800,600};
static uint64_t kTicksSinceStart;
static window_t window;
static bool owned=true,locked,invariant_failure;
static unsigned notifications,cancellations;
static int border=1,top=20,decor_width=80;
static int wm_border_width(unsigned f){(void)f;return border;}
static int wm_chrome_top(unsigned f){(void)f;return top;}
static uint32_t wm_decoration_min_width(unsigned f){(void)f;return (uint32_t)decor_width;}
static uint64_t gui_lock(unsigned site){assert(site==1 && !locked);locked=true;return 7;}
static void gui_unlock(uint64_t token){assert(locked && token==7);locked=false;}
static window_t *handle_lookup_owned(int64_t h,int64_t *err)
{assert(locked);if(h!=1){*err=GUI_ERR_INVALID_HANDLE;return NULL;}if(!owned){*err=GUI_ERR_NOT_OWNER;return NULL;}return &window;}
static void gui_geometry_changing(window_t *w){assert(locked && w==&window);}
static void gui_cancel_resize(window_t *w){assert(locked && w==&window);++cancellations;}
static void gui_cancel_decoration(window_t *w){assert(locked && w==&window);}
static rect_t rect_union(rect_t a,rect_t b){(void)b;return a;}
static bool surface_set_size(surface_t *s,uint32_t w,uint32_t h,uint32_t cw,uint32_t ch)
{if(invariant_failure || w>cw || h>ch)return false;s->width=w;s->height=h;return true;}
static void surface_fill_rect(surface_t *s,rect_t r,unsigned c){(void)s;(void)r;(void)c;}
static void gui_damage_add_locked(rect_t r){(void)r;assert(locked);}
static void wm_deliver_event(window_t *w,const input_event_t *e)
{assert(locked && w==&window && e->type==INPUT_EVENT_WINDOW_RESIZE);++notifications;}
'''
body=r'''
static void reset(void)
{window=(window_t){.frame={40,50,202,181},.min_content_w=100,.min_content_h=80,.canvas_cap_w=800,.canvas_cap_h=600,
.content={.width=200,.height=160},.canvas={.width=200,.height=160}};notifications=cancellations=0;}
static void refused(uint64_t w,uint64_t h,int64_t error)
{window_t before=window;unsigned n=cancellations;assert((int64_t)syscall_gui_window_set_content_size(1,w,h,0,0,0)==error);
assert(!memcmp(&before,&window,sizeof(window)) && cancellations==n && !locked);}
int main(void)
{
 assert(SYSCALL_GUI_WINDOW_SET_CONTENT_SIZE==68);reset();
 refused(0,100,GUI_ERR_BAD_ARGS);refused(UINT64_C(1)<<32,100,GUI_ERR_BAD_ARGS);
 refused(99,100,GUI_ERR_BELOW_MINIMUM);refused(100,79,GUI_ERR_BELOW_MINIMUM);
 refused(801,100,GUI_ERR_ABOVE_CAPACITY);refused(800,100,GUI_ERR_SCREEN_FIT);
 assert(!gui_window_set_content_size(1,200,160) && !notifications);
 assert(!gui_window_set_content_size(1,340,220));assert(window.frame.x==40 && window.frame.y==50 && window.canvas.width==340 && notifications==1);
 window.frame.x=790;window.frame.y=590;assert(!gui_window_set_content_size(1,200,160));assert(window.frame.x==598 && window.frame.y==419);
 window.frame.x=-20;window.frame.y=-10;assert(!gui_window_set_content_size(1,220,180));assert(!window.frame.x && !window.frame.y);
 unsigned n=notifications;window.flags=GUI_WINDOW_MAXIMIZED|OS64_GUI_WINDOW_PINNED|OS64_GUI_WINDOW_MINIMIZED;
 window.restoreFrame=(rect_t){750,550,202,181};rect_t visible=window.frame;
 assert(!gui_window_set_content_size(1,300,200));assert(!memcmp(&visible,&window.frame,sizeof(visible)) && notifications==n);
 assert(window.restoreFrame.x==498 && window.restoreFrame.y==379 && window.restoreFrame.w==302 && window.restoreFrame.h==221);
 assert(window.flags==(GUI_WINDOW_MAXIMIZED|OS64_GUI_WINDOW_PINNED|OS64_GUI_WINDOW_MINIMIZED));
 reset();owned=false;refused(200,100,GUI_ERR_NOT_OWNER);owned=true;
 assert(gui_window_set_content_size(2,200,100)==GUI_ERR_INVALID_HANDLE);
 window.flags=GUI_WINDOW_POPUP;refused(200,100,GUI_ERR_BAD_ARGS);window.flags=GUI_WINDOW_DESKTOP;refused(200,100,GUI_ERR_BAD_ARGS);
 reset();decor_width=300;refused(200,100,GUI_ERR_BELOW_MINIMUM);decor_width=80;
 border=4;top=36;assert(!gui_window_set_content_size(1,250,180));assert(window.frame.w==258 && window.frame.h==220);
 reset();border=1;top=20;invariant_failure=true;assert(gui_window_set_content_size(1,300,200)==GUI_ERR_NO_RESOURCES);assert(window.canvas.width==200 && window.content.width==200);
 puts("window content size: raw arguments, owner/policy gates, bounds, anchoring, no-op, maximize, chrome, invariant refusal PASS");
}
'''
src=header+'\n'+function('kernel/src/gui/window.c','wm_clamp_frame')+'\n'+function('kernel/src/gui/window.c','wm_resize')+'\n'+function('kernel/src/gui/gui_client.c','gui_window_set_content_size')+'\n'+function('kernel/src/syscall.c','syscall_gui_window_set_content_size')+'\n'+body
(out/'size.c').write_text(src)
subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(root/'abi/include'),str(out/'size.c'),'-o',str(out/'test_size')],check=True)
if not args.build_only:subprocess.run([str(out/'test_size')],check=True,env={**os.environ,'ASAN_OPTIONS':os.environ.get('ASAN_OPTIONS','detect_leaks=1')})
print('Artifacts:',out)
