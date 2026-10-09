#include "ActorsWindow.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX   // keep std::min / std::max usable (MSVC's windows.h macros)
#endif
#include <windows.h>
#include <objbase.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <vector>

// ---- ActorsBridge ------------------------------------------------------------------
void ActorsBridge::publish(std::shared_ptr<dancer::Dancer> d, std::optional<int> bpm,
                           double maxBpmDiff, const std::string& actorsDir) {
    std::lock_guard<std::mutex> lk(m_);
    if (d.get() != last_) { ++v_.generation; last_ = d.get(); }
    v_.dancer = std::move(d);
    v_.bpm = bpm;
    v_.maxBpmDiff = maxBpmDiff;
    v_.actorsDir = actorsDir;
}

ActorsBridge::View ActorsBridge::view() const {
    std::lock_guard<std::mutex> lk(m_);
    return v_;
}

void ActorsBridge::requestDisabled(std::set<std::string> files) {
    std::lock_guard<std::mutex> lk(m_);
    disabled_ = std::move(files);
}

void ActorsBridge::requestReload() {
    std::lock_guard<std::mutex> lk(m_);
    reload_ = true;
}

std::optional<std::set<std::string>> ActorsBridge::takeDisabled() {
    std::lock_guard<std::mutex> lk(m_);
    auto out = std::move(disabled_);
    disabled_.reset();
    return out;
}

bool ActorsBridge::takeReload() {
    std::lock_guard<std::mutex> lk(m_);
    const bool r = reload_;
    reload_ = false;
    return r;
}

