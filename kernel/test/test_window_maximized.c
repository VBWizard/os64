#include "gui/gui_client.h"
#include "gui/gui_internal.h"
#include "gui/window.h"
#include "printd.h"
#include "CONFIG.h"

// Exercise the client boundary and the real Restore path. The initial surface
// must already be maximized while Restore retains the requested content size.
bool test_window_maximized(void)
{
    uint64_t flags=OS64_GUI_CREATE_CONTENT_SIZE|OS64_GUI_CREATE_FIT_SCREEN|OS64_GUI_CREATE_MAXIMIZED;
    int64_t handle=gui_window_create("maximized birth",60,40,640,440,flags);
    if (handle==GUI_ERR_NOT_RUNNING) return true;
    if (handle<=0) return false;
    os64_gui_window_state_t state;
    surface_t surface;
    bool ok=gui_window_get_state(handle,&state)==0 &&
        (state.flags & OS64_GUI_WINDOW_MAXIMIZED) && state.x==0 && state.y==0 &&
        gui_window_get_surface(handle,&surface)==0 && surface.width>=640 && surface.height>=440;
    uint64_t irq=gui_lock(GUI_LOCK_OTHER);
    window_t *w=wm_focused();
    if (!w || w->client_handle!=(uint16_t)handle) ok=false;
    else {
        rect_t normal=w->restoreFrame;
        wm_set_maximized(w,false);
        ok=ok && !(w->flags & GUI_WINDOW_MAXIMIZED) && w->frame.x==normal.x &&
            w->frame.y==normal.y && w->frame.w==normal.w && w->frame.h==normal.h &&
            w->content.width==640 && w->content.height==440;
    }
    gui_unlock(irq);
    // Maximized birth must retain the focus decision made at creation.
    // Keep the restored window as the existing keyboard focus owner.
    int64_t unfocused=gui_window_create("unfocused maximized birth",80,60,640,440,
        flags|OS64_GUI_WINDOW_START_UNFOCUSED);
    if (unfocused<=0) ok=false;
    else {
        bool birth_ok=gui_window_get_state(unfocused,&state)==0 &&
            (state.flags & OS64_GUI_WINDOW_MAXIMIZED) && state.x==0 && state.y==0 &&
            gui_window_get_surface(unfocused,&surface)==0 &&
            surface.width>=640 && surface.height>=440;
        irq=gui_lock(GUI_LOCK_OTHER);
        window_t *focused=wm_focused();
        birth_ok=birth_ok && focused && focused->client_handle==(uint16_t)handle;
        // The new ordinary window was linked above the existing one.
        window_t *born=focused;
        while (born && born->client_handle!=(uint16_t)unfocused) born=born->above;
        bool interactive_ok=false;
        if (born) {
            rect_t normal=born->restoreFrame;
            wm_set_maximized(born,false);
            interactive_ok=!(born->flags & GUI_WINDOW_MAXIMIZED) &&
                born->frame.x==normal.x && born->frame.y==normal.y &&
                born->frame.w==normal.w && born->frame.h==normal.h && wm_focused()==focused;
            wm_set_maximized(born,true);
            interactive_ok=interactive_ok && (born->flags & GUI_WINDOW_MAXIMIZED) &&
                wm_focused()==born;
        }
        gui_unlock(irq);
        if (!birth_ok)
            printd(DEBUG_TESTS,"\tFAIL: maximized birth did not preserve START_UNFOCUSED\n");
        if (!interactive_ok)
            printd(DEBUG_TESTS,"\tFAIL: interactive maximize/Restore after unfocused birth\n");
        ok=birth_ok && interactive_ok && ok;
        ok=gui_window_destroy(unfocused)==0 && ok;
    }
    ok=gui_window_destroy(handle)==0 && ok;
    // A popup or desktop cannot acquire ordinary maximization semantics.
    uint64_t invalid[]={OS64_GUI_WINDOW_DESKTOP,
        OS64_GUI_WINDOW_NO_DECORATIONS|OS64_GUI_WINDOW_PINNED,
        OS64_GUI_CREATE_POPUP_CAPACITY|OS64_GUI_WINDOW_NO_DECORATIONS|OS64_GUI_WINDOW_PINNED};
    for (unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        handle=gui_window_create("invalid maximized birth",60,40,640,440,flags|invalid[i]);
        ok=handle==GUI_ERR_BAD_ARGS && ok;
        if (handle>0) (void)gui_window_destroy(handle);
    }
    printd(DEBUG_TESTS,"\t%s: test_window_maximized\n",ok ? "PASS" : "FAIL");
    return ok;
}
