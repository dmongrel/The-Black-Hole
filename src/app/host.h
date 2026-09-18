// The screen-saver host: the windows, the message loop, and the rules for when input ends a run.
#ifndef BLACK_HOLE_HOST_H
#define BLACK_HOLE_HOST_H

#include <windows.h>

namespace app {

// /s or no arguments: one borderless top-most window per monitor, dismissed by any input.
int RunFullScreen(HINSTANCE instance);

// /w: a normal resizable window for development. Esc or closing it ends the run.
int RunWindowed(HINSTANCE instance);

// /p <hwnd>: a black child of the Screen Saver Settings preview pane, gone when the pane is.
int RunPreview(HINSTANCE instance, HWND parent);

// /c: there are no settings yet, so this says so.
int RunConfigure(HWND owner);

}  // namespace app

#endif
