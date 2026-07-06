#include "AppWindow.h"
#include "Resource.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <stdexcept>

namespace {
constexpr wchar_t kClassName[] = L"PendulumCursorHiddenWindow";
constexpr UINT    WM_TRAYICON  = WM_APP + 1;
constexpr UINT    HOTKEY_ID    = 1;
constexpr UINT    ID_EXIT      = 1001;

// Prefer the embedded app icon; fall back to the generic application icon.
HICON appIcon() {
    HICON h = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
                                            MAKEINTRESOURCEW(IDI_APPICON),
                                            IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    return h ? h : LoadIcon(nullptr, IDI_APPLICATION);
}

NOTIFYICONDATAW makeTrayData(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = appIcon();
    lstrcpynW(nid.szTip, L"Double-Pendulum Cursor  -  right-click to Exit (Ctrl+Alt+P)",
              ARRAYSIZE(nid.szTip));
    return nid;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    auto* self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        return static_cast<LRESULT>(self->handle(msg, wParam, lParam));
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
} // namespace

AppWindow::AppWindow(std::function<void()> onExit) : onExit_(std::move(onExit)) {
    HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClassName;
    wc.hIcon = appIcon();
    RegisterClassExW(&wc);  // ignore "already registered" on a second instance

    HWND hwnd = CreateWindowExW(0, kClassName, L"Pendulum Cursor",
                                0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, inst, this);
    if (!hwnd) throw std::runtime_error("CreateWindowEx failed");
    hwnd_ = hwnd;

    NOTIFYICONDATAW nid = makeTrayData(hwnd);
    Shell_NotifyIconW(NIM_ADD, &nid);

    // Global stop hotkey. If it's already taken by another app, we simply rely
    // on the tray menu instead -- not fatal.
    RegisterHotKey(hwnd, HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'P');
}

AppWindow::~AppWindow() {
    if (hwnd_) {
        HWND hwnd = static_cast<HWND>(hwnd_);
        UnregisterHotKey(hwnd, HOTKEY_ID);
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        DestroyWindow(hwnd);
    }
}

void AppWindow::requestQuit() {
    if (!exitFired_) {
        exitFired_ = true;
        if (onExit_) onExit_();
    }
    quit_ = true;
    PostQuitMessage(0);
}

long long AppWindow::handle(unsigned msg, unsigned long long wParam, long long lParam) {
    HWND hwnd = static_cast<HWND>(hwnd_);
    switch (msg) {
        case WM_TRAYICON:
            if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
                POINT pt;
                GetCursorPos(&pt);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");
                SetForegroundWindow(hwnd);  // required so the menu dismisses right
                TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
                PostMessageW(hwnd, WM_NULL, 0, 0);
                DestroyMenu(menu);
            }
            return 0;

        case WM_COMMAND:
            if (LOWORD(wParam) == ID_EXIT) requestQuit();
            return 0;

        case WM_HOTKEY:
            if (wParam == HOTKEY_ID) requestQuit();
            return 0;

        case WM_ENDSESSION:
            if (wParam) {
                if (!exitFired_) { exitFired_ = true; if (onExit_) onExit_(); }
                quit_ = true;
            }
            return 0;

        case WM_CLOSE:
            requestQuit();
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, static_cast<WPARAM>(wParam),
                          static_cast<LPARAM>(lParam));
}

bool AppWindow::pump() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) { quit_ = true; continue; }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return !quit_;
}
