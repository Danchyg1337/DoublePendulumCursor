// BeatDsp.h -- C++ port of BPMidentifier's classic (no-model) beat detector.
//
// Mirrors bpm_detector.py + bpm_common.py:
//   TempoEstimator : spectral-flux onset envelope + autocorrelation with a
//                    tempo prior; beat phase by folding the onset envelope.
//   IntensityMeter : 0 (calm) .. 1 (intense), absolute + track-relative.
//   snare*         : "backbeat rule" octave check (DnB 87 -> 174).
//   Stabilizer / OctaveVote / BeatClock / Tracker : noisy estimates ->
//                    calm displayed BPM + phase-locked beat grid.
//
// Platform independent (no Windows headers) so it can be unit tested anywhere.
#pragma once

#include <array>
#include <complex>
#include <cstdint>
#include <optional>
#include <vector>

namespace bpm {

// Monotonic wall clock in seconds (the C++ twin of time.perf_counter()).
double nowSec();

// ---- small numeric helpers (numpy semantics) ---------------------------------
double median(std::vector<double> v);                  // np.median
double percentile(std::vector<double> v, double q);    // np.percentile (linear)
double interp(double x, const std::vector<double>& fp); // np.interp(x, arange(n), fp)
double pyRound(double x);                              // Python round(): half to even
double pyMod(double a, double m);                      // Python a % m (m > 0)

// In-place radix-2 complex FFT (size must be a power of two).
class FFT {
public:
    explicit FFT(int n);
    int size() const { return n_; }
    void forward(std::vector<std::complex<double>>& a) const;
    void inverse(std::vector<std::complex<double>>& a) const;   // scaled by 1/n
private:
    int n_;
    std::vector<int> rev_;
    std::vector<std::complex<double>> tw_;
};

// Hann window as np.hanning(n).
std::vector<double> hanning(int n);

// ---- tempo estimator ----------------------------------------------------------
struct Estimate {
    std::optional<double> bpm;
    double confidence = 0.0;
};

class TempoEstimator {
public:
    TempoEstimator(double sr, std::int64_t startSample = 0, double windowSec = 8.0,
                   double bpmMin = 60.0, double bpmMax = 200.0,
                   double priorCenter = 120.0, double priorOctaves = 1.0,
                   double decay = 0.8, double minSec = 2.0);

    void reset();
    void feed(const float* x, std::size_t n);
    Estimate estimate();
    // Absolute sample index of the most recent beat, or nullopt.
    std::optional<double> beatPhase(double periodSec, double lookbackSec = 4.0) const;

    double fps() const { return fps_; }
    // per-frame features: sub-bass share, flatness, rms, activity
    const std::vector<std::array<float, 4>>& features() const { return feat_; }

private:
    template <class T> void appendTrim(std::vector<T>& dst, const std::vector<T>& src);

    double sr_;
    int    hop_, nfft_;
    double fps_;
    FFT    fft_;
    std::vector<double> window_;
    std::vector<std::vector<int>> bands_;   // bin indices per log band (24)
    int    nLowPhase_, nSub_;
    std::size_t maxFrames_;

    std::vector<float> env_, envLow_, rms_;
    std::vector<std::array<float, 4>> feat_;
    std::vector<double> prevLin_, prevBands_;
    bool havePrev_ = false;
    std::vector<float> buf_;
    std::int64_t bufStart_;
    std::optional<double> envLastCenter_;

    std::vector<double> bpms_, prior_, acc_;
    double decay_, minSec_, conf_ = 0.0, minConf_ = 0.12;
};

// ---- intensity ------------------------------------------------------------------
class IntensityMeter {
public:
    // fastWindowSec = 0 and attackTau = 0.4 reproduce BPMidentifier exactly;
    // the defaults react to a drop much sooner (see update()).
    explicit IntensityMeter(double updateSec = 0.25, double memorySec = 60.0,
                            double fastWindowSec = 0.5, double attackTau = 0.15);
    void reset();
    std::optional<double> update(const TempoEstimator& est);
    std::optional<double> value() const { return value_; }
private:
    double dt_, alpha_, fastWindow_, attackTau_;
    bool   init_ = false;
    std::array<double, 4> mu_{}, var_{};
    std::optional<double> value_;
};

// ---- snare / octave ---------------------------------------------------------------
inline constexpr double DOUBLE_MAX_BPM = 195.0;
inline constexpr int    SNARE_HOP  = 240;
inline constexpr int    SNARE_NFFT = 1024;

// Per-frame band levels used by the v2 snare envelope:
//   [0] crack 1-4 kHz, [1] noise tail 5-10 kHz, [2] kick 30-90 Hz
// (each = sqrt of summed magnitude in the band).
class SnareLevels {
public:
    explicit SnareLevels(double sr);
    std::array<float, 3> frame(const float* x) const;   // SNARE_NFFT samples
    double sr() const { return sr_; }
private:
    double sr_;
    FFT    fft_;
    std::vector<double> win_;
    std::vector<int> crack_, tail_, kick_;
};

// snare_envelope_v2 from per-frame levels (n frames -> n values, env[0] = 0).
std::vector<double> snareEnvelopeFromLevels(const std::vector<std::array<float, 3>>& lv);
// Convenience: full snare_envelope_v2(audio, sr).
std::vector<double> snareEnvelopeV2(const std::vector<float>& audio, double sr);
// snare_cycle_ratio: ~0.5 backbeat (keep), ~0 snare every beat (double).
std::optional<double> snareCycleRatio(const std::vector<double>& env, double efps,
                                      double period, double lastBeat, double minSec = 2.0);

class Stabilizer {
public:
    Stabilizer(double tol = 0.025, int confirm = 2, int sw = 3, double alpha = 0.3, int hold = 6);
    void reset();
    std::optional<int> update(std::optional<double> bpm);
private:
    bool close(double a, double b) const;
    double tol_, alpha_;
    int confirm_, switch_, hold_;
    std::optional<double> value_;
    std::optional<int> shown_;
    std::vector<double> pending_;
    int misses_ = 0;
};

class OctaveVote {
public:
    OctaveVote(double enter, double exit, bool strict);
    void reset();
    int  factorFor(double rawBpm, std::optional<double> ratio);
    bool settled(double rawBpm) const;
private:
    double enter_, exit_;
    bool strict_;
    double score_ = 0.5;
    bool doubled_ = false;
    int n_ = 0;
};

struct Grid { double t0; double period; };   // beat at t0 (+k*period), wall seconds

class BeatClock {
public:
    void reset() { have_ = false; misses_ = 0; }
    void update(double tBeat, double period);
    std::optional<Grid> grid() const;
private:
    bool have_ = false;
    double t0_ = 0, period_ = 0;
    int misses_ = 0;
};

struct Analysis {
    double bpm = 0, period = 0;
    std::optional<double> beatTime;
    std::optional<double> ratio;
};

class Tracker {
public:
    Tracker(int confirm, double octEnter, double octExit, bool strictOctave);
    void reset();
    void update(const std::optional<Analysis>& r);
    std::optional<int>  shown;
    std::optional<Grid> grid;
private:
    Stabilizer stab_;
    OctaveVote octave_;
    BeatClock  clock_;
    int factor_ = 1;
};

} // namespace bpm
