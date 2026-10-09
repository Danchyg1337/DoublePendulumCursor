// BeatWorker.h -- the audio ring buffer and the analysis loop.
//
// AudioRing    : C++ twin of bpm_common.LoopbackCapture's buffer half: keeps
//                the most recent `keepSec` of mono audio, a monotonic sample
//                counter, a sample-index -> wall-clock mapping and a
//                "generation" that bumps on every (re)start / device change.
//                The Windows loopback thread (LoopbackCapture) pushes into it.
// BeatWorker   : C++ twin of bpm_detector.ClassicWorker + analyze(): every
//                0.25 s feeds new audio to the TempoEstimator, updates the
//                Tracker (BPM + beat grid) and the IntensityMeter.
//
// Platform independent: tests drive it with synthetic audio.
#pragma once

#include "BeatDsp.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace bpm {

class AudioRing {
public:
    explicit AudioRing(double keepSec = 12.0) : keepSec_(keepSec) {}

    // Start a new stream (device opened / changed): clears audio and clock
    // mapping, sets the sample rate, then bumps the generation.
    void restart(int sampleRate);
    // Append mono samples that arrived at wall time `now` (nowSec()).
    void push(const float* mono, std::size_t n, double now);

    int sampleRate() const;
    int generation() const { return generation_.load(); }
    // Last `seconds` of audio -> out; returns the total sample counter.
    std::int64_t snapshot(double seconds, std::vector<float>& out) const;
    // Samples received since counter `lastTotal` -> out; returns new total.
    std::int64_t readNew(std::int64_t lastTotal, std::vector<float>& out) const;
    // Wall time at which a given (absolute) sample was captured.
    std::optional<double> sampleTime(double sampleIndex) const;

private:
    mutable std::mutex m_;
    double keepSec_;
    int sr_ = 48000;
    std::vector<float> buf_;
    std::int64_t total_ = 0;
    std::deque<double> offsets_;      // recent (arrival_time - total/SR)
    std::atomic<int> generation_{0};
};

struct BeatState {
    std::optional<int>    bpm;        // displayed (stabilised) tempo
    std::optional<Grid>   grid;       // beat grid in nowSec() time
    std::optional<double> intensity;  // 0..1
    // debug details of the last intensity update
    std::optional<double> intensitySlow;  // score of the 2 s window
    std::optional<double> intensityFast;  // score of the 0.5 s window
    bool fastUsed = false;                // the 0.5 s window drove it
    std::optional<double> intensityBass;  // energy mode: bass vs. track peak
    std::optional<double> intensityLoud;  // energy mode: loudness vs. track peak
    bool music = true;                    // MusicGate verdict
    std::optional<double> musicScore;     // smoothed pause share (speech ~0.3)
    std::optional<double> rawBpm;         // this update's raw estimate
    double updatedAt = 0;                 // nowSec() of the last update
};

class BeatWorker {
public:
    static constexpr double UPDATE_SEC = 0.25;

    struct Options {
        bool fastDrop = true;      // intensity reacts to drops in ~0.5 s
        // 0 = BPMidentifier original, 1 = vocal-robust, 2 = energy (bass +
        // loudness vs. the track's peak, tempo bonus) -- see IntensityMeter
        int intensityMode = 2;
        bool beatLock = true;      // lock tempo + phase once agreed
        bool musicGate = true;     // no beat / GIF for speech-only audio
    };
    explicit BeatWorker(AudioRing& ring) : BeatWorker(ring, Options{}) {}
    BeatWorker(AudioRing& ring, const Options& opt);
    // One analysis update (the body of ClassicWorker.run's loop).
    void step();
    BeatState state() const;

private:
    std::optional<Analysis> analyze();
    // snare_envelope_v2 over the last `sec` seconds, with per-frame band levels
    // cached by absolute frame index so each update only FFTs the new frames.
    // Returns the envelope and the absolute sample index of its frame 0.
    std::vector<double> snareEnvelope(double sec, std::int64_t& startSample);

    AudioRing& ring_;
    Options opt_;
    Tracker tracker_;
    BeatLock lock_;
    MusicGate gate_;
    IntensityMeter meter_;
    std::unique_ptr<TempoEstimator> est_;
    int gen_ = -1;
    int sr_ = 0;
    std::int64_t lastTotal_ = 0;

    std::unique_ptr<SnareLevels> snare_;
    std::deque<std::array<float, 3>> snareCache_;
    std::int64_t snareFirstK_ = 0;

    mutable std::mutex outM_;
    BeatState out_;
};

} // namespace bpm
