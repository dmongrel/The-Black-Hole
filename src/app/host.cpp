#include "app/host.h"

#include <windowsx.h>

#include "app/input_watcher.h"
#include "app/log.h"
#include "render/renderer.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace app {
namespace {

const wchar_t* kFullScreenClass = L"TheBlackHoleFullScreen";
const wchar_t* kWindowedClass   = L"TheBlackHoleWindowed";
const wchar_t* kPreviewClass    = L"TheBlackHolePreview";

struct HostState {
    std::unique_ptr<render::Renderer> renderer;
    std::vector<HWND>                 windows;
    InputWatcher                      input;

    bool   exitOnInput    = false;
    bool   running        = true;
    bool   sawFirstMove   = false;
    double elapsed        = 0.0;
    POINT  referenceMouse = {0, 0};
};

HostState* g_host = nullptr;

void RequestExit() {
    if (!g_host) return;
    g_host->running = false;
    PostQuitMessage(0);
}

// Input is watched twice in full-screen mode: here, and by polling each frame (see
// app/input_watcher.h). The window procedure only sees input while one of our windows has focus.
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    const bool saver = g_host && g_host->exitOnInput;
    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (saver) {
                if (g_host->elapsed >= InputWatcher::kGraceSeconds) RequestExit();
                return 0;
            }
            if (wParam == VK_ESCAPE) RequestExit();
            break;

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
            if (saver && g_host->elapsed >= InputWatcher::kGraceSeconds) RequestExit();
            return 0;

