// main.cpp -- flying double-pendulum cursor for Windows (C++ port).
//
// Replaces the system cursor with a live double-pendulum simulation whose pivot
// follows the real cursor. Moving the mouse accelerates the pivot, which is
// physically equivalent to tilting gravity -- flick to fling it around, hold
// still to let it settle. Certain built-in cursor types (hand/text/resize) get
// their own "snap" poses. Press Ctrl+C to stop; the normal cursor is restored.
#include "Config.h"
#include "CursorController.h"
#include "Renderer.h"
#include "SnapMode.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>   // timeBeginPeriod / timeEndPeriod

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_running{true};

// Restore the cursor exactly once even if we come through multiple exit paths.
std::atomic<bool> g_restored{false};
void restoreOnce() {
    bool expected = false;
    if (g_restored.compare_exchange_strong(expected, true)) {
        CursorController::restore();
    }
}

// atexit covers normal C++-level exits. The console control handler below
// covers Ctrl+C and the window being closed / logoff / shutdown, which do not
// unwind the stack.
BOOL WINAPI consoleHandler(DWORD ctrlType) {
    g_running.store(false);
    switch (ctrlType) {
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            restoreOnce();     // process is about to die -- clean up now
            return TRUE;
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        default:
            return TRUE;       // let the main loop unwind and restore cleanly
    }
}

} // namespace

int main() {
    std::atexit(restoreOnce);
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    // Sub-millisecond Sleep granularity so the frame pacing at up to FPS_CAP
    // is accurate instead of quantised to the ~15 ms default timer tick.
    timeBeginPeriod(1);

    CursorController controller;
    Renderer renderer;

    // Snap modes: each falls into a fixed pose while its cursor type shows.
    std::vector<SnapMode> modes;
    modes.emplace_back("pressable", cfg::OCR_HAND,   cfg::HAND_THETA1,    cfg::HAND_THETA2);
    modes.emplace_back("text",      cfg::OCR_IBEAM,  cfg::TEXT_THETA1,    cfg::TEXT_THETA2);
    modes.emplace_back("vresize",   cfg::OCR_SIZENS, cfg::TEXT_THETA1,    cfg::TEXT_THETA2);
    modes.emplace_back("hresize",   cfg::OCR_SIZEWE, cfg::HRESIZE_THETA1, cfg::HRESIZE_THETA2);
    for (auto& m : modes)
        m.setSlotHandle(controller.loadSlotHandle(m.ocrId()));

    phys::State state;
    state.theta1 = cfg::PI / 2.0;  // start hanging out to the side
    state.theta2 = cfg::PI / 2.0;

    const int detected = (cfg::MONITOR_HZ > 0) ? cfg::MONITOR_HZ
                                               : CursorController::refreshRateHz();
    const int targetFps = detected < cfg::FPS_CAP ? detected : cfg::FPS_CAP;
    const double frameDt = 1.0 / targetFps;
    const double subDt   = frameDt / cfg::SUBSTEPS;

    // Prime every slot we manage so none ever flashes Windows' default image.
    {
        const std::uint8_t* px = renderer.render(state.theta1, state.theta2);
        controller.installCursor(px, cfg::OCR_NORMAL);
        for (auto& m : modes) controller.installCursor(px, m.ocrId());
    }

    std::printf("Flying double-pendulum cursor running at %d fps. Press Ctrl+C to stop.\n",
                targetFps);
    std::fflush(stdout);

    long prevX, prevY;
    CursorController::cursorPos(prevX, prevY);
    double prevVelX = 0.0, prevVelY = 0.0;
    double smoothAx = 0.0, smoothAy = 0.0;

    using clock = std::chrono::steady_clock;

    while (g_running.load()) {
        const auto t0 = clock::now();

        long posX, posY;
        CursorController::cursorPos(posX, posY);
        const double velX = (posX - prevX) / frameDt;
        const double velY = (posY - prevY) / frameDt;
        double rawAx = (velX - prevVelX) / frameDt;
        double rawAy = (velY - prevVelY) / frameDt;

        rawAx = rawAx < -cfg::MAX_ACCEL ? -cfg::MAX_ACCEL : (rawAx > cfg::MAX_ACCEL ? cfg::MAX_ACCEL : rawAx);
        rawAy = rawAy < -cfg::MAX_ACCEL ? -cfg::MAX_ACCEL : (rawAy > cfg::MAX_ACCEL ? cfg::MAX_ACCEL : rawAy);
        smoothAx += cfg::ACCEL_SMOOTHING * (rawAx - smoothAx);
        smoothAy += cfg::ACCEL_SMOOTHING * (rawAy - smoothAy);

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
            // Free flight -- mouse acceleration tilts the effective gravity.
            const double gx = -smoothAx;
            const double gy = cfg::G - smoothAy;
            for (int i = 0; i < cfg::SUBSTEPS; ++i)
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

    restoreOnce();
    timeEndPeriod(1);
    std::printf("Cursor restored.\n");
    return 0;
}
