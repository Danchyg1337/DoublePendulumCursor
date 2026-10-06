// BeatDsp.cpp -- see BeatDsp.h. Line-by-line port of BPMidentifier's
// bpm_detector.py (TempoEstimator, IntensityMeter) and bpm_common.py
// (snare envelope / cycle ratio, Stabilizer, OctaveVote, BeatClock, Tracker).
#include "BeatDsp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

namespace bpm {

namespace {
constexpr double PI = 3.14159265358979323846;
using cd = std::complex<double>;

// np.fft.rfftfreq(n, 1/sr)
std::vector<double> rfftfreq(int n, double sr) {
    std::vector<double> f(static_cast<std::size_t>(n / 2 + 1));
    for (std::size_t k = 0; k < f.size(); ++k) f[k] = k * sr / n;
    return f;
}

std::vector<int> bandBins(const std::vector<double>& f, double lo, double hi) {
    std::vector<int> idx;
    for (std::size_t k = 0; k < f.size(); ++k)
        if (f[k] >= lo && f[k] < hi) idx.push_back(static_cast<int>(k));
    return idx;
}

// |rfft(x * win)| for a real frame of fft.size() samples.
void rfftMag(const FFT& fft, const float* x, const std::vector<double>& win,
             std::vector<cd>& scratch, std::vector<double>& mag) {
    const int n = fft.size();
    scratch.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) scratch[i] = cd(x[i] * win[i], 0.0);
    fft.forward(scratch);
    mag.resize(static_cast<std::size_t>(n / 2 + 1));
    for (int k = 0; k <= n / 2; ++k) mag[k] = std::abs(scratch[k]);
}
} // namespace

double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---- helpers ----------------------------------------------------------------
double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double pos = q / 100.0 * (v.size() - 1);
    const std::size_t i = static_cast<std::size_t>(std::floor(pos));
    if (i + 1 >= v.size()) return v.back();
    const double fr = pos - i;
    return v[i] + (v[i + 1] - v[i]) * fr;
}

double interp(double x, const std::vector<double>& fp) {
    const std::size_t n = fp.size();
    if (n == 0) return 0.0;
    if (x <= 0.0) return fp[0];
    if (x >= static_cast<double>(n - 1)) return fp[n - 1];
    const std::size_t i = static_cast<std::size_t>(x);
    const double fr = x - i;
    return fp[i] + (fp[i + 1] - fp[i]) * fr;
}

double pyRound(double x) {
    const double r = std::round(x);
    if (std::fabs(x - std::trunc(x)) == 0.5) {          // exact tie -> even
        const double f = std::floor(x);
        return std::fmod(f, 2.0) == 0.0 ? f : f + 1.0;
    }
    return r;
}

double pyMod(double a, double m) {
    double r = std::fmod(a, m);
    if (r < 0) r += m;
    return r;
}

FFT::FFT(int n) : n_(n), rev_(static_cast<std::size_t>(n)), tw_(static_cast<std::size_t>(n / 2)) {
    int lg = 0;
    while ((1 << lg) < n) ++lg;
    for (int i = 0; i < n; ++i) {
        int r = 0;
        for (int b = 0; b < lg; ++b) if (i & (1 << b)) r |= 1 << (lg - 1 - b);
        rev_[i] = r;
    }
    for (int i = 0; i < n / 2; ++i) tw_[i] = std::polar(1.0, -2.0 * PI * i / n);
}

void FFT::forward(std::vector<cd>& a) const {
    const int n = n_;
    for (int i = 0; i < n; ++i) if (i < rev_[i]) std::swap(a[i], a[rev_[i]]);
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len / 2, step = n / len;
        for (int i = 0; i < n; i += len)
            for (int j = 0; j < half; ++j) {
                const cd u = a[i + j];
                const cd v = a[i + j + half] * tw_[j * step];
                a[i + j] = u + v;
                a[i + j + half] = u - v;
            }
    }
}

void FFT::inverse(std::vector<cd>& a) const {
    for (auto& z : a) z = std::conj(z);
    forward(a);
    const double s = 1.0 / n_;
    for (auto& z : a) z = std::conj(z) * s;
}