        case WM_MOUSEMOVE: {
            if (!saver) break;
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd, &p);
            if (!g_host->sawFirstMove) {
                g_host->sawFirstMove   = true;
                g_host->referenceMouse = p;
                return 0;
            }
            if (g_host->elapsed < InputWatcher::kGraceSeconds) return 0;
            const int dx = p.x - g_host->referenceMouse.x;
            const int dy = p.y - g_host->referenceMouse.y;
            const int dz = InputWatcher::kDeadZonePixels;
            if (dx * dx + dy * dy > dz * dz) RequestExit();
            return 0;
        }

        case WM_SETCURSOR:
            if (saver) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;

        case WM_ERASEBKGND:
            return 1;  // the renderer owns every pixel; never let GDI flash the background

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC         dc = BeginPaint(hwnd, &ps);
            if (!g_host || !g_host->renderer) {
                FillRect(dc, &ps.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SIZE:
            if (g_host && g_host->renderer) g_host->renderer->ResizeWindow(hwnd);
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (g_host) {
                if (g_host->renderer) g_host->renderer->DetachWindow(hwnd);
                std::erase(g_host->windows, hwnd);
                if (g_host->windows.empty()) RequestExit();
            }
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Windows refuses SetForegroundWindow to a process that is not already in the foreground, which
// happens when the saver is launched by hand or from the settings dialog's Preview button.
// Without focus WM_KEYDOWN never arrives. Attaching to the foreground thread's input queue lifts
// the restriction for the duration of the call.
void ForceForeground(HWND hwnd) {
    const DWORD foreignThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD ownThread     = GetCurrentThreadId();
    const bool  attach        = foreignThread != 0 && foreignThread != ownThread;

    if (attach) AttachThreadInput(foreignThread, ownThread, TRUE);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    if (attach) AttachThreadInput(foreignThread, ownThread, FALSE);
}

BOOL CALLBACK MonitorProc(HMONITOR, HDC, LPRECT rect, LPARAM lParam) {
    reinterpret_cast<std::vector<RECT>*>(lParam)->push_back(*rect);
    return TRUE;
}

std::vector<RECT> EnumerateMonitors() {
    std::vector<RECT> rects;
    EnumDisplayMonitors(nullptr, nullptr, MonitorProc, reinterpret_cast<LPARAM>(&rects));
    if (rects.empty()) {
        RECT r{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
        r.right  = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        rects.push_back(r);
    }
    return rects;
}

bool RegisterClassOnce(HINSTANCE instance, const wchar_t* name, WNDPROC proc) {
    WNDCLASSW wc{};
    wc.style         = CS_OWNDC;
    wc.lpfnWndProc   = proc;
    wc.hInstance     = instance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = name;
    return RegisterClassW(&wc) != 0;
}

// BLACK_HOLE_CAPTURE=<file.bmp> writes the frame shown at BLACK_HOLE_CAPTURE_AT seconds (default
// 4) from the first window and then ends the run. BLACK_HOLE_TIME adds an offset to the
// animation clock so a capture can land anywhere in the camera's orbit.
struct CaptureRequest {
    std::string path;
    double      at = 4.0;
};

CaptureRequest CaptureFromEnvironment() {
    CaptureRequest c;
    if (const char* p = std::getenv("BLACK_HOLE_CAPTURE")) c.path = p;
    if (const char* at = std::getenv("BLACK_HOLE_CAPTURE_AT")) c.at = std::atof(at);
    return c;
}

double TimeOffsetFromEnvironment() {
    const char* t = std::getenv("BLACK_HOLE_TIME");
    return t ? std::atof(t) : 0.0;
}

// Shared by full-screen and windowed runs.
void RenderLoop(HostState& host) {
    LARGE_INTEGER freq{}, start{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    const CaptureRequest capture   = CaptureFromEnvironment();
    const double         offset    = TimeOffsetFromEnvironment();
    bool                 requested = false;

    unsigned long long frame = 0;
    MSG                msg{};
    while (host.running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                host.running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!host.running) break;

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        host.elapsed = static_cast<double>(now.QuadPart - start.QuadPart) /
                       static_cast<double>(freq.QuadPart);

        if (host.exitOnInput && host.input.Consider(host.elapsed, SampleNow())) {
            Log("input ended the run at t=%.3f", host.elapsed);
            break;
        }

        if (!capture.path.empty() && host.renderer && !host.windows.empty()) {
            if (!requested && host.elapsed >= capture.at) {
                host.renderer->RequestCapture(host.windows.front(), capture.path);
                requested = true;
            } else if (requested && !host.renderer->CapturePending()) {
                Log("capture written to %s", capture.path.c_str());
                break;
            }
        }

        if (host.renderer) {
            host.renderer->RenderFrame(host.elapsed + offset);  // FIFO present paces the loop
        } else {
            Sleep(16);
        }
        ++frame;
    }
    Log("loop exited after %llu frames (%.1f fps)", frame,
        host.elapsed > 0.0 ? static_cast<double>(frame) / host.elapsed : 0.0);
}

void Teardown(HostState& host, HINSTANCE instance, const wchar_t* className) {
    if (host.renderer) host.renderer->WaitIdle();
    for (HWND hwnd : std::vector<HWND>(host.windows)) DestroyWindow(hwnd);
    host.renderer.reset();
    UnregisterClassW(className, instance);
    g_host = nullptr;
}

}  // namespace

int RunFullScreen(HINSTANCE instance) {
    HostState host;
    host.exitOnInput = true;
    g_host           = &host;

    if (!RegisterClassOnce(instance, kFullScreenClass, WndProc)) {
        g_host = nullptr;
        return 0;
    }

    host.renderer = render::Renderer::Create();
    Log("renderer: %s", host.renderer ? "vulkan" : "none (black screen)");

    for (const RECT& r : EnumerateMonitors()) {
        HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, kFullScreenClass, L"The Black Hole", WS_POPUP,
                                    r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr,
                                    nullptr, instance, nullptr);
        if (!hwnd) continue;
        // Shown before attaching: an unshown window has no client area, and the surface would
        // report a zero extent.
        ShowWindow(hwnd, SW_SHOW);
        host.windows.push_back(hwnd);
        if (host.renderer && !host.renderer->AttachWindow(hwnd)) {
            Log("window %p could not be attached; it stays black", static_cast<void*>(hwnd));
        }
    }

    if (!host.windows.empty()) {
        ForceForeground(host.windows.front());
        // A pointer that wanders onto a second monitor's window must still end the run.
        SetCapture(host.windows.front());
        ShowCursor(FALSE);
        RenderLoop(host);
        ReleaseCapture();
        ShowCursor(TRUE);
    }

    Teardown(host, instance, kFullScreenClass);
    return 0;
}

int RunWindowed(HINSTANCE instance) {
    HostState host;
    g_host = &host;

    if (!RegisterClassOnce(instance, kWindowedClass, WndProc)) {
        g_host = nullptr;
        return 0;
    }

    host.renderer = render::Renderer::Create();
    Log("renderer: %s", host.renderer ? "vulkan" : "none (black screen)");

    RECT r{0, 0, 1600, 900};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, kWindowedClass, L"The Black Hole", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, instance, nullptr);
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOW);
        host.windows.push_back(hwnd);
        if (host.renderer) host.renderer->AttachWindow(hwnd);
        RenderLoop(host);
    }

    Teardown(host, instance, kWindowedClass);
    return 0;
}

namespace {

constexpr UINT_PTR kParentWatchTimer = 1;

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TIMER:
            // The parent belongs to another process's dialog and can vanish without notice.
            if (wParam == kParentWatchTimer && !IsWindow(GetParent(hwnd))) DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace

int RunPreview(HINSTANCE instance, HWND parent) {
    if (!RegisterClassOnce(instance, kPreviewClass, PreviewProc)) return 0;

    RECT rc{};
    GetClientRect(parent, &rc);
    HWND hwnd = CreateWindowExW(0, kPreviewClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, rc.right,
                                rc.bottom, parent, nullptr, instance, nullptr);
    if (hwnd) {
        SetTimer(hwnd, kParentWatchTimer, 250, nullptr);
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    UnregisterClassW(kPreviewClass, instance);
    return 0;
}

int RunConfigure(HWND owner) {
    MessageBoxW(owner, L"The Black Hole has no settings yet.", L"The Black Hole",
                MB_OK | MB_ICONINFORMATION);
    return 0;
}

}  // namespace app
