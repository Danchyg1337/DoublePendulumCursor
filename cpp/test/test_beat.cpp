// Self-checking test for the beat detector + GIF dancer (portable, no data
// files). Prints one line per check and exits non-zero if any check fails.
// Build: g++ -O2 -std=c++17 -Iinclude test/test_beat.cpp src/BeatDsp.cpp
//        src/BeatWorker.cpp src/GifDecoder.cpp src/Dancer.cpp -o beat_tests
#include "BeatWorker.h"
#include "Dancer.h"
#include "GifDecoder.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("%s %s %s\n", ok ? "PASS" : "FAIL", what, detail.c_str());
    if (!ok) ++failures;
}

constexpr int SR = 48000;
constexpr double PI = 3.14159265358979323846;

// kick on every beat, noisy snare on 2 and 4, hats on the off-beats, bass line
std::vector<float> synthTrack(double bpm, double secs) {
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.f, 1.f);
    const std::size_t n = static_cast<std::size_t>(secs * SR);
    std::vector<float> y(n + SR, 0.f);
    const double beat = 60.0 / bpm;
    for (int b = 0; b * beat < secs; ++b) {
        const std::size_t p = static_cast<std::size_t>(b * beat * SR);
        double ph = 0;
        for (int i = 0; i < SR / 4; ++i) {               // kick: falling sine
            const double t = static_cast<double>(i) / SR;
            ph += 2 * PI * (50 + 100 * std::exp(-t * 30)) / SR;
            y[p + i] += static_cast<float>(std::sin(ph) * std::exp(-t * 12));
        }
        if (b % 2 == 1)
            for (int i = 0; i < SR / 5; ++i) {           // snare
                const double t = static_cast<double>(i) / SR;
                y[p + i] += static_cast<float>(0.7 * (nd(rng) * 0.6 + std::sin(2 * PI * 200 * t)) * std::exp(-t * 25));
            }
        const std::size_t h = p + static_cast<std::size_t>(beat * SR / 2);
        float prev = 0;
        for (int i = 0; i < SR / 20; ++i) {              // hat
            const double t = static_cast<double>(i) / SR;
            const float x = nd(rng);
            y[h + i] += static_cast<float>(0.3 * (x - prev) * std::exp(-t * 80));
            prev = x;
        }
    }
    for (std::size_t i = 0; i < y.size(); ++i) {
        const double t = static_cast<double>(i) / SR;
        y[i] += static_cast<float>(0.25 * std::sin(2 * PI * 55 * t) * (0.6 + 0.4 * std::sin(2 * PI * t * bpm / 60)));
        y[i] *= 0.5f;
    }
    y.resize(n);
    return y;
}

// Feed audio in 10 ms blocks (wall clock == sample clock) and step the worker
// every 0.25 s, like the capture + analysis threads do.
struct Sim {
    bpm::AudioRing ring;
    bpm::BeatWorker worker{ring};
    long long total = 0;
    Sim() { ring.restart(SR); float z[480] = {}; push(z, 480); worker.step(); }
    void push(const float* x, int n) { total += n; ring.push(x, n, static_cast<double>(total) / SR); }
    void run(const std::vector<float>& a) {
        const int step = SR / 4;
        for (std::size_t pos = 0; pos + step <= a.size(); pos += step) {
            for (int i = 0; i < step; i += 480) push(a.data() + pos + i, 480);
            worker.step();
        }
    }
};

// --- tiny GIF writer (palette + LZW with frequent clear codes) ----------------
std::vector<std::uint8_t> makeGif(int w, int h, const std::vector<std::vector<std::uint8_t>>& frames) {
    std::vector<std::uint8_t> g = {'G','I','F','8','9','a'};
    auto u16 = [&](int v) { g.push_back(v & 255); g.push_back((v >> 8) & 255); };
    u16(w); u16(h);
    g.push_back(0x81); g.push_back(0); g.push_back(0);        // 4-colour global table
    const std::uint8_t pal[12] = {0,0,0, 255,0,0, 0,255,0, 0,0,255};
    g.insert(g.end(), pal, pal + 12);
    for (const auto& idx : frames) {
        g.insert(g.end(), {0x21, 0xF9, 4, 0x01 << 0, 10, 0, 0, 0});   // colour 0 transparent
        g.push_back(0x2C); u16(0); u16(0); u16(w); u16(h); g.push_back(0);
        g.push_back(2);                                         // min code size
        std::vector<std::uint8_t> bytes;
        std::uint32_t acc = 0; int bits = 0;
        auto put = [&](int code) {
            acc |= static_cast<std::uint32_t>(code) << bits; bits += 3;
            while (bits >= 8) { bytes.push_back(acc & 255); acc >>= 8; bits -= 8; }
        };
        for (std::size_t i = 0; i < idx.size(); ++i) {
            if (i % 2 == 0) put(4);                             // clear
            put(idx[i]);
        }
        put(5);                                                 // end
        if (bits) bytes.push_back(acc & 255);
        for (std::size_t i = 0; i < bytes.size(); i += 255) {
            const std::size_t n = std::min<std::size_t>(255, bytes.size() - i);
            g.push_back(static_cast<std::uint8_t>(n));
            g.insert(g.end(), bytes.begin() + i, bytes.begin() + i + n);
        }
        g.push_back(0);
    }
    g.push_back(0x3B);
    return g;
}
} // namespace

