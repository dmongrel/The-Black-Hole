#include "app/credits.h"

#include <windows.h>

#include "app/log.h"
#include "app/resource.h"

#include <cstdio>
#include <cwctype>

namespace app {
namespace {

std::wstring Widen(const char* data, size_t size) {
    if (size >= 3 && static_cast<unsigned char>(data[0]) == 0xEF && static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        data += 3;  // a byte-order mark, as Notepad may write
        size -= 3;
    }
    if (size == 0) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(size), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(size), out.data(), n);
    return out;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                      nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// Line ends as an edit control wants them, whatever the file had.
std::wstring ToCrLf(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size() + text.size() / 16);
    for (wchar_t c : text) {
        if (c == L'\r') continue;
        if (c == L'\n') out += L'\r';
        out += c;
    }
    return out;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::iswspace(s[a])) ++a;
    while (b > a && std::iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool ReadFile(const std::wstring& path, std::wstring& text) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::string bytes;
    char        buffer[4096];
    size_t      n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) bytes.append(buffer, n);
    std::fclose(f);
    text = Widen(bytes.data(), bytes.size());
    return true;
}

}  // namespace

std::wstring CreditsPath() {
    if (const wchar_t* p = _wgetenv(L"BLACK_HOLE_CREDITS")) return p;
    wchar_t appData[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    return std::wstring(appData) + L"\\The-Black-Hole\\credits.txt";
}

std::wstring DefaultCreditsText() {
    HRSRC   res  = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_DEFAULT_CREDITS), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    if (!data) return {};
    return ToCrLf(Widen(static_cast<const char*>(LockResource(data)), SizeofResource(nullptr, res)));
}

std::wstring FontLicenseText() {
    HRSRC   res  = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_FONT_LICENSE), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    if (!data) return {};
    return ToCrLf(Widen(static_cast<const char*>(LockResource(data)), SizeofResource(nullptr, res)));
}

std::wstring CreditsText() {
    std::wstring text;
    const std::wstring path = CreditsPath();
    if (!path.empty() && ReadFile(path, text)) return ToCrLf(text);
    return DefaultCreditsText();
}

std::vector<Credit> ParseCredits(const std::wstring& text) {
    std::vector<Credit> credits;
    size_t              start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        const std::wstring line = Trim(text.substr(start, end - start));
        start                   = end + 1;
        if (line.empty()) continue;

        Credit       c;
        const size_t colon = line.find(L':');
        if (colon == std::wstring::npos) {
            c.first = line;
        } else {
            c.first  = Trim(line.substr(0, colon));
            c.second = Trim(line.substr(colon + 1));
            // "Name:" or ": Name" is a single line, not a blank one and a name.
            if (c.first.empty()) std::swap(c.first, c.second);
        }
        if (!c.first.empty()) credits.push_back(std::move(c));
    }
    return credits;
}

std::vector<Credit> LoadCredits() {
    std::vector<Credit> credits = ParseCredits(CreditsText());
    Log("credits: %u from %s", static_cast<unsigned>(credits.size()), Narrow(CreditsPath()).c_str());
    return credits;
}

bool SaveCreditsText(const std::wstring& text) {
    const std::wstring path = CreditsPath();
    if (path.empty()) return false;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);

    std::wstring lf;
    for (wchar_t c : text) {
        if (c != L'\r') lf += c;
    }
    if (!lf.empty() && lf.back() != L'\n') lf += L'\n';
    const std::string bytes = Narrow(lf);

    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok;
}

}  // namespace app
