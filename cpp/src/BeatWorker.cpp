#include "BeatWorker.h"

#include <algorithm>
#include <cmath>

namespace bpm {

// octave rule thresholds for the snare-specific detector (bpm_detector.py)
namespace {
constexpr double OCT_ENTER_V2 = 0.42;
constexpr double OCT_EXIT_V2  = 0.50;
// octave rule thresholds with the beat lock, tuned on real tracks: the
// original 0.42 / 0.50 doubled ordinary ~95 BPM backbeats (ratio ~0.38-0.44)
constexpr double LOCK_OCT_ENTER = 0.34;
constexpr double LOCK_OCT_EXIT  = 0.40;
constexpr double RATIO_WINDOW_SEC = 8.0;
constexpr std::size_t MAX_OFFSETS = 300;
constexpr double TERNARY_MARGIN = 0.2;   // 3:2 fix: ternary must beat binary by this
} // namespace

// ---- AudioRing ----------------------------------------------------------------
void AudioRing::restart(int sampleRate) {
    {
        std::lock_guard<std::mutex> lk(m_);
        buf_.clear();
        offsets_.clear();
        sr_ = sampleRate;
    }
    generation_.fetch_add(1);
}

void AudioRing::push(const float* mono, std::size_t n, double now) {
    std::lock_guard<std::mutex> lk(m_);
    buf_.insert(buf_.end(), mono, mono + n);
    const std::size_t keep = static_cast<std::size_t>(keepSec_ * sr_);
    // trim with ~1 s of slack so we don't memmove on every 10 ms block
    if (buf_.size() > keep + static_cast<std::size_t>(sr_))
        buf_.erase(buf_.begin(), buf_.end() - static_cast<std::ptrdiff_t>(keep));
    total_ += static_cast<std::int64_t>(n);
    // blocks can only arrive late, never early -> min over recent blocks is
    // the best estimate of the true capture clock
    offsets_.push_back(now - static_cast<double>(total_) / sr_);
    if (offsets_.size() > MAX_OFFSETS) offsets_.pop_front();
}

int AudioRing::sampleRate() const {
    std::lock_guard<std::mutex> lk(m_);
    return sr_;
}

std::int64_t AudioRing::snapshot(double seconds, std::vector<float>& out) const {
    std::lock_guard<std::mutex> lk(m_);
    const std::size_t keep = static_cast<std::size_t>(keepSec_ * sr_);
    std::size_t n = std::min(static_cast<std::size_t>(seconds * sr_), std::min(keep, buf_.size()));
    out.assign(buf_.end() - static_cast<std::ptrdiff_t>(n), buf_.end());
    return total_;
}

std::int64_t AudioRing::readNew(std::int64_t lastTotal, std::vector<float>& out) const {
    std::lock_guard<std::mutex> lk(m_);
    const std::int64_t want = total_ - lastTotal;
    const std::size_t n = want > 0 ? std::min(static_cast<std::size_t>(want), buf_.size()) : 0;
    out.assign(buf_.end() - static_cast<std::ptrdiff_t>(n), buf_.end());
    return total_;
}

std::optional<double> AudioRing::sampleTime(double sampleIndex) const {
    std::lock_guard<std::mutex> lk(m_);
    if (offsets_.empty()) return std::nullopt;
    const double off = *std::min_element(offsets_.begin(), offsets_.end());
    return off + sampleIndex / sr_;
}

// ---- BeatWorker -----------------------------------------------------------------
BeatWorker::BeatWorker(AudioRing& ring, const Options& opt)
    : ring_(ring), opt_(opt), tracker_(3, OCT_ENTER_V2, OCT_EXIT_V2, true),
      lock_(LOCK_OCT_ENTER, LOCK_OCT_EXIT, true),
      meter_(UPDATE_SEC, 60.0, opt.fastDrop ? 0.5 : 0.0, opt.fastDrop ? 0.15 : 0.4, opt.vocalRobust) {}

BeatState BeatWorker::state() const {
    std::lock_guard<std::mutex> lk(outM_);
    return out_;
}

void BeatWorker::step() {
    if (ring_.generation() != gen_ || !est_) {           // (re)start
        gen_ = ring_.generation();
        sr_ = ring_.sampleRate();
        std::vector<float> none;
        lastTotal_ = ring_.snapshot(0, none);
        est_ = std::make_unique<TempoEstimator>(sr_, lastTotal_);
        tracker_.reset();
        lock_.reset();
        gate_.reset();
        meter_.reset();
        snare_ = std::make_unique<SnareLevels>(sr_);
        snareCache_.clear();
        snareFirstK_ = 0;
        std::lock_guard<std::mutex> lk(outM_);
        out_.bpm.reset();
        out_.grid.reset();
        return;
    }
    std::vector<float> audio;
    lastTotal_ = ring_.readNew(lastTotal_, audio);
    est_->feed(audio.data(), audio.size());
    const std::optional<Analysis> r = analyze();          // (runs the tempo estimate)
    const bool music = opt_.musicGate ? gate_.update(*est_) : true;
    std::optional<int> shownBpm;
    std::optional<Grid> grid;
    if (opt_.beatLock) {
        // "now" in the capture clock, the same timebase as the beat times
        const double now = ring_.sampleTime(static_cast<double>(lastTotal_)).value_or(nowSec());
        lock_.update(r, music, now);
        shownBpm = lock_.shown;
        grid = lock_.grid;
    } else {
        tracker_.update(music ? r : std::nullopt);
        shownBpm = tracker_.shown;
        grid = tracker_.grid;
    }
    const auto intensity = meter_.update(*est_);

    std::lock_guard<std::mutex> lk(outM_);
    out_.bpm = shownBpm;
    out_.grid = grid;
    out_.music = music;
    out_.musicScore = gate_.score();
    out_.rawBpm = r ? std::optional<double>(r->bpm) : std::nullopt;
    out_.intensity = intensity;
    out_.intensitySlow = meter_.lastSlow();
    out_.intensityFast = meter_.lastFast();
    out_.fastUsed = meter_.fastUsed();
    out_.updatedAt = nowSec();
}

std::optional<Analysis> BeatWorker::analyze() {
    const Estimate e = est_->estimate();
    if (!e.bpm) return std::nullopt;
    Analysis r;
    r.bpm = *e.bpm;
    // 3:2 fix: at a wrong metrical level (e.g. drum & bass read at 116
    // instead of 174) the hats fall on 1/3 and 2/3 of the "beat" instead of
    // halfway between beats -- electronic music subdivides in two.
    {
        const auto sub = est_->subdivision(60.0 / r.bpm);
        if (sub.second - sub.first > TERNARY_MARGIN && r.bpm * 1.5 <= 200.0) r.bpm *= 1.5;
    }
    r.period = 60.0 / r.bpm;
    const auto beatAbs = est_->beatPhase(r.period);
    if (!beatAbs) return r;
    r.beatTime = ring_.sampleTime(*beatAbs);
    if (r.bpm * 2 <= DOUBLE_MAX_BPM) {
        std::int64_t start = 0;
        const std::vector<double> env = snareEnvelope(RATIO_WINDOW_SEC, start);
        if (!env.empty())
            r.ratio = snareCycleRatio(env, static_cast<double>(sr_) / SNARE_HOP, r.period,
                                      (*beatAbs - static_cast<double>(start)) / sr_, 3.0);
    }
    return r;
}

std::vector<double> BeatWorker::snareEnvelope(double sec, std::int64_t& startSample) {
    std::vector<float> audio;
    const std::int64_t total = ring_.snapshot(sec, audio);
    const std::int64_t start = total - static_cast<std::int64_t>(audio.size());
    // frames on an absolute grid: frame k covers samples [k*HOP, k*HOP+NFFT)
    const std::int64_t k0 = (start + SNARE_HOP - 1) / SNARE_HOP;
    const std::int64_t kEnd = (total - SNARE_NFFT) >= 0 ? (total - SNARE_NFFT) / SNARE_HOP + 1 : 0;
    startSample = k0 * SNARE_HOP;
    if (kEnd - k0 < 4) return {};

    // drop cached frames that fell out of the window / don't connect
    if (snareCache_.empty() || snareFirstK_ > k0 ||
        snareFirstK_ + static_cast<std::int64_t>(snareCache_.size()) < k0) {
        snareCache_.clear();
        snareFirstK_ = k0;
    }
    while (snareFirstK_ < k0 && !snareCache_.empty()) { snareCache_.pop_front(); ++snareFirstK_; }
    if (snareCache_.empty()) snareFirstK_ = k0;
    for (std::int64_t k = snareFirstK_ + static_cast<std::int64_t>(snareCache_.size()); k < kEnd; ++k)
        snareCache_.push_back(snare_->frame(audio.data() + (k * SNARE_HOP - start)));

    std::vector<std::array<float, 3>> lv(snareCache_.begin(),
                                         snareCache_.begin() + static_cast<std::ptrdiff_t>(kEnd - k0));
    return snareEnvelopeFromLevels(lv);
}

} // namespace bpm
