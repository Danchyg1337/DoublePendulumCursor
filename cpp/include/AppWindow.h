// AppWindow.h -- makes the program a silent background app (no console).
//
// Creates a hidden window that owns a system-tray icon and registers a global
// hotkey (Ctrl+Alt+P) to quit, so the user can stop the simulation and restore
// their normal cursor without a console window. The main loop calls pump()
// each frame to service these messages.
//
// Tray menu: Actors... / Refresh / Exit.
//
// The tray icon survives a late or restarted Explorer: when the app starts at
// logon before the taskbar exists, adding the icon fails, so it is retried on
// a timer, and it is re-added whenever Explorer broadcasts "TaskbarCreated".
// (The window is a hidden top-level window, not a message-only one, because
// message-only windows never receive that broadcast.)
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

    // Tray menu actions (called from pump(), on the main thread).
    void setOnRefresh(std::function<void()> f) { onRefresh_ = std::move(f); }
    void setOnActors(std::function<void()> f)  { onActors_ = std::move(f); }

    // Service pending window/tray/hotkey messages. Returns false once the user
    // has asked to quit.
    bool pump();

    bool quitRequested() const { return quit_; }

    // Native window procedure (public so the static thunk can reach it).
    long long handle(unsigned msg, unsigned long long wParam, long long lParam);

private:
    void requestQuit();
    bool addTrayIcon();      // true once the icon is in the tray

    void*                 hwnd_ = nullptr;
    std::function<void()> onExit_, onRefresh_, onActors_;
    unsigned              taskbarCreatedMsg_ = 0;
    bool                  trayAdded_ = false;
    bool                  quit_ = false;
    bool                  exitFired_ = false;
};
