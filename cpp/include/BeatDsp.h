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

// Per-frame features (TempoEstimator::features()). The *_NV versions ignore
// the vocal range (150 Hz .. 5 kHz), so a voice on top of the music doesn't
// dilute them.
enum {
    FEAT_SUB = 0,     // sub-bass (< 140 Hz) share of the power
    FEAT_FLAT,        // spectral flatness (noisy / dense mix -> high)
    FEAT_RMS,         // frame level
    FEAT_ACT,         // level-independent onset activity
    FEAT_SUB_NV,
    FEAT_FLAT_NV,
    FEAT_ACT_NV,
    FEAT_SUB_POW,     // sub-bass power (absolute)
    FEAT_POW,         // power outside the vocal range (absolute)
    FEAT_MID_POW,     // power inside the vocal range (absolute)
    FEAT_COUNT
};

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

    // How sharply the onsets line up with a beat grid of this period (best
    // phase vs. average phase, in units of the onset envelope's std). A wrong
    // metrical level (e.g. 2/3 of the true tempo) puts half its beats between
    // hits and scores low.
    double pulseContrast(double periodSec, double lookbackSec = 6.0) const;
    // Fold the onsets at this beat period into a 24-bin phase histogram
    // (rotated so the strongest bin is 0). Returns {binary, ternary}: the
    // onset strength halfway between beats vs. at 1/3 and 2/3 of the beat.
    std::pair<double, double> subdivision(double periodSec, double lookbackSec = 6.0) const;

    double fps() const { return fps_; }
    double confidence() const { return conf_; }   // beat periodicity of the last estimate()
    const std::vector<float>& onsetEnvelope() const { return env_; }   // spectral flux per frame
    // per-frame features, see the FEAT_* indices
    const std::vector<std::array<float, FEAT_COUNT>>& features() const { return feat_; }

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
    std::vector<std::array<float, FEAT_COUNT>> feat_;
    std::array<bool, 24> nonVocal_{};       // bands outside the vocal range
    std::vector<double> prevLin_, prevBands_;
    bool havePrev_ = false;
    std::vector<float> buf_;
    std::int64_t bufStart_;
    std::optional<double> envLastCenter_;

    std::vector<double> bpms_, prior_, acc_;
    double decay_, minSec_, conf_ = 0.0, minConf_ = 0.12;
};

// ---- energy features ----------------------------------------------------------------
// What a drop has and a build-up / breakdown / vocal section doesn't: heavy
// sub-bass and loudness (measured on the 7 test tracks: drops are 6-14 dB
// stronger in sub-bass and 5-8 dB louder than the rest; a voice on top changes
// neither). Onsets per second were tried too but don't separate drops: quiet
// sections have as many small onsets as drops have big ones.
struct EnergyFeatures {
    double subDb = -120;      // sub-bass (< 140 Hz) power, dB
    double loudDb = -120;     // frame level (rms), dB
    bool valid = false;
};
// Mean over the last `windowSec`.
EnergyFeatures energyFeatures(const TempoEstimator& est, double windowSec);

// ---- intensity ------------------------------------------------------------------
class IntensityMeter {
public:
    // fastWindowSec = 0 and attackTau = 0.4 reproduce BPMidentifier exactly;
    // the defaults react to a drop much sooner (see update()).
    //
    // Modes:
    //   Original    : BPMidentifier's features (sub-bass share, flatness,
    //                 loudness, activity), half absolute, half relative to the
    //                 last 60 s of the track.
    //   VocalRobust : the same, but the features ignore the vocal range and a
    //                 vocal-range level term is added.
    //   Energy      : what a drop has: sub-bass and loudness relative to the
    //                 track's recent peak, plus a small tempo bonus. Holds up
    //                 through a long drop (the 60 s-relative modes decay) and
    //                 isn't fooled by noisy build-ups or vocals. Default.
    enum class Mode { Original, VocalRobust, Energy };
    explicit IntensityMeter(double updateSec = 0.25, double memorySec = 60.0,
                            double fastWindowSec = 0.5, double attackTau = 0.15,
                            Mode mode = Mode::Energy);
    void reset();
    // bpm: the current (locked) tempo, if any -- used by the Energy mode
    std::optional<double> update(const TempoEstimator& est, std::optional<int> bpm = std::nullopt);
    std::optional<double> value() const { return value_; }
    // Debug: the last update's scores before smoothing -- 2 s window, short
    // (fast-drop) window, and whether the short one was used.
    std::optional<double> lastSlow() const { return lastSlow_; }
    std::optional<double> lastFast() const { return lastFast_; }
    bool fastUsed() const { return fastUsed_; }
    // Energy mode debug: bass / loudness vs. the track's peak (0..1)
    std::optional<double> lastBass() const { return lastBass_; }
    std::optional<double> lastLoud() const { return lastLoud_; }
private:
    std::optional<double> updateEnergy(const TempoEstimator& est, std::optional<int> bpm);
    void smooth(double target);

