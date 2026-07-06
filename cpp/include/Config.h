// Config.h -- all tunable parameters for the flying double-pendulum cursor.
//
// Everything here is a compile-time constant so the hot loop never touches
// global mutable state. Values mirror the original Python script exactly;
// tweak and rebuild to change behaviour.
#pragma once

// Make M_PI available even on toolchains (e.g. MSVC) that gate it behind this.
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <algorithm>
#include <cmath>

namespace cfg {

// Pi as an explicit constant so we never depend on the non-portable M_PI macro.
inline constexpr double PI = 3.14159265358979323846;

// ---- Geometry (pixels) ------------------------------------------------------
inline constexpr int    PIVOT_RADIUS = 5;   // the red dot -- the true click point
inline constexpr int    BOB_RADIUS   = 3;   // the blue / green pendulum bobs
inline constexpr int    ROD_WIDTH    = 2;   // rod thickness
inline constexpr double L1 = 20.0;          // first arm length
inline constexpr double L2 = 40.0;          // second arm length

// ---- Masses & forces --------------------------------------------------------
inline constexpr double M1 = 1.0;
inline constexpr double M2 = 1.0;
inline constexpr double G  = 2200.0;        // base "gravity" (px/s^2); higher = snappier
inline constexpr double FRICTION = 0.35;    // exponential velocity decay (1/s)

// ---- Canvas -----------------------------------------------------------------
// The pivot sits at the centre so the pendulum has L1+L2 px of room to swing
// in every direction without ever leaving the bitmap.
inline constexpr int MARGIN = (PIVOT_RADIUS > BOB_RADIUS ? PIVOT_RADIUS : BOB_RADIUS) + 4;
inline constexpr int REACH  = static_cast<int>(L1 + L2) + MARGIN;
inline constexpr int CANVAS = REACH * 2;    // square cursor bitmap size
inline constexpr int HOTSPOT_X = REACH;     // real pointer tip inside the bitmap
inline constexpr int HOTSPOT_Y = REACH;

// ---- Integration & timing ---------------------------------------------------
inline constexpr int    SUBSTEPS = 6;       // physics substeps per rendered frame
inline constexpr int    FPS_CAP  = 240;     // ceiling regardless of refresh rate
inline constexpr int    MONITOR_HZ = 0;     // 0 = auto-detect; else force this rate
inline constexpr double ACCEL_SMOOTHING = 0.25; // 0..1 higher = snappier / jitterier
inline constexpr double MAX_ACCEL = 9000.0; // clamp pivot accel (mouse teleport guard)

// ---- Colours (RGB, 0..255) --------------------------------------------------
struct Rgb { unsigned char r, g, b; };
inline constexpr Rgb COLOR_ROD  {  20,  20,  20 };
inline constexpr Rgb COLOR_PIVOT{ 230,  40,  40 };  // red -- true tip
inline constexpr Rgb COLOR_BOB1 {  30,  90, 220 };  // blue
inline constexpr Rgb COLOR_BOB2 {  30, 160,  90 };  // green

// ---- Snap-mode homing spring (grace-period fallback only) -------------------
inline constexpr double SNAP_STIFFNESS = 900.0;
inline const     double SNAP_DAMPING   = 2.0 * std::sqrt(SNAP_STIFFNESS); // critical

// ---- The "hole": gentle local attractor added on top of gravity -------------
inline constexpr double WELL_RADIUS    = 0.4;        // rad (~23 deg) capture zone
inline constexpr double WELL_STIFFNESS = 122225.0;
inline const     double WELL_DAMPING   = 2.0 * std::sqrt(WELL_STIFFNESS);

// ---- Settle & grace ---------------------------------------------------------
inline constexpr double SETTLE_ANGLE_TOL = 0.05;  // rad (~3 deg)
inline constexpr double SETTLE_VEL_TOL   = 0.3;   // rad/s
inline constexpr double SNAP_GRACE_PERIOD = 0.5;  // s a bob may fall in on its own

// ---- Snap-target angles (fixed, deliberately not solved from L1/L2) ---------
// See the long comment in the original script: fixed angles never degenerate
// for any arm-length ratio, unlike solved-for angles.
inline const double HAND_THETA1    = -30.0 * PI / 180.0; // "pressable" triangle
inline const double HAND_THETA2    =  90.0 * PI / 180.0;
inline constexpr double TEXT_THETA1 = 0.0;               // "text" straight down / fold
inline const     double TEXT_THETA2 = PI;
inline const double HRESIZE_THETA1 = -90.0 * PI / 180.0; // horizontal resize
inline const double HRESIZE_THETA2 =  90.0 * PI / 180.0;

// ---- Windows OCR_* system cursor ids ----------------------------------------
inline constexpr int OCR_NORMAL = 32512;
inline constexpr int OCR_IBEAM  = 32513;
inline constexpr int OCR_SIZENS = 32645;
inline constexpr int OCR_SIZEWE = 32644;
inline constexpr int OCR_HAND   = 32649;

} // namespace cfg
