// AppWindow.h -- makes the program a silent background app (no console).
//
// Creates a hidden window that owns a system-tray icon (right-click -> Exit)
// and registers a global hotkey (Ctrl+Alt+P) to quit, so the user can stop the
// simulation and restore their normal cursor without a console window. The main
// loop calls pump() each frame to service these messages.
#pragma once

#include <functional>
#include <string>

class AppWindow {
public:
    // `onExit` is invoked exactly on shutdown paths (menu Exit, hotkey, session
    // end) and should restore the user's cursor. It may run more than once, so
    // make it idempotent.
    explicit AppWindow(std::function<void()> onExit);
    ~AppWindow();

    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    // Service pending window/tray/hotkey messages. Returns false once the user
    // has asked to quit.
    bool pump();

    bool quitRequested() const { return quit_; }

    // Native window procedure (public so the static thunk can reach it).
    long long handle(unsigned msg, unsigned long long wParam, long long lParam);

private:
    void requestQuit();

    void*                 hwnd_ = nullptr;
    std::function<void()> onExit_;
    bool                  quit_ = false;
    bool                  exitFired_ = false;
};
