// Dancer.h -- C++ port of BPMidentifier's gif_dancer.py.
//
// Plays the .gifbpm "actor" whose tempo is closest to the music, with its
// marked beat frames landing on the music's beats:
//   pre-roll  frame 0 .. first beat frame, started early so the first beat
//             frame lands exactly on an upcoming music beat;
//   beats     every gap between two beat frames is stretched/squeezed to last
//             exactly one music beat;
//   post-roll last beat frame .. end of the GIF (non-repeatable GIFs).
// Nothing is shown while no beat is identified or while the music is not
// intense enough (SHOW / HIDE intensity hysteresis). Sudden beat jumps are
// ignored unless they hold. GIFs take turns (closest tempo not played yet);
// repeatable GIFs loop for >= 3 s and change at the end of a loop.
//
// Platform independent: the engine is unit tested, the cursor draws the frame.
#pragma once

#include "BeatDsp.h"
#include "GifDecoder.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace dancer {

struct Actor {
    std::string name;
    bool loopable = false;
    std::vector<int> delays;              // ms per frame
    std::vector<double> starts;           // frame start times (ms)
    double duration = 0;                  // ms
    std::vector<double> beatTimes;        // ms, of the marked beat frames
    struct Seg { double start, len; };
    std::vector<Seg> segments;            // beat-to-beat gaps in GIF time
    bool usable = false;
    double bpm = 0;
    std::vector<std::uint8_t> gifBytes;
    std::vector<gif::Image> frames;       // scaled, premultiplied RGBA

    // Parse a .gifbpm file (throws std::runtime_error on bad files).
    static Actor load(const std::string& path);
    static Actor parse(const std::vector<std::uint8_t>& data, const std::string& fallbackName);
    // Decode + scale the frames to fit a maxSide x maxSide box.
    // `cancel` (optional) aborts decoding early when set.
    void decode(int maxSide, const std::atomic<bool>* cancel = nullptr);
    int frameAtTime(double tMs) const;
};

std::vector<std::unique_ptr<Actor>> loadActors(const std::string& folder);

// One scheduled playback of an actor, positioned in music-beat units.
struct Run {
    const Actor* actor;
    double ratio;            // GIF beats per music beat
    double phiB;             // music-beat position of the 1st beat frame
    double prePhi;           // length of the pre-roll (music beats)
    double phiStart;
    std::optional<int> endCycle;      // nullopt = loops until planned
    std::optional<double> postPhi;
    std::optional<double> startedAt;

    Run(const Actor* a, double ratio, double phiB, double prePhi);
    double segPhi() const { return 1.0 / ratio; }
    double lastBeatPhi(int cycle = 0) const;
    double gifTime(double phi) const;
    int frame(double phi) const { return actor->frameAtTime(gifTime(phi)); }
};

class DancerEngine {
public:
    explicit DancerEngine(std::vector<const Actor*> actors);
    void reset();
    struct Out { const Actor* actor = nullptr; int frame = 0; };
    // actor == nullptr -> show nothing
    Out step(double now, std::optional<int> bpm, std::optional<bpm::Grid> grid, double offset = 0.0);

    double phi() const { return phi_.value_or(0.0); }

private:
    void advance(double now, const bpm::Grid& grid, double offset);
    std::pair<const Actor*, double> pick(double bpm, const Actor* exclude = nullptr);
    void startFresh(double bpm);
    void planNext(const Actor* actor, double ratio, int cycle);

    std::vector<const Actor*> actors_;
    std::set<const Actor*> used_;
    std::optional<double> phi_;
    double lastT_ = 0, period_ = 0;
    std::optional<Run> cur_, next_;
    std::optional<std::pair<double, double>> phaseJump_, tempoJump_;
    bool following_ = false;
};

// The whole dancer as the cursor uses it: loads + decodes the actors on a
// background thread, applies the intensity "red zone" hysteresis and returns
// the frame to draw (or nullptr).
class Dancer {
public:
    Dancer(std::string actorsDir, int maxSide, double showIntensity, double hideIntensity);
    ~Dancer();
    Dancer(const Dancer&) = delete;
    Dancer& operator=(const Dancer&) = delete;

    const gif::Image* update(double now, const std::optional<int>& bpm,
                             const std::optional<bpm::Grid>& grid,
                             const std::optional<double>& intensity, double offsetSec);
    bool ready() const { return ready_.load(); }
    bool showing() const { return inRed_; }   // intensity is in the "show" zone

private:
    bool redZone(const std::optional<double>& intensity);

    std::vector<std::unique_ptr<Actor>> actors_;
    std::unique_ptr<DancerEngine> engine_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> cancel_{false};
    std::thread loader_;
    double show_, hide_;
    bool inRed_ = false;
};

} // namespace dancer
