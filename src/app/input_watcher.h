// Deciding when input should end the saver.
//
// The policy is kept separate from the sampling for two reasons. It is the most important logic
// in the program — a saver that cannot be dismissed leaves the user unable to get their desktop
// back — and it is the hardest to test against the real thing, because a process can only read
// keyboard and cursor state while it is on the *input* desktop. Any other process, including a
// test harness running while some other screen saver owns the input desktop, gets
// ERROR_ACCESS_DENIED from GetCursorPos and reads nothing at all from GetAsyncKeyState.
//
// So Watcher::Consider() takes a Sample and never calls Win32, which makes every rule below
// exercisable from the selftest; SampleNow() does the Win32 part and nothing else.
#ifndef BLACK_HOLE_INPUT_WATCHER_H
#define BLACK_HOLE_INPUT_WATCHER_H

#include <windows.h>

namespace app {

struct InputSample {
    bool  cursorValid = false;  // false when GetCursorPos failed (wrong desktop, etc.)
    POINT cursor{0, 0};

    // Indexed by virtual-key code. True when the key is down now, or went down since the last
    // sample — the second half matters because a keystroke can begin and end between two frames.
    bool keyDown[256] = {};
};

class InputWatcher {
public:
    // Input in the first moments after launch is fallout from starting the saver — most often
    // the key that launched it, still held — not somebody asking it to stop.
    static constexpr double kGraceSeconds = 0.5;

    // A small dead zone so that pointer jitter does not dismiss the saver.
    static constexpr int kDeadZonePixels = 8;

    // Returns true when the saver should end. Pure: no Win32, no globals, no clock.
    bool Consider(double elapsed, const InputSample& sample);

    bool primed() const { return primed_; }

private:
    bool  primed_ = false;
    POINT reference_{0, 0};
    bool  hadCursorReference_ = false;
    bool  held_[256]          = {};
};

// Reads the real keyboard and cursor. Everything it cannot read is reported as "no input"
// rather than as a reason to exit.
InputSample SampleNow();

}  // namespace app

#endif
