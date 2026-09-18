#include "app/args.h"

#include <cwchar>
#include <cwctype>

namespace app {

Args Parse(int argc, const wchar_t* const* argv) {
    Args a;

    if (argc <= 1 || !argv || !argv[1]) return a;  // no arguments: full screen

    const wchar_t* arg = argv[1];
    if (*arg == L'/' || *arg == L'-') ++arg;

    const wchar_t lower = static_cast<wchar_t>(std::towlower(*arg));

    if (lower == L's') {
        a.mode = Mode::FullScreen;
        return a;
    }

    if (lower == L'w') {
        a.mode = Mode::Windowed;
        return a;
    }

    if (lower == L'p') {
        a.mode = Mode::Preview;
        ++arg;
        if (*arg == L':') {
            ++arg;
        } else {
            // The handle is the next argument instead. Without one there is nothing to draw in.
            if (argc < 3 || !argv[2]) {
                a.mode = Mode::Exit;
                return a;
            }
            arg = argv[2];
        }
        if (!*arg) {
            a.mode = Mode::Exit;
            return a;
        }
        a.parentHandle = std::wcstoull(arg, nullptr, 10);
        a.hasParent    = true;
        if (a.parentHandle == 0) a.mode = Mode::Exit;
        return a;
    }

    if (lower == L'c') {
        a.mode = Mode::Configure;
        ++arg;
        if (*arg == L':') {
            ++arg;
        } else {
            arg = (argc < 3 || !argv[2]) ? nullptr : argv[2];
        }
        if (arg && *arg) {
            a.parentHandle = std::wcstoull(arg, nullptr, 10);
            a.hasParent    = a.parentHandle != 0;
        }
        return a;
    }

    a.mode = Mode::Exit;
    return a;
}

}  // namespace app
