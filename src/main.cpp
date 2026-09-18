// the-black-hole entry point: parse the screen-saver arguments and dispatch.
//
// Anything unrecognised exits 0 immediately and silently, as Windows expects of a screen saver.

#include <windows.h>
#include <shellapi.h>

#include "app/args.h"
#include "app/host.h"
#include "app/log.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    app::LogInit();

    int     argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 0;

    const app::Args args = app::Parse(argc, argv);
    LocalFree(argv);
    app::Log("argc=%d mode=%d parent=%llu", argc, static_cast<int>(args.mode), args.parentHandle);

    HWND parent = nullptr;
    if (args.hasParent) {
        parent = reinterpret_cast<HWND>(static_cast<UINT_PTR>(args.parentHandle));
        if (!IsWindow(parent)) parent = nullptr;
    }

    switch (args.mode) {
        case app::Mode::FullScreen:
            return app::RunFullScreen(instance);
        case app::Mode::Windowed:
            return app::RunWindowed(instance);
        case app::Mode::Preview:
            return parent ? app::RunPreview(instance, parent) : 0;
        case app::Mode::Configure:
            return app::RunConfigure(parent);
        case app::Mode::Exit:
        default:
            return 0;
    }
}