// ---- ActorsWindow ------------------------------------------------------------------
namespace {
constexpr wchar_t kClass[] = L"PendulumCursorActorsWindow";
constexpr int HEADER = 84;            // toolbar + status lines
constexpr int PAD = 10;
constexpr int TILE_W = 172, TILE_H = 214, THUMB = 150;
constexpr UINT_PTR TIMER_ANIM = 1;
constexpr int ID_ADD = 101, ID_FOLDER = 102, ID_ENABLE_ALL = 103;

constexpr COLORREF C_BG = RGB(30, 31, 34), C_TILE = RGB(43, 45, 49), C_TEXT = RGB(235, 235, 235);
constexpr COLORREF C_DIM = RGB(140, 140, 140), C_PLAY = RGB(59, 165, 93), C_OFF = RGB(220, 70, 70);
constexpr COLORREF C_NEAR = RGB(110, 200, 120);

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<ActorsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return static_cast<LRESULT>(self->handle(hwnd, msg, wp, lp));
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Composite a premultiplied RGBA frame over `bg` into a BGRA buffer (dimmed
// for disabled GIFs) and stretch it into the box, keeping the aspect ratio.
void drawThumb(HDC dc, const gif::Image& im, int bx, int by, int box, COLORREF bg, bool dim) {
    if (im.w <= 0 || im.h <= 0) return;
    std::vector<std::uint32_t> px(static_cast<std::size_t>(im.w) * im.h);
    const int br = GetRValue(bg), bgc = GetGValue(bg), bb = GetBValue(bg);
    for (std::size_t i = 0; i < px.size(); ++i) {
        const std::uint8_t* p = &im.rgba[i * 4];
        const int ia = 255 - p[3];
        int r = p[0] + br * ia / 255, g = p[1] + bgc * ia / 255, b = p[2] + bb * ia / 255;
        if (dim) { r = (r + 2 * br) / 3; g = (g + 2 * bgc) / 3; b = (b + 2 * bb) / 3; }
        px[i] = static_cast<std::uint32_t>(b) | (static_cast<std::uint32_t>(g) << 8) | (static_cast<std::uint32_t>(r) << 16);
    }
    const double s = std::min(static_cast<double>(box) / im.w, static_cast<double>(box) / im.h);
    const int w = std::max(1, static_cast<int>(im.w * s)), h = std::max(1, static_cast<int>(im.h * s));
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = im.w;
    bi.bmiHeader.biHeight = -im.h;           // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchDIBits(dc, bx + (box - w) / 2, by + (box - h) / 2, w, h, 0, 0, im.w, im.h,
                  px.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

void fill(HDC dc, int x, int y, int w, int h, COLORREF c) {
    RECT r{ x, y, x + w, y + h };
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void text(HDC dc, const std::wstring& s, int x, int y, int w, int h, COLORREF c, UINT flags) {
    RECT r{ x, y, x + w, y + h };
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, flags | DT_SINGLELINE | DT_NOPREFIX);
}

// the tiles in display order: by tempo
std::vector<const dancer::Actor*> sortedActors(const ActorsBridge::View& v) {
    std::vector<const dancer::Actor*> out;
    if (!v.dancer || !v.dancer->ready()) return out;
    for (const auto& a : v.dancer->actors()) out.push_back(a.get());
    std::stable_sort(out.begin(), out.end(), [](const dancer::Actor* a, const dancer::Actor* b) {
        return a->bpm < b->bpm;
    });
    return out;
}
} // namespace

ActorsWindow::~ActorsWindow() {
    if (HWND h = static_cast<HWND>(hwnd_.load())) PostMessageW(h, WM_CLOSE, 0, 0);
    if (thread_.joinable()) thread_.join();
}

void ActorsWindow::show() {
    if (running_.load()) {
        if (HWND h = static_cast<HWND>(hwnd_.load())) {
            ShowWindow(h, IsIconic(h) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(h);
        }
        return;
    }
    if (thread_.joinable()) thread_.join();
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void ActorsWindow::run() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // file dialog
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(101));
    wc.hbrBackground = nullptr;              // we paint everything (no flicker)
    RegisterClassExW(&wc);

    font_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    fontBold_ = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    refreshView();

    HWND hwnd = CreateWindowExW(0, kClass, L"Pendulum Cursor - Actors",
                                WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 4 * TILE_W + 5 * PAD + 30, 620,
                                nullptr, nullptr, inst, this);
    if (hwnd) {
        hwnd_ = hwnd;
        const wchar_t* labels[3] = { L"Add GIFs...", L"Open folder", L"Enable all" };
        const int ids[3] = { ID_ADD, ID_FOLDER, ID_ENABLE_ALL };
        for (int i = 0; i < 3; ++i) {
            HWND b = CreateWindowExW(0, L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     PAD + i * 120, PAD, 110, 28, hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids[i])), inst, nullptr);
            SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            buttons_[i] = b;
        }
        SetTimer(hwnd, TIMER_ANIM, 40, nullptr);
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    hwnd_ = nullptr;
    if (font_) DeleteObject(static_cast<HFONT>(font_));
    if (fontBold_) DeleteObject(static_cast<HFONT>(fontBold_));
    font_ = fontBold_ = nullptr;
    view_ = ActorsBridge::View{};            // let go of the dancer
    if (SUCCEEDED(co)) CoUninitialize();
    running_ = false;
}

void ActorsWindow::refreshView() {
    view_ = bridge_.view();
    if (view_.generation != generation_) {   // new dancer (start, refresh, reload)
        generation_ = view_.generation;
        if (!view_.actorsDir.empty()) disabled_ = dancer::loadDisabledList(view_.actorsDir);
    }
}

void ActorsWindow::layout(int clientW) {
    cols_ = std::max(1, (clientW - PAD) / (TILE_W + PAD));
    const int n = static_cast<int>(sortedActors(view_).size());
    const int rows = (n + cols_ - 1) / cols_;
    contentH_ = rows * (TILE_H + PAD) + PAD;
}

int ActorsWindow::hitTest(int x, int y) const {
    if (y < HEADER) return -1;
    const int cx = x - PAD, cy = y - HEADER - PAD + scrollY_;
    if (cx < 0 || cy < 0) return -1;
    const int c = cx / (TILE_W + PAD), r = cy / (TILE_H + PAD);
    if (c >= cols_ || cx % (TILE_W + PAD) >= TILE_W || cy % (TILE_H + PAD) >= TILE_H) return -1;
    const int i = r * cols_ + c;
    return i < static_cast<int>(sortedActors(view_).size()) ? i : -1;
}

void ActorsWindow::toggle(int index) {
    const auto list = sortedActors(view_);
    if (index < 0 || index >= static_cast<int>(list.size())) return;
    const std::string& f = list[static_cast<std::size_t>(index)]->file;
    if (disabled_.count(f)) { disabled_.erase(f); status_ = "Enabled " + f; }
    else                    { disabled_.insert(f); status_ = "Disabled " + f; }
    bridge_.requestDisabled(disabled_);
}

void ActorsWindow::addFiles() {
    if (view_.actorsDir.empty()) return;
    std::vector<wchar_t> buf(65536, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(hwnd_.load());
    ofn.lpstrFilter = L"GIF actors (*.gifbpm)\0*.gifbpm\0All files\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = L"Add GIF actors";
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;

    // single file: full path; several: directory, then names, double-null ended
    std::vector<std::wstring> files;
    const std::wstring first(buf.data());
    const wchar_t* p = buf.data() + first.size() + 1;
    if (*p == L'\0') {
        files.push_back(first);
    } else {
        for (; *p; p += std::wcslen(p) + 1) files.push_back(first + L"\\" + p);
    }
    const std::wstring dir = widen(view_.actorsDir);
    CreateDirectoryW(dir.c_str(), nullptr);
    int added = 0, skipped = 0;
    for (const auto& src : files) {
        const std::wstring name = std::filesystem::path(src).filename().wstring();
        const std::wstring dst = dir + L"\\" + name;
        if (CopyFileW(src.c_str(), dst.c_str(), TRUE)) ++added; else ++skipped;
    }
    status_ = "Added " + std::to_string(added) + " GIF(s)";
    if (skipped) status_ += ", " + std::to_string(skipped) + " skipped (already there or unreadable)";
    if (added) { status_ += " - loading..."; bridge_.requestReload(); }
}

long long ActorsWindow::handle(void* hwndPtr, unsigned msg, unsigned long long wParam, long long lParam) {
    HWND hwnd = static_cast<HWND>(hwndPtr);
    const WPARAM wp = static_cast<WPARAM>(wParam);
    const LPARAM lp = static_cast<LPARAM>(lParam);
    auto updateScroll = [&] {
        RECT rc; GetClientRect(hwnd, &rc);
        layout(rc.right);
        const int viewH = std::max(1, static_cast<int>(rc.bottom) - HEADER);
        scrollY_ = std::max(0, std::min(scrollY_, contentH_ - viewH));
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin = 0;
        si.nMax = std::max(0, contentH_ - 1);
        si.nPage = static_cast<UINT>(viewH);
        si.nPos = scrollY_;
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
    };
    switch (msg) {
        case WM_TIMER:
            refreshView();
            updateScroll();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SIZE:
            updateScroll();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_VSCROLL: {
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            GetScrollInfo(hwnd, SB_VERT, &si);
            int y = si.nPos;
            switch (LOWORD(wp)) {
                case SB_LINEUP: y -= 40; break;
                case SB_LINEDOWN: y += 40; break;
                case SB_PAGEUP: y -= static_cast<int>(si.nPage); break;
                case SB_PAGEDOWN: y += static_cast<int>(si.nPage); break;
                case SB_THUMBTRACK: y = si.nTrackPos; break;
                default: break;
            }
            scrollY_ = y;
            updateScroll();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL:
            scrollY_ -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 60;
            updateScroll();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP:
            toggle(hitTest(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
                SetCursor(LoadCursor(nullptr, hitTest(pt.x, pt.y) >= 0 ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_COMMAND:
            if (LOWORD(wp) == ID_ADD) addFiles();
            else if (LOWORD(wp) == ID_FOLDER && !view_.actorsDir.empty())
                ShellExecuteW(hwnd, L"open", widen(view_.actorsDir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            else if (LOWORD(wp) == ID_ENABLE_ALL) {
                disabled_.clear();
                bridge_.requestDisabled(disabled_);
                status_ = "All GIFs enabled";
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
            HGDIOBJ old = SelectObject(mem, bmp);
            paint(mem, rc.right, rc.bottom);
            BitBlt(dc, 0, HEADER, rc.right, rc.bottom - HEADER, mem, 0, HEADER, SRCCOPY);
            BitBlt(dc, 0, 0, rc.right, HEADER, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, TIMER_ANIM);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ActorsWindow::paint(void* hdcPtr, int w, int h) {
    HDC dc = static_cast<HDC>(hdcPtr);
    fill(dc, 0, 0, w, h, C_BG);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, static_cast<HFONT>(font_));

    const auto list = sortedActors(view_);
    const double t = nowMs();
    const auto* playing = view_.dancer ? view_.dancer->playing() : nullptr;
    const bool haveBpm = view_.bpm.has_value();

    // ---- tiles ----
    for (std::size_t i = 0; i < list.size(); ++i) {
        const dancer::Actor* a = list[i];
        const int c = static_cast<int>(i) % cols_, r = static_cast<int>(i) / cols_;
        const int x = PAD + c * (TILE_W + PAD);
        const int y = HEADER + PAD + r * (TILE_H + PAD) - scrollY_;
        if (y + TILE_H < HEADER || y > h) continue;
        const bool off = disabled_.count(a->file) > 0;
        const bool play = (a == playing);
        if (play) fill(dc, x - 3, y - 3, TILE_W + 6, TILE_H + 6, C_PLAY);
        fill(dc, x, y, TILE_W, TILE_H, C_TILE);

        if (!a->frames.empty() && a->duration > 0) {
            const int f = std::min(a->frameAtTime(std::fmod(t, a->duration)),
                                   static_cast<int>(a->frames.size()) - 1);
            drawThumb(dc, a->frames[static_cast<std::size_t>(f)], x + (TILE_W - THUMB) / 2, y + 8, THUMB, C_TILE, off);
        }
        if (off) {
            SelectObject(dc, static_cast<HFONT>(fontBold_));
            text(dc, L"DISABLED", x, y + 8, TILE_W, THUMB, C_OFF, DT_CENTER | DT_VCENTER);
        }
        SelectObject(dc, static_cast<HFONT>(fontBold_));
        text(dc, widen(a->name), x + 8, y + THUMB + 14, TILE_W - 16, 20, off ? C_DIM : C_TEXT, DT_LEFT | DT_END_ELLIPSIS);
        SelectObject(dc, static_cast<HFONT>(font_));
        wchar_t info[64];
        std::swprintf(info, 64, L"%.0f BPM \x00b7 %ls", a->bpm, a->loopable ? L"loop" : L"once");
        text(dc, info, x + 8, y + THUMB + 34, TILE_W - 16, 20, C_DIM, DT_LEFT);
        // fit to the music right now: tempo difference, or "too far"
        if (haveBpm && !off) {
            const double d = a->bpm - *view_.bpm;
            const bool inRange = view_.maxBpmDiff <= 0 || std::fabs(d) <= view_.maxBpmDiff;
            wchar_t fit[32];
            if (inRange) std::swprintf(fit, 32, L"%+.0f", d);
            else         std::swprintf(fit, 32, L"too far");
            text(dc, fit, x + 8, y + THUMB + 34, TILE_W - 16, 20, inRange ? C_NEAR : C_DIM, DT_RIGHT);
        }
        if (play) {
            SelectObject(dc, static_cast<HFONT>(fontBold_));
            text(dc, L"\x25B6 playing", x + 8, y + 10, TILE_W - 16, 18, C_PLAY, DT_RIGHT);
        }
    }

    // ---- header (drawn last, over scrolled tiles) ----
    fill(dc, 0, 0, w, HEADER, C_BG);
    SelectObject(dc, static_cast<HFONT>(font_));
    std::wstring line1;
    if (!view_.dancer) line1 = L"Beat dancer is off (DANCER_ENABLED = 0)";
    else if (!view_.dancer->ready()) line1 = L"Loading GIFs...";
    else {
        wchar_t b[200];
        if (haveBpm) {
            if (view_.maxBpmDiff > 0)
                std::swprintf(b, 200, L"Music: %d BPM  \x00b7  GIFs within \x00b1%.0f BPM can play  \x00b7  %zu GIFs",
                              *view_.bpm, view_.maxBpmDiff, list.size());
            else
                std::swprintf(b, 200, L"Music: %d BPM  \x00b7  %zu GIFs", *view_.bpm, list.size());
        } else {
            std::swprintf(b, 200, L"No beat right now  \x00b7  %zu GIFs", list.size());
        }
        line1 = b;
    }
    text(dc, line1, 3 * 120 + 2 * PAD, PAD, std::max(10, w - 3 * 120 - 3 * PAD), 28, C_TEXT, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    const std::wstring line2 = status_.empty()
        ? std::wstring(L"Click a GIF to enable / disable it. Disabled GIFs are listed in disabled.txt in the actors folder.")
        : widen(status_);
    text(dc, line2, PAD, PAD + 36, std::max(10, w - 2 * PAD), 22, C_DIM, DT_LEFT | DT_END_ELLIPSIS);
}