std::vector<double> hanning(int n) {
    std::vector<double> w(static_cast<std::size_t>(n));
    if (n == 1) { w[0] = 1.0; return w; }
    for (int i = 0; i < n; ++i) w[i] = 0.5 - 0.5 * std::cos(2.0 * PI * i / (n - 1));
    return w;
}

// ---- TempoEstimator ---------------------------------------------------------
namespace {
int nfftFor(int hop) {
    int lg = static_cast<int>(std::ceil(std::log2(4.0 * hop)));
    return 1 << lg;
}
} // namespace

TempoEstimator::TempoEstimator(double sr, std::int64_t startSample, double windowSec,
                               double bpmMin, double bpmMax, double priorCenter,
                               double priorOctaves, double decay, double minSec)
    : sr_(sr),
      hop_(static_cast<int>(pyRound(sr / 172.0))),       // ~172 frames/s
      nfft_(nfftFor(hop_)),
      fps_(sr / hop_),
      fft_(nfft_),
      window_(hanning(nfft_)),
      bufStart_(startSample),
      decay_(decay), minSec_(minSec) {
    // log-spaced band matrix (30 Hz .. 12 kHz, 24 bands)
    const std::vector<double> freqs = rfftfreq(nfft_, sr);
    const double top = std::min(12000.0, sr / 2 - 1);
    std::vector<double> edges(25);
    for (int i = 0; i < 25; ++i) edges[i] = 30.0 * std::pow(top / 30.0, i / 24.0);
    edges[0] = 30.0; edges[24] = top;
    bands_.resize(24);
    for (int i = 0; i < 24; ++i) {
        bands_[i] = bandBins(freqs, edges[i], edges[i + 1]);
        if (bands_[i].empty()) {     // very narrow low bands: take nearest bin
            const double mid = (edges[i] + edges[i + 1]) / 2;
            int best = 0;
            for (std::size_t k = 1; k < freqs.size(); ++k)
                if (std::fabs(freqs[k] - mid) < std::fabs(freqs[best] - mid)) best = static_cast<int>(k);
            bands_[i].push_back(best);
        }
    }
    maxFrames_ = static_cast<std::size_t>(windowSec * fps_);
    nLowPhase_ = static_cast<int>(std::lower_bound(edges.begin(), edges.end(), 400.0) - edges.begin());
    nSub_      = static_cast<int>(std::lower_bound(edges.begin(), edges.end(), 140.0) - edges.begin());

    for (double b = bpmMin; b <= bpmMax + 1e-9; b += 0.25) bpms_.push_back(b);
    // (rebuild exactly like np.arange to avoid accumulated rounding)
    for (std::size_t i = 0; i < bpms_.size(); ++i) bpms_[i] = bpmMin + 0.25 * i;
    prior_.resize(bpms_.size());
    for (std::size_t i = 0; i < bpms_.size(); ++i) {
        const double z = std::log2(bpms_[i] / priorCenter) / priorOctaves;
        prior_[i] = std::exp(-0.5 * z * z);
    }
    acc_.assign(bpms_.size(), 0.0);
}

void TempoEstimator::reset() {
    env_.clear(); rms_.clear(); envLow_.clear(); feat_.clear();
    std::fill(acc_.begin(), acc_.end(), 0.0);
    conf_ = 0.0;
}

template <class T>
void TempoEstimator::appendTrim(std::vector<T>& dst, const std::vector<T>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
    if (dst.size() > maxFrames_) dst.erase(dst.begin(), dst.end() - static_cast<std::ptrdiff_t>(maxFrames_));
}

