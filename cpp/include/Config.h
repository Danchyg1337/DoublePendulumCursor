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

    // ---- Snap-target angles (radians; loaded from the file in DEGREES) ------
    // Fixed angles, deliberately not solved from L1/L2, so a pose never
    // degenerates for any arm-length ratio. See run.py's long comment.
    double HAND_THETA1    = deg2rad(-30.0);  // "pressable" triangle
    double HAND_THETA2    = deg2rad( 90.0);
    double TEXT_THETA1    = 0.0;             // "text" straight down, fold back up
    double TEXT_THETA2    = PI;
    double HRESIZE_THETA1 = deg2rad(-90.0);  // horizontal line: bob1 left, bob2 right
    double HRESIZE_THETA2 = deg2rad( 90.0);
    // Diagonal lines: rod1 points one way, rod2 the opposite (differ by 180),
    // exactly as the H/V poses do, so pivot-bob1-bob2 form a straight line
    // along the resize axis.
    double NWSE_THETA1    = deg2rad(-135.0); // NW-SE (\) : bob1 up-left, bob2 down-right
    double NWSE_THETA2    = deg2rad(  45.0);
    double NESW_THETA1    = deg2rad( 135.0); // NE-SW (/) : bob1 up-right, bob2 down-left
    double NESW_THETA2    = deg2rad( -45.0);

    // ---- Derived (never in the file) ----------------------------------------
    double snapDamping() const { return 2.0 * std::sqrt(SNAP_STIFFNESS); } // critical
    double wellDamping() const { return 2.0 * std::sqrt(WELL_STIFFNESS); }
    int margin()  const { return (PIVOT_RADIUS > BOB_RADIUS ? PIVOT_RADIUS : BOB_RADIUS) + 4; }
    int reach()   const { return static_cast<int>(L1 + L2) + margin(); }
    int canvas()  const { return reach() * 2; }   // square cursor bitmap size
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
