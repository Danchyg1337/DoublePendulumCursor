// SnapMode.h -- a named custom cursor "pose" that a built-in Windows cursor
// type falls into while it is showing.
//
// Platform independent: the OS cursor-slot handle is stored as an opaque
// void* set by the Windows layer, so this class carries no Windows headers and
// can be unit tested. Each mode owns its own grace timer and per-joint capture
// state, all reset every time a fresh session starts (the cursor becomes this
// type again after not being it).
#pragma once

#include "Physics.h"
#include <string>

class SnapMode {
public:
    SnapMode(std::string name, int ocrId, double theta1Target, double theta2Target);

    // Advance the simulation one frame while this mode is the active cursor.
    //
    // bob1 swings under its own power with a gentle local well around its
    // target; if a slow natural pass lets it settle, friction and the well trap
    // it for real (no teleport). Only if it hasn't settled within the grace
    // period does a strong forced spring guarantee arrival. Once bob1 is pinned,
    // bob2 gets the identical treatment, timed from when bob1 finished.
    phys::State advance(phys::State s, double frameDt, double subDt);

    // Called when this mode is NOT the active cursor this frame, so its grace
    // timer restarts cleanly next time it becomes active.
    void deactivate() { wasActive_ = false; }

    bool isActive(const void* currentHandle) const { return currentHandle == slotHandle_; }

    // Capture state (primarily useful for tests).
    bool bob1Locked() const { return bob1Locked_; }
    bool bob2Locked() const { return bob2Locked_; }

    int         ocrId()      const { return ocrId_; }
    const void* slotHandle() const { return slotHandle_; }
    void        setSlotHandle(const void* h) { slotHandle_ = h; }
    const std::string& name() const { return name_; }

private:
    std::string name_;
    int         ocrId_;
    double      t1_;
    double      t2_;
    const void* slotHandle_ = nullptr;

    // Per-session state.
    double timer_        = 0.0;
    bool   wasActive_    = false;
    bool   bob1Locked_   = false;
    bool   bob2Locked_   = false;
    double bob1LockTime_ = 0.0;
};