void TempoEstimator::feed(const float* x, std::size_t count) {
    buf_.insert(buf_.end(), x, x + count);
    if (buf_.size() < static_cast<std::size_t>(nfft_)) return;
    const std::size_t n = (buf_.size() - nfft_) / hop_ + 1;
    envLastCenter_ = static_cast<double>(bufStart_ + static_cast<std::int64_t>((n - 1) * hop_) + nfft_ / 2);

    std::vector<float> flux(n), fluxLow(n), rmsv(n);
    std::vector<std::array<float, 4>> feat(n);
    std::vector<cd> scratch;
    std::vector<double> mag, lin(24), b(24);
    for (std::size_t f = 0; f < n; ++f) {
        const float* fr = buf_.data() + f * hop_;
        rfftMag(fft_, fr, window_, scratch, mag);
        for (int i = 0; i < 24; ++i) {
            double s = 0;
            for (int k : bands_[i]) s += static_cast<float>(mag[k]);
            lin[i] = s;
            b[i] = std::log1p(1000.0 * s);
        }
        if (!havePrev_) { prevBands_ = b; prevLin_ = lin; havePrev_ = true; }
        double fl = 0, flLow = 0;
        for (int i = 0; i < 24; ++i) {
            const double d = std::max(0.0, b[i] - prevBands_[i]);
            fl += d;
            if (i < nLowPhase_) flLow += d;
        }
        prevBands_ = b;

        double sq = 0;
        for (int i = 0; i < nfft_; ++i) sq += static_cast<double>(fr[i]) * fr[i];
        const double rms = std::sqrt(sq / nfft_);

        double pwAll = 0, pwSub = 0, logSum = 0, linSum = 0, dl = 0;
        for (int i = 0; i < 24; ++i) {
            const double pw = lin[i] * lin[i] + 1e-12;
            pwAll += pw;
            if (i < nSub_) pwSub += pw;
            logSum += std::log(lin[i] + 1e-9);
            linSum += lin[i];
            dl += std::max(0.0, lin[i] - prevLin_[i]);
        }
        prevLin_ = lin;
        const double subShare = pwSub / pwAll;
        const double flat = std::exp(logSum / 24.0) / (linSum / 24.0 + 1e-9);
        const double act = dl / (linSum + 1e-6);

        flux[f] = static_cast<float>(fl);
        fluxLow[f] = static_cast<float>(flLow);
        rmsv[f] = static_cast<float>(rms);
        feat[f] = { static_cast<float>(subShare), static_cast<float>(flat),
                    static_cast<float>(rms), static_cast<float>(act) };
    }
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(n * hop_));
    bufStart_ += static_cast<std::int64_t>(n * hop_);

    appendTrim(env_, flux);
    appendTrim(envLow_, fluxLow);
    appendTrim(rms_, rmsv);
    appendTrim(feat_, feat);
}

Estimate TempoEstimator::estimate() {
    const std::size_t minFrames = static_cast<std::size_t>(minSec_ * fps_);
    if (env_.size() < minFrames) return { std::nullopt, 0.0 };
    {   // silence: last 2 s nearly empty -> reset
        const std::size_t k = std::min(rms_.size(), static_cast<std::size_t>(2 * fps_));
        double s = 0;
        for (std::size_t i = rms_.size() - k; i < rms_.size(); ++i) s += rms_[i];
        if (k == 0 || s / k < 1e-3) { reset(); return { std::nullopt, 0.0 }; }
    }
    const std::size_t n = env_.size();
    std::vector<double> e(env_.begin(), env_.end());

    // remove slow trend (1 s moving average, np.convolve mode="same")
    const int k = static_cast<int>(fps_);
    {
        std::vector<double> pre(n + 1, 0.0);
        for (std::size_t i = 0; i < n; ++i) pre[i + 1] = pre[i] + e[i];
        const long s = (k - 1) / 2;
        std::vector<double> trend(n);
        for (std::size_t i = 0; i < n; ++i) {
            long hi = static_cast<long>(i) + s;          // inclusive
            long lo = hi - k + 1;
            lo = std::max(lo, 0L);
            hi = std::min(hi, static_cast<long>(n) - 1);
            trend[i] = (hi >= lo) ? (pre[hi + 1] - pre[lo]) / k : 0.0;
        }
        double mean = 0;
        for (std::size_t i = 0; i < n; ++i) { e[i] -= trend[i]; mean += e[i]; }
        mean /= n;
        for (auto& v : e) v -= mean;
    }

    // autocorrelation via FFT (zero-padded to >= 2n -> exact linear lags)
    int N = 1;
    while (N < static_cast<int>(2 * n)) N <<= 1;
    FFT fft(N);
    std::vector<cd> a(static_cast<std::size_t>(N), cd(0, 0));
    for (std::size_t i = 0; i < n; ++i) a[i] = cd(e[i], 0);
    fft.forward(a);
    for (auto& z : a) z = z * std::conj(z);
    fft.inverse(a);
    std::vector<double> ac(n);
    for (std::size_t i = 0; i < n; ++i) ac[i] = a[i].real();
    if (ac[0] <= 0) return { std::nullopt, 0.0 };
    const double a0 = ac[0];
    for (std::size_t i = 0; i < n; ++i) {
        ac[i] /= a0;
        ac[i] /= std::max(1.0 - static_cast<double>(i) / n, 0.25);   // unbiased-ish
    }

    const std::size_t nb = bpms_.size();
    std::vector<double> score(nb, 0.0), wsum(nb, 0.0);
    const double mults[4][2] = { {1, 1.0}, {2, 0.5}, {3, 0.33}, {4, 0.25} };
    for (std::size_t i = 0; i < nb; ++i) {
        const double lag = 60.0 * fps_ / bpms_[i];
        for (const auto& mw : mults) {
            const double L = lag * mw[0];
            if (L < 0.75 * n) {               // only lags with enough overlap
                score[i] += mw[1] * interp(L, ac);
                wsum[i] += mw[1];
            }
        }
    }
    for (std::size_t i = 0; i < nb; ++i) {
        score[i] = std::max(score[i] / std::max(wsum[i], 1e-9), 0.0);
        if (wsum[i] == 0) score[i] = 0.0;
        acc_[i] = decay_ * acc_[i] + score[i] * prior_[i];
    }
    const std::size_t i = static_cast<std::size_t>(std::max_element(acc_.begin(), acc_.end()) - acc_.begin());
    const double strength = score[i];
    conf_ = 0.5 * conf_ + 0.5 * strength;
    if (acc_[i] <= 0 || conf_ < minConf_) return { std::nullopt, conf_ };
    double bpm = bpms_[i];
    if (i > 0 && i < nb - 1) {                               // parabolic refine
        const double a_ = acc_[i - 1], b_ = acc_[i], c_ = acc_[i + 1];
        const double d = a_ - 2 * b_ + c_;
        if (d < 0) bpm += 0.5 * (a_ - c_) / d * (bpms_[1] - bpms_[0]);
    }
    return { bpm, std::min(1.0, conf_) };
}

