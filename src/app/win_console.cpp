// Windows: opensmash is a GUI-subsystem executable (no console window on double-click). When it is started from
// cmd.exe / PowerShell, attach to that console so --help, --version and argument errors are visible. Handles that were
// redirected by the shell (> file, | more) are left alone. An interactive game session detaches again before the
// window opens, so closing that console (or Ctrl+C at its prompt, or a .bat launcher ending) does not kill the game.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>

static bool g_attached = false, g_out = false, g_err = false; // which std streams we pointed at the console

void sh_attach_parent_console() {
    auto missing = [](DWORD id) {
        HANDLE h = GetStdHandle(id);
        return h == nullptr || h == INVALID_HANDLE_VALUE;
    };
    bool out = missing(STD_OUTPUT_HANDLE), errh = missing(STD_ERROR_HANDLE);
    if (!out && !errh) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    g_attached = true;
    FILE* dummy = nullptr;
    if (out) g_out = freopen_s(&dummy, "CONOUT$", "w", stdout) == 0;
    if (errh) g_err = freopen_s(&dummy, "CONOUT$", "w", stderr) == 0;
    // the shell already printed its prompt; start our output on a fresh line
    if (out) fputc('\n', stdout);
}

void sh_detach_parent_console() {
    if (!g_attached) return;
    fflush(stdout);
    fflush(stderr);
    FILE* dummy = nullptr;
    if (g_out) freopen_s(&dummy, "NUL", "w", stdout); // redirections made by the user stay untouched
    if (g_err) freopen_s(&dummy, "NUL", "w", stderr);
    FreeConsole();
    g_attached = g_out = g_err = false;
}
