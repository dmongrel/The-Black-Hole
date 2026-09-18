// The credits list: where it lives, how a line becomes a credit, and the built-in default.
//
// The list is a UTF-8 text file, one credit per line: %APPDATA%\The-Black-Hole\credits.txt, or
// the file named by BLACK_HOLE_CREDITS. Until the user saves one, the default list embedded in
// the .scr (assets/credits-default.txt) plays. A saved file with no credits turns them off.
#ifndef BLACK_HOLE_CREDITS_H
#define BLACK_HOLE_CREDITS_H

#include <string>
#include <vector>

namespace app {

// One credit. `second` is empty for a single line; a colon splits "Role: Name" into two.
struct Credit {
    std::wstring first;
    std::wstring second;
};

std::wstring CreditsPath();

// The list as text, with "\r\n" line ends for an edit control: the saved file, or the default
// when there is none.
std::wstring CreditsText();
std::wstring DefaultCreditsText();

std::vector<Credit> ParseCredits(const std::wstring& text);
std::vector<Credit> LoadCredits();

// Writes `text` as the saved list, creating its folder. False on failure.
bool SaveCreditsText(const std::wstring& text);

}  // namespace app

#endif