std::optional<double> TempoEstimator::beatPhase(double periodSec, double lookbackSec) const {
    const std::size_t L = std::min(env_.size(), static_cast<std::size_t>(lookbackSec * fps_));
    const double Pf = periodSec * fps_;
    if (L < 2 * Pf || !envLastCenter_) return std::nullopt;
    // kick + snare bodies live below ~400 Hz: weight the low-band onsets more
    std::vector<double> e(L);
    double mean = 0;
    for (std::size_t j = 0; j < L; ++j) {
        const std::size_t src = env_.size() - L + j;
        e[j] = envLow_[src] * 2.0 + env_[src];
        mean += e[j];
    }
    mean /= L;
    for (auto& v : e) v -= mean;

    const int nPhi = static_cast<int>(std::ceil(Pf));
    const int nK = static_cast<int>(std::floor(L / Pf)) + 1;
    std::vector<double> score(static_cast<std::size_t>(nPhi));
    for (int phi = 0; phi < nPhi; ++phi) {
        double s = 0; int cnt = 0;
        for (int kk = 0; kk < nK; ++kk) {
            const double pos = (static_cast<double>(L) - 1) - phi - Pf * kk;
            if (pos >= 0) { s += interp(pos, e); ++cnt; }
        }
        score[phi] = s / std::max(cnt, 1);
    }
    const std::size_t i = static_cast<std::size_t>(std::max_element(score.begin(), score.end()) - score.begin());
    double phi = static_cast<double>(i);
    if (i > 0 && i + 1 < score.size()) {
        const double a = score[i - 1], b = score[i], c = score[i + 1];
        const double d = a - 2 * b + c;
        if (d < 0) phi += 0.5 * (a - c) / d;
    }
    return *envLastCenter_ - phi * hop_;
}

// ---- IntensityMeter ---------------------------------------------------------
namespace {
constexpr double W_INT[4]  = { 0.35, 0.30, 0.25, 0.10 };   // sub, flat, loud, act
constexpr double FLOORS[4] = { 0.05, 0.05, 2.0, 0.01 };    // min std for z-scores
constexpr double FAST_MARGIN = 0.10;   // short window must beat the 2 s one by this much
} // namespace

