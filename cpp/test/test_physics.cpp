// Validation harness for the platform-independent code. Emits `key,value`
// scenarios compared numerically against reference.py by compare.py.
// Build: g++ -O2 -std=c++17 -Iinclude test/test_physics.cpp \
//        src/Physics.cpp src/SnapMode.cpp src/Renderer.cpp src/ConfigFile.cpp -o physics_tests
#include "Physics.h"
#include "SnapMode.h"
#include "Renderer.h"
#include "Config.h"
#include "ConfigFile.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

using phys::State;
using phys::JointForce;

static void emit4(const char* key, const State& s) {
    std::printf("%s,%.17g,%.17g,%.17g,%.17g\n", key, s.theta1, s.theta2, s.w1, s.w2);
}

static void runSnap(const char* tag, double t1, double t2) {
    SnapMode mode(tag, 0, t1, t2);
    State s{cfg::PI / 2, cfg::PI / 2, 0.0, 0.0};
    const double frameDt = 1.0 / 144.0;
    const double subDt = frameDt / 6.0;
    int b1 = -1, b2 = -1;
    for (int f = 0; f < 400; ++f) {
        s = mode.advance(s, frameDt, subDt);
        if (b1 < 0 && mode.bob1Locked()) b1 = f;
        if (b2 < 0 && mode.bob2Locked()) b2 = f;
    }
    std::printf("%s_lockframes,%d,%d\n", tag, b1, b2);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s_final", tag);
    emit4(buf, s);
}

int main() {
    // --- Default-physics scenarios (cfg::g is at its defaults here) ---
    {
        State s{cfg::PI / 2, cfg::PI / 2, 0.0, 0.0};
        s = phys::step(s, 150.0, 2000.0, 0.001);
        emit4("free_1step", s);
    }
    {
        State s{cfg::PI / 2, cfg::PI / 2, 0.0, 0.0};
        for (int i = 0; i < 6; ++i) s = phys::step(s, 150.0, 2000.0, 0.001);
        emit4("free_frame", s);
    }
    {
        State s{0.1, cfg::PI, 0.0, 0.0};
        for (int i = 0; i < 5; ++i)
            s = phys::step(s, 0.0, cfg::g.G, 0.001, JointForce::well(0.0), JointForce::none());
        emit4("well_step", s);
    }
    {
        State s{1.0, 0.5, 2.0, -1.0};
        for (int i = 0; i < 5; ++i)
            s = phys::step(s, 0.0, cfg::g.G, 0.001, JointForce::homing(0.0), JointForce::none());
        emit4("homing_step", s);
    }

    // Snap: text (vertical) and the new NW-SE diagonal.
    runSnap("snap", cfg::g.TEXT_THETA1, cfg::g.TEXT_THETA2);     // -> snap_lockframes / snap_final
    runSnap("nwse", cfg::g.NWSE_THETA1, cfg::g.NWSE_THETA2);

    {
        State s{cfg::PI / 2, cfg::PI / 2, 0.0, 0.0};
        const double subDt = (1.0 / 240.0) / 6.0;
        for (int i = 0; i < 240 * 6; ++i) s = phys::step(s, -300.0, 2100.0, subDt);
        emit4("free_long", s);
    }

    // Renderer sanity (defaults): centre pixel = opaque red pivot.
    {
        Renderer r;
        const std::uint8_t* px = r.render(cfg::PI / 2, cfg::PI / 2);
        const int N = r.size();
        const int c = (cfg::g.hotspot() * N + cfg::g.hotspot()) * 4;
        std::printf("render_center_BGRA,%d,%d,%d,%d\n", px[c], px[c + 1], px[c + 2], px[c + 3]);
        long opaque = 0;
        for (int i = 0; i < N * N; ++i) if (px[i * 4 + 3] > 200) ++opaque;
        std::printf("render_opaque_count,%ld\n", opaque);
    }

    // --- Config-file parsing (mutates cfg::g, so runs LAST) ---
    {
        const std::string path = "._pendulum_test.conf";
        std::ofstream(path)
            << "# test override\n"
            << "L1 = 30\n"
            << "G = 1000\n"
            << "COLOR_PIVOT = 10,20,30\n"
            << "NWSE_THETA1 = -100   # degrees\n"
            << "UNKNOWN_KEY = 5\n";
        cfg::loadConfig(path);
        std::remove(path.c_str());
        std::printf("config_L1,%.17g\n", cfg::g.L1);
        std::printf("config_G,%.17g\n", cfg::g.G);
        std::printf("config_pivot,%d,%d,%d\n",
                    cfg::g.COLOR_PIVOT.r, cfg::g.COLOR_PIVOT.g, cfg::g.COLOR_PIVOT.b);
        std::printf("config_nwse1,%.17g\n", cfg::g.NWSE_THETA1);
    }
    return 0;
}
