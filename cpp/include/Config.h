// Config.h -- runtime-tunable parameters for the flying double-pendulum cursor.
//
// Unlike the first C++ port (compile-time constants), every knob now lives in a
// Settings struct that is loaded from a plain-text config file at startup (see
// ConfigFile.h). A single global instance, cfg::g, holds the active values; it
// is written once before the simulation starts and only read thereafter, so the
// hot loop stays branch-free and allocation-free.
#pragma once

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <algorithm>
#include <cmath>
#include <string>

namespace cfg {

// Pi as an explicit constant so we never depend on the non-portable M_PI macro.
inline constexpr double PI = 3.14159265358979323846;

inline constexpr double deg2rad(double d) { return d * PI / 180.0; }

struct Rgb { unsigned char r, g, b; };

// All user-tunable settings, with defaults identical to the original run.py.
struct Settings {
    // ---- Geometry (pixels) --------------------------------------------------
    int    PIVOT_RADIUS = 5;    // the red dot -- the true click point
    int    BOB_RADIUS   = 3;    // the blue / green pendulum bobs
    int    ROD_WIDTH    = 2;    // rod thickness
    double L1 = 20.0;           // first arm length
    double L2 = 40.0;           // second arm length

    // ---- Masses & forces ----------------------------------------------------
    double M1 = 1.0;
    double M2 = 1.0;
    double G  = 2200.0;         // base "gravity" (px/s^2); higher = snappier
    double FRICTION = 0.35;     // exponential velocity decay (1/s)

    // ---- Integration & timing -----------------------------------------------
    int    SUBSTEPS = 6;        // physics substeps per rendered frame
    int    FPS_CAP  = 240;      // ceiling regardless of refresh rate
    int    MONITOR_HZ = 0;      // 0 = auto-detect; else force this rate
    double ACCEL_SMOOTHING = 0.25; // 0..1 higher = snappier / jitterier
    double MAX_ACCEL = 9000.0;  // clamp pivot accel (mouse teleport guard)

    // ---- Colours (RGB) ------------------------------------------------------
    Rgb COLOR_ROD  {  20,  20,  20 };
    Rgb COLOR_PIVOT{ 230,  40,  40 };  // red -- true tip
    Rgb COLOR_BOB1 {  30,  90, 220 };  // blue
    Rgb COLOR_BOB2 {  30, 160,  90 };  // green

    // ---- Snap-mode homing spring (grace-period fallback only) ---------------
    double SNAP_STIFFNESS = 900.0;

    // ---- The "hole": gentle local attractor added on top of gravity ---------
    double WELL_RADIUS    = 0.4;        // rad (~23 deg) capture zone
    double WELL_STIFFNESS = 122225.0;

    // ---- Settle & grace -----------------------------------------------------
    double SETTLE_ANGLE_TOL = 0.05;   // rad (~3 deg)
    double SETTLE_VEL_TOL   = 0.3;    // rad/s
    double SNAP_GRACE_PERIOD = 0.5;   // s a bob may fall in on its own

    // ---- Beat dancer (port of BPMidentifier) --------------------------------
    // Listens to the default output device; while the music is intense enough
    // a .gifbpm GIF dances on the beat at the bottom-right of the cursor.
    int    DANCER_ENABLED = 1;        // 0 = pendulum only (no audio capture)
    std::string ACTORS_DIR = "actors"; // folder of .gifbpm files (relative = next to exe)
    int    GIF_SIZE     = 160;        // GIF is scaled to fit this box (px)
    int    GIF_OFFSET_X = 24;         // GIF top-left, relative to the pointer tip
    int    GIF_OFFSET_Y = 24;
    double SHOW_INTENSITY = 0.70;     // appear at/above this intensity (0..1)
    double HIDE_INTENSITY = 0.62;     // disappear below this (hysteresis)
    double BEAT_OFFSET_MS = 30.0;     // delay to compensate audio output latency
    int    FAST_DROP = 1;             // 1 = intensity reacts to drops in ~0.5 s
                                      // 0 = exactly as BPMidentifier (~2 s)
    int    DEBUG = 0;                 // 1 = draw BPM + intensity next to the cursor
    int    DEBUG_LOG = 0;             // 1 = also write debug_log.csv next to the exe

    // Snap-target angles per cursor type are NOT here -- they live in their own
    // cursors.conf, loaded into an array at startup (see CursorPoses.h), so new
    // cursor types can be added without recompiling.

    // ---- Derived (never in the file) ----------------------------------------
    double snapDamping() const { return 2.0 * std::sqrt(SNAP_STIFFNESS); } // critical
    double wellDamping() const { return 2.0 * std::sqrt(WELL_STIFFNESS); }
    int margin()  const { return (PIVOT_RADIUS > BOB_RADIUS ? PIVOT_RADIUS : BOB_RADIUS) + 4; }
    int reach()   const { return static_cast<int>(L1 + L2) + margin(); }
    // Room right/below the tip for the dancing GIF (0 when disabled).
    int dancerExtent() const {
        if (!DANCER_ENABLED) return 0;
        const int gif = std::max(std::max(GIF_OFFSET_X, GIF_OFFSET_Y), 0) + GIF_SIZE;
        return DEBUG ? std::max(gif, 186) : gif;    // debug text needs ~186 px
    }
    // Square cursor bitmap: the tip sits at (reach, reach); the right/bottom
    // side grows when the GIF needs more room than the pendulum.
    int canvas()  const { return reach() + std::max(reach(), dancerExtent()); }
    int hotspot() const { return reach(); }       // pointer tip inside the bitmap
};

// The one active configuration, defined in ConfigFile.cpp.
extern Settings g;

// ---- Windows OCR_* system cursor ids (not tunable) --------------------------
inline constexpr int OCR_NORMAL   = 32512;
inline constexpr int OCR_IBEAM    = 32513;  // text
inline constexpr int OCR_SIZENWSE = 32642;  // diagonal resize  "\"
inline constexpr int OCR_SIZENESW = 32643;  // diagonal resize  "/"
inline constexpr int OCR_SIZEWE   = 32644;  // horizontal resize
inline constexpr int OCR_SIZENS   = 32645;  // vertical resize
inline constexpr int OCR_HAND     = 32649;  // links / clickable

} // namespace cfg