IntensityMeter::IntensityMeter(double updateSec, double memorySec, double fastWindowSec,
                               double attackTau)
    : dt_(updateSec), alpha_(updateSec / memorySec), fastWindow_(fastWindowSec),
      attackTau_(attackTau) { reset(); }

void IntensityMeter::reset() {
    init_ = false; value_.reset();
    lastSlow_.reset(); lastFast_.reset(); fastUsed_ = false;
}

std::optional<double> IntensityMeter::update(const TempoEstimator& est) {
    const std::size_t L = static_cast<std::size_t>(2.0 * est.fps());
    const auto& feat = est.features();
    if (feat.size() < L || L == 0) return value_;

    // mean features of the last `w` frames -> (sub, flat, loudness dB, act);
    // false on silence
    auto window = [&](std::size_t w, double x[4]) {
        double m[4] = {0, 0, 0, 0};
        for (std::size_t i = feat.size() - w; i < feat.size(); ++i)
            for (int c = 0; c < 4; ++c) m[c] += feat[i][c];
        for (double& v : m) v /= w;
        if (m[2] < 1e-3) return false;
        x[0] = m[0]; x[1] = m[1]; x[2] = 20 * std::log10(m[2] + 1e-9); x[3] = m[3];
        return true;
    };
    // absolute part + part relative to this track's last ~60 s
    auto targetOf = [&](const double x[4]) {
        double dot = 0;
        for (int c = 0; c < 4; ++c) {
            double z = (x[c] - mu_[c]) / std::max(std::sqrt(var_[c]), FLOORS[c]);
            z = std::min(3.0, std::max(-3.0, z));
            dot += W_INT[c] * z;
        }
        const double rel = 1.0 / (1.0 + std::exp(-1.3 * dot));
        const double absol = 0.55 * std::min(1.0, x[0] / 0.35) + 0.45 * std::min(1.0, x[1] / 0.4);
        return 0.6 * absol + 0.4 * rel;
    };

    double x[4];
    if (!window(L, x)) return value_;                        // silence
    if (!init_) {
        for (int c = 0; c < 4; ++c) { mu_[c] = x[c]; var_[c] = FLOORS[c] * FLOORS[c]; }
        init_ = true;
    }
    double target = targetOf(x);
    lastSlow_ = target;
    lastFast_.reset();
    fastUsed_ = false;
    // Fast drop detection: the 2 s average only reaches a drop's level after
    // ~2 s, so also score the last `fastWindow_` seconds and take whichever is
    // higher. Falls still follow the 2 s window (and the slow release).
    const std::size_t S = static_cast<std::size_t>(fastWindow_ * est.fps());
    double xs[4];
    if (S > 0 && S < L && window(S, xs)) {
        const double fast = targetOf(xs);
        lastFast_ = fast;
        if (fast > target + FAST_MARGIN) { target = fast; fastUsed_ = true; }   // a real jump, not jitter
    }

    for (int c = 0; c < 4; ++c) {                            // track statistics: 2 s window
        mu_[c] += alpha_ * (x[c] - mu_[c]);
        var_[c] += alpha_ * ((x[c] - mu_[c]) * (x[c] - mu_[c]) - var_[c]);
    }
    if (!value_) {
        value_ = target;
    } else {
        const double tau = target > *value_ ? attackTau_ : 1.5;   // fast up, slow down
        *value_ += (target - *value_) * (1 - std::exp(-dt_ / tau));
    }
    return value_;
}

// ---- snare ------------------------------------------------------------------
SnareLevels::SnareLevels(double sr) : sr_(sr), fft_(SNARE_NFFT), win_(hanning(SNARE_NFFT)) {
    const auto f = rfftfreq(SNARE_NFFT, sr);
    crack_ = bandBins(f, 1000.0, 4000.0);
    tail_  = bandBins(f, 5000.0, 10000.0);
    kick_  = bandBins(f, 30.0, 90.0);
}

std::array<float, 3> SnareLevels::frame(const float* x) const {
    std::vector<cd> scratch;
    std::vector<double> mag;
    rfftMag(fft_, x, win_, scratch, mag);
    auto lvl = [&](const std::vector<int>& idx) {
        double s = 0;
        for (int k : idx) s += mag[k];
        return static_cast<float>(std::sqrt(s));             // mildly compressed
    };
    return { lvl(crack_), lvl(tail_), lvl(kick_) };
}

