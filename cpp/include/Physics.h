// Physics.h -- double-pendulum dynamics with RK4 integration.
//
// Platform independent: no Windows headers, so it compiles and can be unit
// tested anywhere. An accelerating pivot is equivalent, in the pivot's own
// reference frame, to a uniform "gravity" vector (gx, gy) acting on both
// bobs -- so mouse acceleration just feeds into the effective gravity.
#pragma once

namespace phys {

// Full pendulum state: two angles and their angular velocities.
struct State {
    double theta1 = 0.0;
    double theta2 = 0.0;
    double w1 = 0.0;
    double w2 = 0.0;
};

// A per-joint acceleration modifier used by the snap modes.
//   None   -- natural physics only.
//   Well   -- ADD a gentle local attractor (the "hole") around `target`.
//   Homing -- REPLACE accel with a strong critically-damped spring to `target`.
//   Pin    -- REPLACE accel with zero (freeze the joint where it is).
struct JointForce {
    enum class Kind { None, Well, Homing, Pin };
    Kind   kind   = Kind::None;
    double target = 0.0;

    static JointForce none()             { return {}; }
    static JointForce well(double t)     { return {Kind::Well,   t}; }
    static JointForce homing(double t)   { return {Kind::Homing, t}; }
    static JointForce pin()              { return {Kind::Pin,    0.0}; }
};

// Shortest signed angular distance target-current, wrapped to [-pi, pi].
double angleDiff(double target, double current);

// True if `theta` is within tolerance of `target` and nearly stationary.
bool settled(double target, double theta, double w);

// Advance one RK4 step under effective gravity (gx, gy), then apply
// dt-independent exponential friction. Optional per-joint force modifiers
// implement the snap behaviour.
State step(const State& s, double gx, double gy, double dt,
           JointForce f1 = JointForce::none(),
           JointForce f2 = JointForce::none());

} // namespace phys
