#include "app/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace app {
namespace {

FILE* g_file = nullptr;

}  // namespace

void LogInit() {
    const char* value = std::getenv("BLACK_HOLE_LOG");
    if (!value || !*value) return;

    char path[MAX_PATH * 2];
    if (std::strcmp(value, "1") == 0) {
        char  temp[MAX_PATH];
        DWORD n = GetTempPathA(MAX_PATH, temp);
        if (n == 0 || n >= MAX_PATH) return;
        std::snprintf(path, sizeof(path), "%sthe-black-hole.log", temp);
    } else {
        std::snprintf(path, sizeof(path), "%s", value);
    }

    g_file = std::fopen(path, "w");
    if (g_file) Log("the-black-hole log opened");
}

bool LogEnabled() { return g_file != nullptr; }

void Log(const char* fmt, ...) {
    if (!g_file) return;

    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const double t = freq.QuadPart ? static_cast<double>(now.QuadPart) / freq.QuadPart : 0.0;

    std::fprintf(g_file, "[%10.3f] ", t);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_file, fmt, args);
    va_end(args);
    std::fputc('\n', g_file);
    std::fflush(g_file);
}

}  // namespace app