    double dt_, alpha_, fastWindow_, attackTau_;
    Mode mode_;
    bool vocalRobust_;
    // Energy mode: decaying peaks of the 2 s sub-bass / loudness levels (dB)
    std::optional<double> subRef_, loudRef_;
    double silentFor_ = 0;
    bool startPhase_ = false;   // no peak reached yet since the app started
    std::optional<double> lastSlow_, lastFast_, lastBass_, lastLoud_;
    bool fastUsed_ = false;
    bool   init_ = false;
    std::array<double, 5> mu_{}, var_{};
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

// ---- beat lock -----------------------------------------------------------------
// Replacement for Tracker: once the readings agree on a tempo, the beat grid is
// LOCKED and simply runs on. Readings that fit the locked pulse (the same tempo
// or a 2x / 1/2x / 3:2 relative of it) never move the speed; same-tempo
// readings nudge the tempo very slowly and the phase only by the median of the
// recent beat times, so a brief vocal or fill can't drag the GIF off the beat.
// A different tempo is adopted only after it has held for SWITCH_SEC, and the
// octave only flips when the other octave dominates the readings for ~8 s.
class BeatLock {
public:
    BeatLock(double octEnter, double octExit, bool strictOctave);
    void reset();
    // r = this update's analysis (nullopt = no beat found); music = the
    // audio sounds like music (MusicGate); now = nowSec() of the update.
    void update(const std::optional<Analysis>& r, bool music, double now);
    bool locked() const { return locked_; }

    std::optional<int>  shown;
    std::optional<Grid> grid;

    static constexpr double DT = 0.25;            // update interval
    static constexpr double TOL = 0.03;           // "same tempo" tolerance
    static constexpr int    CONFIRM = 3;          // agreeing readings to lock
    static constexpr double SWITCH_SEC = 3.0;     // a new tempo must hold this long
    static constexpr double HOLD_SEC = 4.0;       // keep the beat through gaps this long
    static constexpr double NO_MUSIC_SEC = 1.0;   // drop the lock after this much non-music
private:
    enum Kind { SAME, DOUBLE, HALF, THREE_TWO, OTHER };
    Kind classify(double bpmValue) const;
    void lockTo(double bpmValue, std::optional<double> beatTime, double now);
    void publish(double now);

    OctaveVote octave_;
    bool locked_ = false;
    double bpm_ = 0, t0_ = 0;
    bool havePhase_ = false;
    std::vector<double> pending_;                 // acquisition readings
    std::vector<double> phaseErr_;                // recent phase errors (beats)
    std::vector<int> kinds_;                      // recent reading kinds (~8 s)
    std::vector<double> halfPhase_;               // phase of HALF readings (beats)
    double cand_ = 0, candTime_ = 0;              // switch candidate
    std::optional<double> candBeat_;
    double miss_ = 0, noMusic_ = 0;
};

// ---- music / speech gate ------------------------------------------------------
// Speech has frequent, irregular pauses between words; music is continuous,
// or, when sparse, its gaps repeat with the beat. Two cues over the last few s:
//   pause share  : near-silent frames (< 25% of the mean level) in 3 s --
//                  music ~0-5% (sparse beats up to ~20%), speech 25-45%
//   periodicity  : the tempo estimator's confidence -- speech <= ~0.12,
//                  music with a beat ~0.3-0.5
// Speech = many pauses AND no steady beat. Hysteresis + smoothing.
// Call after TempoEstimator::estimate() so the confidence is current.
class MusicGate {
public:
    void reset() { score_.reset(); conf_.reset(); music_ = false; }
    bool update(const TempoEstimator& est);   // -> music?
    bool music() const { return music_; }
    std::optional<double> score() const { return score_; }   // smoothed pause share
    std::optional<double> beatConf() const { return conf_; }  // smoothed periodicity
    static constexpr double WINDOW_SEC = 3.0;
    static constexpr double OFF_ABOVE = 0.16;   // more pauses than this ...
    static constexpr double CONF_SPEECH = 0.15; // ... and a weaker beat -> speech
    static constexpr double ON_BELOW = 0.10;    // fewer pauses -> music again
    static constexpr double CONF_MUSIC = 0.22;  // or a clear beat -> music again
private:
    std::optional<double> score_, conf_;
    bool music_ = false;
};

} // namespace bpm