std::vector<double> snareEnvelopeFromLevels(const std::vector<std::array<float, 3>>& lv) {
    const std::size_t n = lv.size();
    if (n < 4) return {};
    const std::size_t m = n - 1;
    auto onset = [&](int band) {
        std::vector<double> fl(m);
        for (std::size_t i = 0; i < m; ++i)
            fl[i] = std::max(0.0, static_cast<double>(lv[i + 1][band]) - lv[i][band]);
        const double scale = percentile(fl, 99.5) + 1e-9;    // robust scale
        for (auto& v : fl) v = std::min(v / scale, 1.5);
        return fl;
    };
    const std::vector<double> crack = onset(0), tail = onset(1), kick = onset(2);
    std::vector<double> env(n, 0.0);
    for (std::size_t i = 0; i < m; ++i) {
        // kick sweeps down: sub arrives late -> max over a window (np.roll)
        double km = 0;
        for (int d = -2; d <= 6; ++d) {
            const long j = static_cast<long>(pyMod(static_cast<double>(static_cast<long>(i) - d), static_cast<double>(m)));
            km = std::max(km, kick[static_cast<std::size_t>(j)]);
        }
        env[i + 1] = std::sqrt(crack[i] * tail[i]) / (1.0 + 3.0 * km);
    }
    return env;
}

std::vector<double> snareEnvelopeV2(const std::vector<float>& audio, double sr) {
    if (audio.size() < static_cast<std::size_t>(SNARE_NFFT)) return {};
    const std::size_t n = (audio.size() - SNARE_NFFT) / SNARE_HOP + 1;
    if (n < 4) return {};
    SnareLevels sl(sr);
    std::vector<std::array<float, 3>> lv(n);
    for (std::size_t i = 0; i < n; ++i) lv[i] = sl.frame(audio.data() + i * SNARE_HOP);
    return snareEnvelopeFromLevels(lv);
}

std::optional<double> snareCycleRatio(const std::vector<double>& env, double efps,
                                      double period, double lastBeat, double minSec) {
    if (env.size() < efps * minSec || env.empty()) return std::nullopt;
    const double half = period / 2.0;
    const double dur = env.size() / efps;
    const long jMin = static_cast<long>(std::ceil((0.1 - lastBeat) / half));
    const long jMax = static_cast<long>(std::floor((dur - 0.1 - lastBeat) / half));
    std::vector<double> slots[4];
    const long w = static_cast<long>(0.03 * efps);
    const long len = static_cast<long>(env.size());
    for (long j = jMin; j <= jMax; ++j) {
        const long c = static_cast<long>(pyRound((lastBeat + j * half) * efps));
        const long lo = std::max(0L, c - w), hi = std::min(len, c + w + 1);
        if (hi > lo) {
            double mx = env[static_cast<std::size_t>(lo)];
            for (long q = lo; q < hi; ++q) mx = std::max(mx, env[static_cast<std::size_t>(q)]);
            slots[((j % 4) + 4) % 4].push_back(mx);
        }
    }
    for (auto& s : slots) if (s.size() < 2) return std::nullopt;
    double c[4];
    for (int i = 0; i < 4; ++i) c[i] = median(slots[i]);
    const double cmin = std::min(std::min(c[0], c[1]), std::min(c[2], c[3]));
    for (double& v : c) v -= cmin;
    const double cmax = std::max(std::max(c[0], c[1]), std::max(c[2], c[3]));
    if (cmax <= 0.15 * (median(env) + 1e-9) + 1e-9) return std::nullopt;  // flat
    const double x1 = std::abs(cd(c[0] - c[2], c[3] - c[1]));   // 2-beat periodicity
    const double x2 = std::fabs(c[0] - c[1] + c[2] - c[3]);     // 1-beat periodicity
    if (x1 + x2 <= 1e-9) return std::nullopt;
    return x1 / (x1 + x2);
}

// ---- Stabilizer -------------------------------------------------------------
Stabilizer::Stabilizer(double tol, int confirm, int sw, double alpha, int hold)
    : tol_(tol), alpha_(alpha), confirm_(confirm), switch_(sw), hold_(hold) {}

