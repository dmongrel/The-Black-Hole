// /c: the Settings dialog. For now it holds the credits list.

#include "app/credits.h"
#include "app/host.h"
#include "app/resource.h"

#include <string>

namespace app {
namespace {

std::wstring EditText(HWND edit) {
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(edit)) + 1, L'\0');
    const int    n = GetWindowTextW(edit, text.data(), static_cast<int>(text.size()));
    text.resize(static_cast<size_t>(n));
    return text;
}

INT_PTR CALLBACK SettingsProc(HWND dialog, UINT msg, WPARAM wParam, LPARAM) {
    switch (msg) {
        case WM_INITDIALOG: {
            HWND edit = GetDlgItem(dialog, IDC_CREDITS);
            SendMessageW(edit, EM_SETLIMITTEXT, 0, 0);  // the default 32K would be arbitrary
            SetWindowTextW(edit, CreditsText().c_str());
            // Focus with the caret at the end: a dialog selects all of its first control's text,
            // and one stray key would then replace the whole list.
            SetFocus(edit);
            const LPARAM end = GetWindowTextLengthW(edit);
            SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(end), end);
            return FALSE;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_DEFAULTS:
                    SetWindowTextW(GetDlgItem(dialog, IDC_CREDITS), DefaultCreditsText().c_str());
                    return TRUE;
                case IDC_FONT_LICENSE:
                    MessageBoxW(dialog, FontLicenseText().c_str(), L"Michroma - SIL Open Font License",
                                MB_OK | MB_ICONINFORMATION);
                    return TRUE;
                case IDOK:
                    if (!SaveCreditsText(EditText(GetDlgItem(dialog, IDC_CREDITS)))) {
                        const std::wstring message = L"The credits could not be saved to\n" + CreditsPath();
                        MessageBoxW(dialog, message.c_str(), L"The Black Hole", MB_OK | MB_ICONWARNING);
                        return TRUE;
                    }
                    EndDialog(dialog, IDOK);
                    return TRUE;
                case IDCANCEL:
                    EndDialog(dialog, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

}  // namespace

int RunConfigure(HWND owner) {
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), owner, SettingsProc, 0);
    return 0;
}

}  // namespace app
