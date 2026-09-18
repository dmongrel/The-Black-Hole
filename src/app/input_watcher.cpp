#include "app/input_watcher.h"

namespace app {

bool InputWatcher::Consider(double elapsed, const InputSample& sample) {
    if (elapsed < kGraceSeconds) return false;

    if (!primed_) {
        // First look after the grace period: record the world as it stands. Nothing that is
        // already true counts as input.
        if (sample.cursorValid) {
            reference_          = sample.cursor;
            hadCursorReference_ = true;
        }
        for (int vk = 0; vk < 256; ++vk) held_[vk] = sample.keyDown[vk];
        primed_ = true;
        return false;
    }

    if (sample.cursorValid) {
        if (!hadCursorReference_) {
            // The cursor only became readable now — for instance because the saver has just
            // been given the input desktop. Treat this as the reference rather than as a jump.
            reference_          = sample.cursor;
            hadCursorReference_ = true;
        } else {
            const int dx = sample.cursor.x - reference_.x;
            const int dy = sample.cursor.y - reference_.y;
            if (dx * dx + dy * dy > kDeadZonePixels * kDeadZonePixels) return true;
        }
    }

    for (int vk = 0; vk < 256; ++vk) {
        if (sample.keyDown[vk]) {
            if (!held_[vk]) return true;  // a press that was not already happening
        } else {
            held_[vk] = false;  // released, so the next press is a real one
        }
    }

    return false;
}

InputSample SampleNow() {
    InputSample s;

    // GetCursorPos fails with ERROR_ACCESS_DENIED when this thread is not on the input desktop.
    // That is not input, and it is not an error worth acting on either — just an unknown.
    s.cursorValid = GetCursorPos(&s.cursor) != FALSE;

    for (int vk = 1; vk < 256; ++vk) {
        // 0x8000 is "down now"; 0x0001 is "went down since this thread last asked", which is
        // what catches a keystroke that began and ended between two frames. Reading also clears
        // the low bit, so each transition is reported exactly once.
        const SHORT state = GetAsyncKeyState(vk);
        s.keyDown[vk]     = (state & 0x8000) != 0 || (state & 0x0001) != 0;
    }

    return s;
}

}  // namespace app