void Stabilizer::reset() { value_.reset(); shown_.reset(); pending_.clear(); misses_ = 0; }

bool Stabilizer::close(double a, double b) const { return std::fabs(a - b) <= tol_ * std::max(a, b); }

std::optional<int> Stabilizer::update(std::optional<double> bpm) {
    if (!bpm) {
        ++misses_;
        pending_.clear();
        if (misses_ >= hold_) { value_.reset(); shown_.reset(); }
        return shown_;
    }
    misses_ = 0;
    const double b = *bpm;
    if (value_ && close(b, *value_)) {
        *value_ += alpha_ * (b - *value_);
        pending_.clear();
    } else {
        if (!pending_.empty() && !close(b, pending_.back())) pending_.clear();
        pending_.push_back(b);
        const int need = value_ ? switch_ : confirm_;
        if (static_cast<int>(pending_.size()) >= need) {
            value_ = median(pending_);
            shown_.reset();
            pending_.clear();
        }
    }
    if (value_) {
        if (!shown_ || std::fabs(*value_ - *shown_) > 0.65)
            shown_ = static_cast<int>(pyRound(*value_));
    }
    return shown_;
}

// ---- OctaveVote ---------------------------------------------------------------
OctaveVote::OctaveVote(double enter, double exit, bool strict)
    : enter_(enter), exit_(exit), strict_(strict) { reset(); }

void OctaveVote::reset() {
    score_ = strict_ ? std::max(0.5, exit_ + 0.1) : 0.5;
    doubled_ = false;
    n_ = 0;
}

int OctaveVote::factorFor(double rawBpm, std::optional<double> ratio) {
    if (rawBpm * 2 > DOUBLE_MAX_BPM) return 1;
    if (ratio) {
        ++n_;
        score_ += 0.35 * (*ratio - score_);
    }
    if (!doubled_ && score_ < enter_) doubled_ = true;
    else if (doubled_ && score_ > exit_) doubled_ = false;
    return doubled_ ? 2 : 1;
}

bool OctaveVote::settled(double rawBpm) const {
    if (rawBpm * 2 > DOUBLE_MAX_BPM) return true;
    if (!strict_) return n_ >= 2;
    if (doubled_) return true;
    return (n_ >= 3 && score_ > exit_) || n_ >= 6;
}

// ---- BeatClock ------------------------------------------------------------------
void BeatClock::update(double tBeat, double period) {
    if (!have_ || std::fabs(period - period_) > 0.04 * period_) {
        t0_ = tBeat; period_ = period; misses_ = 0; have_ = true;
        return;
    }
    const double n = pyRound((tBeat - t0_) / period_);
    const double err = tBeat - (t0_ + n * period_);            // phase error
    if (std::fabs(err) < 0.2 * period_) {
        misses_ = 0;
        t0_ += n * period_ + 0.4 * err;
        period_ += 0.3 * (period - period_);
    } else {
        ++misses_;
        if (misses_ >= 2) { t0_ = tBeat; period_ = period; misses_ = 0; }   // re-sync
    }
}

std::optional<Grid> BeatClock::grid() const {
    if (!have_) return std::nullopt;
    return Grid{ t0_, period_ };
}

// ---- Tracker ------------------------------------------------------------------
Tracker::Tracker(int confirm, double octEnter, double octExit, bool strictOctave)
    : stab_(0.025, confirm), octave_(octEnter, octExit, strictOctave) {}

void Tracker::reset() {
    stab_.reset(); octave_.reset(); clock_.reset();
    shown.reset(); grid.reset();
}

void Tracker::update(const std::optional<Analysis>& r) {
    if (!r) {
        shown = stab_.update(std::nullopt);
    } else {
        factor_ = octave_.factorFor(r->bpm, r->ratio);
        if (!octave_.settled(r->bpm)) return;               // wait for the octave decision
        shown = stab_.update(r->bpm * factor_);
        if (r->beatTime) clock_.update(*r->beatTime, r->period / factor_);
    }
    if (!shown) {
        clock_.reset();
        grid.reset();
    } else {
        grid = clock_.grid();
    }
}

} // namespace bpm
