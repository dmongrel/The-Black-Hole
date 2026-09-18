// Debug tracing to a file, enabled by the environment variable BLACK_HOLE_LOG.
//
// The shipped .scr has no console and a screen saver must not pop dialogs, so this is the only
// place it can say anything. BLACK_HOLE_LOG=1 writes %TEMP%\the-black-hole.log; any other value
// is taken as the path.
#ifndef BLACK_HOLE_LOG_H
#define BLACK_HOLE_LOG_H

namespace app {

void LogInit();
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
bool LogEnabled();

}  // namespace app

#endif
