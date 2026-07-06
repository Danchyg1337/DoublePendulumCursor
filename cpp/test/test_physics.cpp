// Validation harness for the platform-independent code. Emits the same
// key,value scenarios as reference.py so the two can be compared numerically.
// Build (any platform):
//   g++ -O2 -std=c++17 -Iinclude test/test_physics.cpp src/Physics.cpp src/SnapMode.cpp src/Renderer.cpp -o test_physics
#include "Physics.h"
#include "SnapMode.h"
#include "Renderer.h"
#include "Config.h"

#include <cmath>
#include <cstdio>

using phys::State;
using phys::JointForce;

static void emit4(const char* key, const State& s) {
    std::printf("%s,%.17g,%.17g,%.17g,%.17g\n", key, s.theta1, s.theta2, s.w1, s.w2);
}

int main() {
    // Scenario 1: single free-flight step.
    {
        State s{cfg::PI / 2, cfg::PI / 2, 0.0, 0.0};
        s = phys::step(s, 150.0, 2000.0, 0.001);
        emit4("free_1step", s);
    }
    // Scenario 2: one frame = 6 substeps.
    {
        State s{cfg::PI / 2, cfg::PI / 2, 0.0, 0.0};
        for (int i = 0; i < 6; ++i) {
            s = phys::step(s, 150.0, 2000.0, 0.001);
        }
        emit4("free_frame", s);
    }
    // Scenario 3: well modifier for 5 steps.
    {
        State s{0.1, cfg::PI, 0.0, 0.0};
        for (int i = 0; i < 5; ++i) {
            s = phys::step(s, 0.0, cfg::G, 0.001, JointForce::well(0.0), JointForce::none());
        }
        emit4("well_step", s);
    }
    // Scenario 4: homing modifier for 5 steps.
    {
        State s{1.0, 0.5, 2.0, -1.0};
        for (int i = 0; i < 5; ++i) {
            s = phys::step(s, 0.0, cfg::G, 0.001, JointForce::homing(0.0), JointForce::none());
        }
        emit4("homing_step", s);
    }
    // Scenario 5: full snap 'text' mode over 400 frames.
    {
        SnapMode mode("text", cfg::OCR_IBEAM, 0.0, cfg::PI);
        State s{cfg::PI / 2, cfg::PI / 2, 0.0, 0.0};
        const double frameDt = 1.0 / 144.0;
        const double subDt = frameDt / 6.0;
        int bob1Frame = -1, bob2Frame = -1;
        for (int f = 0; f < 400; ++f) {
            s = mode.advance(s, frameDt, subDt);
            if (bob1Frame < 0 && mode.bob1Locked()) bob1Frame = f;
            if (bob2Frame < 0 && mode.bob2Locked()) bob2Frame = f;
        }
        std::printf("snap_lockframes,%d,%d\n", bob1Frame, bob2Frame);
        emit4("snap_final", s);
    }
    // Scenario 6: longer free chaotic run, sanity/finite.
    {
        State s{cfg::PI / 2, cfg::PI / 2, 0.0, 0.0};
        const double subDt = (1.0 / 240.0) / 6.0;
        for (int i = 0; i < 240 * 6; ++i) {
            s = phys::step(s, -300.0, 2100.0, subDt);
        }
        emit4("free_long", s);
    }
    // Renderer sanity: report a few pixel checks on a rendered frame.
    {
        Renderer r;
        const std::uint8_t* px = r.render(cfg::PI / 2, cfg::PI / 2);
        const int N = Renderer::size();
        const int c = (cfg::HOTSPOT_Y * N + cfg::HOTSPOT_X) * 4;  // centre = red pivot
        std::printf("render_center_BGRA,%d,%d,%d,%d\n", px[c], px[c + 1], px[c + 2], px[c + 3]);
        long opaque = 0;
        for (int i = 0; i < N * N; ++i) {
            if (px[i * 4 + 3] > 200) ++opaque;
        }
        std::printf("render_opaque_count,%ld\n", opaque);
    }
    return 0;
}