int main() {
    // --- tempo, beat grid, intensity ---------------------------------------------
    {
        Sim sim;
        sim.run(synthTrack(128.0, 20.0));
        const auto st = sim.worker.state();
        check(st.bpm && std::abs(*st.bpm - 128) <= 1, "bpm_128", st.bpm ? std::to_string(*st.bpm) : "none");
        check(st.grid && std::fabs(60.0 / st.grid->period - 128.0) < 1.0, "grid_period");
        if (st.grid) {
            // beats were synthesised at k*60/128 s: grid phase must sit on them
            const double per = 60.0 / 128.0;
            double ph = std::fmod(st.grid->t0, per) / per;
            if (ph > 0.5) ph -= 1.0;
            check(std::fabs(ph) < 0.06, "grid_phase", std::to_string(ph));
        }
        check(st.intensity && *st.intensity >= 0.0 && *st.intensity <= 1.0, "intensity_range",
              st.intensity ? std::to_string(*st.intensity) : "none");

        sim.run(std::vector<float>(SR * 4, 0.0f));               // silence clears it
        check(!sim.worker.state().bpm, "silence_clears_bpm");
    }
    {
        Sim sim;
        sim.run(synthTrack(150.0, 20.0));
        const auto st = sim.worker.state();
        check(st.bpm && std::abs(*st.bpm - 150) <= 1, "bpm_150", st.bpm ? std::to_string(*st.bpm) : "none");
    }

    // --- GIF decoder ----------------------------------------------------------------
    std::vector<std::vector<std::uint8_t>> fr;
    for (int f = 0; f < 4; ++f) {
        std::vector<std::uint8_t> idx(4 * 4, 0);
        idx[f] = static_cast<std::uint8_t>(1 + f % 3);          // one moving pixel
        fr.push_back(idx);
    }
    const auto gifBytes = makeGif(4, 4, fr);
    {
        int n = 0; bool ok = true;
        gif::decode(gifBytes.data(), gifBytes.size(), [&](const gif::Image& im, int i) {
            const std::uint8_t* p = &im.rgba[i * 4];
            ok = ok && im.w == 4 && p[3] == 255 && im.rgba[(i == 0 ? 15 : 15) * 4 + 3] == 0;
            ++n; return true;
        });
        check(n == 4 && ok, "gif_decode", std::to_string(n));
    }

    // --- .gifbpm actor + dancer engine ------------------------------------------------
    {
        const std::string meta = "{\"name\":\"t\",\"frameDelaysMs\":[100,100,100,100],"
                                 "\"beatFrames\":[0,2],\"bpm\":300}";
        std::vector<std::uint8_t> file = {'G','B','P','M', 1, 1, 0, 0};   // loopable
        auto le32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) file.push_back((v >> (8 * i)) & 255); };
        le32(static_cast<std::uint32_t>(meta.size()));
        file.insert(file.end(), meta.begin(), meta.end());
        le32(static_cast<std::uint32_t>(gifBytes.size()));
        file.insert(file.end(), gifBytes.begin(), gifBytes.end());

        dancer::Actor a = dancer::Actor::parse(file, "x");
        a.decode(160);
        check(a.usable && std::fabs(a.bpm - 300.0) < 1e-9 && a.frames.size() == 4, "actor_parse",
              std::to_string(a.bpm));

        // steady 120 BPM grid: on every music beat a marked beat frame (0 or 2)
        dancer::DancerEngine eng({&a});
        const bpm::Grid grid{0.0, 0.5};
        int beatsChecked = 0, onBeat = 0;
        for (int i = 0; i < 2000; ++i) {
            const double t = i * 0.005;
            const auto o = eng.step(t, 120, grid);
            const double beatPos = t / 0.5;
            if (o.actor && t > 2.0 && std::fabs(beatPos - std::round(beatPos)) < 1e-9) {
                ++beatsChecked;
                if (o.frame == 0 || o.frame == 2) ++onBeat;
            }
        }
        check(beatsChecked > 10 && onBeat == beatsChecked, "dancer_on_beat",
              std::to_string(onBeat) + "/" + std::to_string(beatsChecked));
        check(!eng.step(11.0, std::nullopt, std::nullopt).actor, "dancer_hidden_without_beat");

        // first appearance must not hold a frozen frame: from any beat phase,
        // the first frame may last at most ~1 stretched GIF frame (here 250 ms
        // at 120 BPM, +35% intro stretch), and beat frames still land on beats
        double worstHold = 0;
        bool beatsOk = true;
        for (int s = 0; s < 40; ++s) {
            dancer::DancerEngine e2({&a});
            const double t0 = 1.0 + s * 0.0125;              // start phases 0..1 beat
            int firstFrame = -1;
            double firstChange = -1;
            for (int i = 0; i < 1200; ++i) {
                const double t = t0 + i * 0.005;
                const auto o = e2.step(t, 120, grid);
                if (!o.actor) continue;
                if (firstFrame < 0) firstFrame = o.frame;
                else if (firstChange < 0 && o.frame != firstFrame) firstChange = t - t0;
                const double bp = t / 0.5;
                if (t > t0 + 1.0 && std::fabs(bp - std::round(bp)) < 1e-9 && o.frame != 0 && o.frame != 2)
                    beatsOk = false;
            }
            worstHold = std::max(worstHold, firstChange);
        }
        check(worstHold <= 0.25 * 1.35 + 0.01, "dancer_no_start_stutter",
              "first frame held " + std::to_string(worstHold) + " s");
        check(beatsOk, "dancer_fresh_start_on_beat");
    }

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
