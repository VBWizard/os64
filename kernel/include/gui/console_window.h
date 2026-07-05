#ifndef GUI_CONSOLE_WINDOW_H
#define GUI_CONSOLE_WINDOW_H

// The GUI console window: a text-grid terminal that receives everything the
// kernel prints via print_n()/printf once the desktop owns the screen.
//
// Design constraint that shapes everything here: the sink runs in the
// CALLER's context — any thread, any core, possibly inside an exception
// handler. So the sink only appends characters to a grid under its own tiny
// lock and NEVER touches kGuiLock or draws pixels. The compositor calls
// gui_console_render_if_dirty() each frame to turn the grid into pixels.

// Create the console window and attach print_n's kConsoleSink to it.
// Called by the compositor thread during GUI startup.
void gui_console_start(void);

// Compositor-only, once per frame, WITHOUT kGuiLock held: if the grid
// changed, redraw it into the window surface and present.
void gui_console_render_if_dirty(void);

#endif // GUI_CONSOLE_WINDOW_H
