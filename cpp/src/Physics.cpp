#include "Physics.h"
#include "Config.h"

#include <cmath>

namespace phys {
namespace {

// Angular accelerations of a double pendulum whose pivot is accelerating.
void baseAccel(const State& s, double gx, double gy,
               double& w1dot, double& w2dot) {
    const cfg::Settings& c = cfg::g;
    const double dth = s.theta1 - s.theta2;
    const double cs = std::cos(dth);
    const double sn = std::sin(dth);

    const double a11 = (c.M1 + c.M2) * c.L1;
    const double a12 = c.M2 * c.L2 * cs;
    const double b1  = -c.M2 * c.L2 * s.w2 * s.w2 * sn
                       - (c.M1 + c.M2)
                             * (gy * std::sin(s.theta1) - gx * std::cos(s.theta1));

    const double a21 = c.M2 * c.L1 * cs;
    const double a22 = c.M2 * c.L2;
    const double b2  = c.M2 * c.L1 * s.w1 * s.w1 * sn
                       - c.M2
                             * (gy * std::sin(s.theta2) - gx * std::cos(s.theta2));

    double det = a11 * a22 - a12 * a21;
    if (std::fabs(det) < 1e-9) det = 1e-9;

    w1dot = (b1 * a22 - a12 * b2) / det;
    w2dot = (a11 * b2 - b1 * a21) / det;
}

// Strong critically-damped spring that FORCES theta toward target. Biases the
// approach direction to match existing motion so engaging it never slams the
// brakes and yanks a reversal.
double homingAccel(double target, double theta, double w) {
    double d = angleDiff(target, theta);
    if (w > 0.0 && d < 0.0)      d += 2.0 * cfg::PI;
    else if (w < 0.0 && d > 0.0) d -= 2.0 * cfg::PI;
    return cfg::g.SNAP_STIFFNESS * d - cfg::g.snapDamping() * w;
}

// Gentle local attractor -- zero outside WELL_RADIUS, so it never disturbs
// normal swinging; only pulls / damps once a bob is already close.
double wellAccel(double target, double theta, double w) {
    const double d = angleDiff(target, theta);
    if (std::fabs(d) > cfg::g.WELL_RADIUS) return 0.0;
    return cfg::g.WELL_STIFFNESS * d - cfg::g.wellDamping() * w;
}

// Apply a joint force modifier to a naturally-computed angular acceleration.
double applyForce(const JointForce& f, double baseDot, double theta, double w) {
    switch (f.kind) {
        case JointForce::Kind::Well:   return baseDot + wellAccel(f.target, theta, w);
        case JointForce::Kind::Homing: return homingAccel(f.target, theta, w);
        case JointForce::Kind::Pin:    return 0.0;
        case JointForce::Kind::None:   break;
    }
    return baseDot;
}

// Derivative of the state vector: (w1, w2, w1dot, w2dot) with modifiers.
State deriv(const State& s, double gx, double gy,
            const JointForce& f1, const JointForce& f2) {
    double w1dot, w2dot;
    baseAccel(s, gx, gy, w1dot, w2dot);
    w1dot = applyForce(f1, w1dot, s.theta1, s.w1);
    w2dot = applyForce(f2, w2dot, s.theta2, s.w2);
    return State{s.w1, s.w2, w1dot, w2dot};
}

inline State axpy(const State& a, double h, const State& b) {
    return State{a.theta1 + h * b.theta1,
                 a.theta2 + h * b.theta2,
                 a.w1     + h * b.w1,
                 a.w2     + h * b.w2};
}

} // namespace

double angleDiff(double target, double current) {
    double m = std::fmod(target - current + cfg::PI, 2.0 * cfg::PI);
    if (m < 0.0) m += 2.0 * cfg::PI;
    return m - cfg::PI;
}

bool settled(double target, double theta, double w) {
    return std::fabs(angleDiff(target, theta)) < cfg::g.SETTLE_ANGLE_TOL
        && std::fabs(w) < cfg::g.SETTLE_VEL_TOL;
}

State step(const State& s, double gx, double gy, double dt,
           JointForce f1, JointForce f2) {
    // RK4 conserves energy far better than Euler, which matters a lot here:
    // any artificial energy drift masks the pendulum's real chaotic flipping.
    const State k1 = deriv(s, gx, gy, f1, f2);
    const State k2 = deriv(axpy(s, 0.5 * dt, k1), gx, gy, f1, f2);
    const State k3 = deriv(axpy(s, 0.5 * dt, k2), gx, gy, f1, f2);
    const State k4 = deriv(axpy(s, dt, k3), gx, gy, f1, f2);

    const double h = dt / 6.0;
    State out{
        s.theta1 + h * (k1.theta1 + 2.0 * k2.theta1 + 2.0 * k3.theta1 + k4.theta1),
        s.theta2 + h * (k1.theta2 + 2.0 * k2.theta2 + 2.0 * k3.theta2 + k4.theta2),
        s.w1     + h * (k1.w1     + 2.0 * k2.w1     + 2.0 * k3.w1     + k4.w1),
        s.w2     + h * (k1.w2     + 2.0 * k2.w2     + 2.0 * k3.w2     + k4.w2),
    };

    // dt-independent exponential decay (unlike a flat per-substep multiplier,
    // which coupled damping to fps/substep count -- the old "rope" bug).
    if (cfg::g.FRICTION > 0.0) {
        const double damp = std::exp(-cfg::g.FRICTION * dt);
        out.w1 *= damp;
        out.w2 *= damp;
    }
    return out;
}

} // namespace phys
