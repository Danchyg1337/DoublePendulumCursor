// main.cpp -- flying double-pendulum cursor for Windows (background app).
//
// Replaces the system cursor with a live double-pendulum simulation whose pivot
// follows the real cursor. Runs silently in the background: no console window,
// a system-tray icon (right-click -> Exit) and a global Ctrl+Alt+P hotkey stop
// it and restore the normal cursor. Tunables are read from pendulum.conf and
// per-cursor snap poses from cursors.conf at startup (both created next to the
// exe on first run if missing).
#include "AppWindow.h"
#include "Config.h"
#include "ConfigFile.h"
#include "CursorController.h"
#include "CursorPoses.h"
#include "Renderer.h"
#include "SnapMode.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW
#include <mmsystem.h>   // timeBeginPeriod / timeEndPeriod

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_restored{false};
void restoreOnce() {
    bool expected = false;
    if (g_restored.compare_exchange_strong(expected, true)) {
        CursorController::restore();
    }
}

// Directory containing this executable, with a trailing backslash.
std::wstring exeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf, n);
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

bool fileExists(const std::string& p) {
    std::wstring w(p.begin(), p.end());
    DWORD a = GetFileAttributesW(w.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// argv[1] = pendulum.conf path (optional), argv[2] = cursors.conf path (optional).
std::string cliArg(int index) {
    std::string result;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > index) result = narrow(argv[index]);
    if (argv) LocalFree(argv);
    return result;
}

// Load pendulum.conf into cfg::g; create a documented default if it's missing.
void loadConfiguration() {
    std::string path = cliArg(1);
    if (path.empty()) path = narrow(exeDir()) + "pendulum.conf";
    if (fileExists(path)) cfg::loadConfig(path);
    else                  cfg::writeDefaultConfig(path);  // defaults already active
}

// Load cursors.conf into an array; create a default if missing; fall back to
// the built-in poses if the file is empty or unreadable.
std::vector<cursors::Pose> loadCursorPoses() {
    std::string path = cliArg(2);
    if (path.empty()) path = narrow(exeDir()) + "cursors.conf";

    std::vector<cursors::Pose> poses;
    if (fileExists(path)) {
        cursors::load(path, poses);
    } else {
        cursors::writeDefault(path);
    }
    if (poses.empty()) poses = cursors::defaults();
    return poses;
}

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    std::atexit(restoreOnce);
    loadConfiguration();
    const std::vector<cursors::Pose> poses = loadCursorPoses();

    // A logon scheduled task (or Startup-folder launch) can start us below
    // normal priority; force normal so cursor updates stay smooth.
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);

    // Sub-millisecond Sleep granularity for accurate frame pacing.
    timeBeginPeriod(1);

    try {
        CursorController controller;
        Renderer renderer;
        AppWindow app(restoreOnce);

        // Build one snap mode per configured cursor pose.
        std::vector<SnapMode> modes;
        modes.reserve(poses.size());
        for (const auto& p : poses)
            modes.emplace_back(p.name, p.ocrId, p.theta1, p.theta2);
        for (auto& m : modes)
            m.setSlotHandle(controller.loadSlotHandle(m.ocrId()));

        const cfg::Settings& s = cfg::g;
        phys::State state;
        state.theta1 = cfg::PI / 2.0;  // start hanging out to the side
        state.theta2 = cfg::PI / 2.0;

        const int detected = (s.MONITOR_HZ > 0) ? s.MONITOR_HZ
                                                : CursorController::refreshRateHz();
        const int targetFps = detected < s.FPS_CAP ? detected : s.FPS_CAP;
        const double frameDt = 1.0 / targetFps;
        const double subDt   = frameDt / s.SUBSTEPS;

        // Prime every slot so none flashes Windows' default image.
        {
            const std::uint8_t* px = renderer.render(state.theta1, state.theta2);
            controller.installCursor(px, cfg::OCR_NORMAL);
            for (auto& m : modes) controller.installCursor(px, m.ocrId());
        }

        long prevX, prevY;
        CursorController::cursorPos(prevX, prevY);
        double prevVelX = 0.0, prevVelY = 0.0;
        double smoothAx = 0.0, smoothAy = 0.0;

        using clock = std::chrono::steady_clock;

        while (app.pump()) {
            const auto t0 = clock::now();

            long posX, posY;
            CursorController::cursorPos(posX, posY);
            const double velX = (posX - prevX) / frameDt;
            const double velY = (posY - prevY) / frameDt;
            double rawAx = (velX - prevVelX) / frameDt;
            double rawAy = (velY - prevVelY) / frameDt;

            rawAx = rawAx < -s.MAX_ACCEL ? -s.MAX_ACCEL : (rawAx > s.MAX_ACCEL ? s.MAX_ACCEL : rawAx);
            rawAy = rawAy < -s.MAX_ACCEL ? -s.MAX_ACCEL : (rawAy > s.MAX_ACCEL ? s.MAX_ACCEL : rawAy);
            smoothAx += s.ACCEL_SMOOTHING * (rawAx - smoothAx);
            smoothAy += s.ACCEL_SMOOTHING * (rawAy - smoothAy);

            const void* current = controller.activeCursorHandle();
            SnapMode* active = nullptr;
            for (auto& m : modes) {
                if (m.isActive(current)) { active = &m; break; }
            }

            if (active) {
                for (auto& m : modes)
                    if (&m != active) m.deactivate();
                state = active->advance(state, frameDt, subDt);
                controller.installCursor(renderer.render(state.theta1, state.theta2),
                                         active->ocrId());
            } else {
                for (auto& m : modes) m.deactivate();
                const double gx = -smoothAx;
                const double gy = s.G - smoothAy;
                for (int i = 0; i < s.SUBSTEPS; ++i)
                    state = phys::step(state, gx, gy, subDt);
                controller.installCursor(renderer.render(state.theta1, state.theta2),
                                         cfg::OCR_NORMAL);
            }

            prevX = posX; prevY = posY;
            prevVelX = velX; prevVelY = velY;

            const auto elapsed = std::chrono::duration<double>(clock::now() - t0).count();
            const double remaining = frameDt - elapsed;
            if (remaining > 0.0)
                std::this_thread::sleep_for(std::chrono::duration<double>(remaining));
        }
    } catch (const std::exception& e) {
        restoreOnce();
        MessageBoxA(nullptr, e.what(), "Pendulum Cursor - error", MB_ICONERROR | MB_OK);
        timeEndPeriod(1);
        return 1;
    }

    restoreOnce();
    timeEndPeriod(1);
    return 0;
}
