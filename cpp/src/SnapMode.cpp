#include "SnapMode.h"
#include "Config.h"

#include <utility>

using phys::JointForce;
using phys::State;

SnapMode::SnapMode(std::string name, int ocrId, double theta1Target, double theta2Target)
    : name_(std::move(name)), ocrId_(ocrId), t1_(theta1Target), t2_(theta2Target) {}

State SnapMode::advance(State s, double frameDt, double subDt) {
    if (!wasActive_) {
        timer_        = 0.0;
        bob1Locked_   = false;
        bob2Locked_   = false;
        bob1LockTime_ = 0.0;
    } else {
        timer_ += frameDt;
    }
    wasActive_ = true;

    for (int i = 0; i < cfg::SUBSTEPS; ++i) {
        if (!bob1Locked_) {
            // bob1 falls in on its own; the well captures a slow pass, the
            // homing spring guarantees arrival once the grace period expires.
            const bool assist = timer_ >= cfg::SNAP_GRACE_PERIOD;
            const JointForce f1 = assist ? JointForce::homing(t1_) : JointForce::well(t1_);
            s = phys::step(s, 0.0, cfg::G, subDt, f1, JointForce::none());

            if (phys::settled(t1_, s.theta1, s.w1)) {
                s.theta1 = t1_;
                s.w1 = 0.0;
                bob1Locked_ = true;
                bob1LockTime_ = timer_;
            }
        } else if (!bob2Locked_) {
            // Keep bob1 pinned; give bob2 the same well-then-homing treatment,
            // timed from when bob1 finished.
            s.theta1 = t1_;
            s.w1 = 0.0;
            const double bob2Timer = timer_ - bob1LockTime_;
            const bool assist = bob2Timer >= cfg::SNAP_GRACE_PERIOD;
            const JointForce f2 = assist ? JointForce::homing(t2_) : JointForce::well(t2_);
            s = phys::step(s, 0.0, cfg::G, subDt, JointForce::pin(), f2);

            if (phys::settled(t2_, s.theta2, s.w2)) {
                s.theta2 = t2_;
                s.w2 = 0.0;
                bob2Locked_ = true;
            }
        } else {
            // Both captured -- hold the pose.
            s.theta1 = t1_;
            s.w1 = 0.0;
            s.theta2 = t2_;
            s.w2 = 0.0;
        }
    }
    return s;
}
