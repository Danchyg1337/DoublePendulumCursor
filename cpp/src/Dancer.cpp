// Dancer.cpp -- see Dancer.h. Port of BPMidentifier/gif_dancer.py.
#include "Dancer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>

namespace dancer {

namespace {

// beat following: small timing drift is followed smoothly; a sudden jump (new
// beat "heard" between beats, tempo glitch) is ignored unless it holds
constexpr double PHASE_SMALL = 0.12;        // beat fraction treated as normal drift
constexpr double PHASE_RATE = 5.0;          // 1/s, how fast drift is absorbed
constexpr double MAX_SLEW = 1.0;            // beats/s, max speed of a phase catch-up
constexpr double JUMP_CONFIRM_SEC = 0.6;    // a phase jump must hold this long
constexpr double TEMPO_SMALL = 0.04;        // relative tempo change treated as drift
constexpr double TEMPO_CONFIRM_SEC = 1.5;   // a bigger tempo change must hold this long
constexpr double MIN_LOOP_PLAY_SEC = 3.0;   // repeatable GIF plays at least this long
constexpr double PRE_STRETCH = 1.35;        // max intro speed change on a fresh start
// one marked GIF beat always = one music beat; (gif beats per music beat, penalty)
constexpr double RATIOS[][2] = { {1.0, 0.0} };

// ---- minimal JSON (enough for .gifbpm metadata) -------------------------------
struct Json {
    enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;

    const Json* get(const std::string& k) const {
        auto it = obj.find(k);
        return it == obj.end() ? nullptr : &it->second;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}
    Json parse() {
        Json v = value();
        ws();
        return v;
    }
private:
    [[noreturn]] void bad() { throw std::runtime_error("bad metadata JSON"); }
    void ws() { while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_; }
    char peek() { ws(); if (i_ >= s_.size()) bad(); return s_[i_]; }
    void expect(char c) { if (peek() != c) bad(); ++i_; }
    bool lit(const char* w) {
        const std::size_t n = std::strlen(w);
        if (s_.compare(i_, n, w) == 0) { i_ += n; return true; }
        return false;
    }
    Json value() {
        Json v;
        const char c = peek();
        if (c == '{') {
            v.t = Json::T::Obj; ++i_;
            if (peek() == '}') { ++i_; return v; }
            for (;;) {
                std::string k = string();
                expect(':');
                v.obj[k] = value();
                if (peek() == ',') { ++i_; continue; }
                expect('}');
                return v;
            }
        }
        if (c == '[') {
            v.t = Json::T::Arr; ++i_;
            if (peek() == ']') { ++i_; return v; }
            for (;;) {
                v.arr.push_back(value());
                if (peek() == ',') { ++i_; continue; }
                expect(']');
                return v;
            }
        }
        if (c == '"') { v.t = Json::T::Str; v.str = string(); return v; }
        if (lit("true"))  { v.t = Json::T::Bool; v.b = true; return v; }
        if (lit("false")) { v.t = Json::T::Bool; return v; }
        if (lit("null"))  { return v; }
        const char* begin = s_.c_str() + i_;
        char* end = nullptr;
        v.num = std::strtod(begin, &end);
        if (end == begin) bad();
        i_ += static_cast<std::size_t>(end - begin);
        v.t = Json::T::Num;
        return v;
    }
    std::string string() {
        expect('"');
        std::string out;
        while (i_ < s_.size() && s_[i_] != '"') {
            char c = s_[i_++];
            if (c == '\\' && i_ < s_.size()) {
                const char e = s_[i_++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (i_ + 4 > s_.size()) bad();
                        const unsigned cp = static_cast<unsigned>(std::stoul(s_.substr(i_, 4), nullptr, 16));
                        i_ += 4;
                        if (cp < 0x80) out += static_cast<char>(cp);
                        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                        else { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                        break;
                    }
                    default: out += e;
                }
            } else {
                out += c;
            }
        }
        if (i_ >= s_.size()) bad();
        ++i_;
        return out;
    }
    const std::string& s_;
    std::size_t i_ = 0;
};

std::uint32_t le32(const std::vector<std::uint8_t>& d, std::size_t at) {
    if (at + 4 > d.size()) throw std::runtime_error("truncated .gifbpm");
    return d[at] | (d[at + 1] << 8) | (d[at + 2] << 16) | (static_cast<std::uint32_t>(d[at + 3]) << 24);
}

double naturalPhi(const Actor* actor, double ratio, double gifMs) {
    const double segMs = 60000.0 / actor->bpm;                 // avg beat gap
    return gifMs / segMs / ratio;
}

// -> (cost, actor, ratio) with the smallest tempo change
bool bestMatch(const std::vector<const Actor*>& actors, double bpm,
               const Actor*& best, double& bestRatio) {
    double bestCost = std::numeric_limits<double>::infinity();
    best = nullptr;
    for (const Actor* a : actors) {
        if (!a->usable || a->bpm <= 0) continue;
        for (const auto& rp : RATIOS) {
            const double stretch = bpm * rp[0] / a->bpm;        // >1: GIF plays faster
            const double cost = std::fabs(std::log2(stretch)) + rp[1];
            if (!best || cost < bestCost) { bestCost = cost; best = a; bestRatio = rp[0]; }
        }
    }
    return best != nullptr;
}

} // namespace

// ---- Actor ---------------------------------------------------------------------
Actor Actor::load(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open");
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parse(data, std::filesystem::u8path(path).stem().u8string());
}

Actor Actor::parse(const std::vector<std::uint8_t>& data, const std::string& fallbackName) {
    // Format: "GBPM" | u8 version | u8 flags | u16 reserved | u32 metaLen |
    //         meta JSON | u32 gifLen | GIF bytes
    if (data.size() < 12 || std::memcmp(data.data(), "GBPM", 4) != 0)
        throw std::runtime_error("not a .gifbpm file");
    const int version = data[4];
    const int flags = data[5];
    if (version != 1) throw std::runtime_error("unsupported .gifbpm version " + std::to_string(version));
    const std::uint32_t metaLen = le32(data, 8);
    if (12 + static_cast<std::size_t>(metaLen) > data.size()) throw std::runtime_error("truncated .gifbpm");
    const std::string metaText(reinterpret_cast<const char*>(data.data()) + 12, metaLen);
    const Json meta = JsonParser(metaText).parse();
    const std::uint32_t gifLen = le32(data, 12 + metaLen);
    const std::size_t gifAt = 16 + static_cast<std::size_t>(metaLen);
    const std::size_t gifEnd = std::min(data.size(), gifAt + gifLen);

    Actor a;
    a.gifBytes.assign(data.begin() + static_cast<std::ptrdiff_t>(std::min(gifAt, data.size())),
                      data.begin() + static_cast<std::ptrdiff_t>(gifEnd));
    const Json* nm = meta.get("name");
    a.name = (nm && nm->t == Json::T::Str && !nm->str.empty()) ? nm->str : fallbackName;
    a.loopable = (flags & 1) != 0;
    if (const Json* d = meta.get("frameDelaysMs"))
        for (const Json& v : d->arr) a.delays.push_back(std::max(1, static_cast<int>(v.num)));
    a.starts.push_back(0);
    for (std::size_t i = 0; i + 1 < a.delays.size(); ++i) a.starts.push_back(a.starts.back() + a.delays[i]);
    if (a.delays.empty()) a.starts.clear();
    for (int d : a.delays) a.duration += d;

    std::set<int> beatFrames;
    if (const Json* bf = meta.get("beatFrames"))
        for (const Json& v : bf->arr) beatFrames.insert(static_cast<int>(v.num));
    for (int b : beatFrames)
        if (b >= 0 && b < static_cast<int>(a.starts.size())) a.beatTimes.push_back(a.starts[b]);

    const auto& bt = a.beatTimes;
    for (std::size_t i = 0; i + 1 < bt.size(); ++i) a.segments.push_back({bt[i], bt[i + 1] - bt[i]});
    if (a.loopable && !bt.empty()) a.segments.push_back({bt.back(), a.duration + bt.front() - bt.back()});
    a.segments.erase(std::remove_if(a.segments.begin(), a.segments.end(),
                                    [](const Seg& s) { return !(s.len > 0); }), a.segments.end());
    a.usable = !a.segments.empty();
    double avg = 0;
    if (a.usable) {
        for (const Seg& s : a.segments) avg += s.len;
        avg /= a.segments.size();
    }
    if (avg) {
        a.bpm = 60000.0 / avg;
    } else {
        const Json* b = meta.get("bpm");
        a.bpm = (b && b->t == Json::T::Num) ? b->num : 0.0;
    }
    return a;
}

void Actor::decode(int maxSide, const std::atomic<bool>* cancel) {
    frames.clear();
    std::string err;
    const int limit = static_cast<int>(delays.size());
    gif::decode(gifBytes.data(), gifBytes.size(), [&](const gif::Image& im, int) {
        if (cancel && cancel->load()) return false;
        const double s = std::min(1.0, static_cast<double>(maxSide) / std::max(im.w, im.h));
        const int w = std::max(1, static_cast<int>(bpm::pyRound(im.w * s)));
        const int h = std::max(1, static_cast<int>(bpm::pyRound(im.h * s)));
        frames.push_back(gif::resizePremultiplied(im, w, h));
        return true;
    }, limit, &err);
    // metadata may list more delays than decodable frames
    const std::size_t n = frames.size();
    if (delays.size() > n) delays.resize(n);
    if (starts.size() > n) starts.resize(n);
    gifBytes.clear();
    gifBytes.shrink_to_fit();
}

int Actor::frameAtTime(double tMs) const {
    if (starts.empty() || duration <= 0) return 0;
    const double t = bpm::pyMod(tMs, duration);
    const int i = static_cast<int>(std::upper_bound(starts.begin(), starts.end(), t) - starts.begin()) - 1;
    return std::max(0, std::min(static_cast<int>(starts.size()) - 1, i));
}

std::vector<std::unique_ptr<Actor>> loadActors(const std::string& folder) {
    namespace fs = std::filesystem;
    std::vector<std::unique_ptr<Actor>> out;
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::u8path(folder), ec)) {
        std::string ext = e.path().extension().u8string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (e.is_regular_file(ec) && ext == ".gifbpm") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& p : files) {
        try {
            auto a = std::make_unique<Actor>(Actor::load(p.u8string()));
            if (a->usable) out.push_back(std::move(a));   // else: no beat frames, skipped
        } catch (const std::exception&) {
            // unreadable file: skipped
        }
    }
    return out;
}

// ---- Run -----------------------------------------------------------------------
Run::Run(const Actor* a, double r, double pb, double pre)
    : actor(a), ratio(r), phiB(pb), prePhi(pre), phiStart(pb - pre) {
    if (!a->loopable) endCycle = 0;
}

double Run::lastBeatPhi(int cycle) const {
    const double k = static_cast<double>(actor->beatTimes.size());
    const double nSeg = static_cast<double>(actor->segments.size());
    return phiB + (cycle * nSeg + (k - 1)) * segPhi();
}

double Run::gifTime(double phi) const {
    const Actor& a = *actor;
    const double b0 = a.beatTimes.front(), bl = a.beatTimes.back();
    if (phi < phiB) {                                            // pre-roll
        if (prePhi <= 1e-9) return b0;
        const double u = (phi - phiStart) / prePhi;
        return std::max(0.0, u) * b0;
    }
    if (endCycle && phi >= lastBeatPhi(*endCycle)) {             // post-roll
        const double post = a.duration - bl;
        if (!postPhi || *postPhi == 0.0 || post <= 0) return bl;
        const double u = std::min(1.0, (phi - lastBeatPhi(*endCycle)) / *postPhi);
        return std::min(bl + u * post, a.duration - 0.5);
    }
    const double j = (phi - phiB) * ratio;                       // beat gaps
    const double k = std::floor(j);
    const auto n = static_cast<long long>(a.segments.size());
    const auto idx = ((static_cast<long long>(k) % n) + n) % n;
    const Actor::Seg& s = a.segments[static_cast<std::size_t>(idx)];
    return bpm::pyMod(s.start + (j - k) * s.len, a.duration);
}

// ---- DancerEngine --------------------------------------------------------------
DancerEngine::DancerEngine(std::vector<const Actor*> actors) : actors_(std::move(actors)) { reset(); }

void DancerEngine::reset() {
    phi_.reset();
    lastT_ = 0;
    period_ = 0;
    cur_.reset();
    next_.reset();
    phaseJump_.reset();
    following_ = false;
    tempoJump_.reset();
}

void DancerEngine::advance(double now, const bpm::Grid& grid, double offset) {
    const double t = now - offset;
    const double tBeat = grid.t0, gperiod = grid.period;
    if (!phi_) {
        period_ = gperiod;
        phi_ = (t - tBeat) / gperiod;
        lastT_ = now;
        return;
    }
    const double dt = std::max(0.0, now - lastT_);
    lastT_ = now;

    // tempo
    const double rel = gperiod / period_ - 1.0;
    if (std::fabs(rel) < TEMPO_SMALL) {
        period_ += (gperiod - period_) * std::min(1.0, dt * 1.5);
        tempoJump_.reset();
    } else if (!tempoJump_ || std::fabs(gperiod / tempoJump_->first - 1.0) > TEMPO_SMALL) {
        tempoJump_ = std::make_pair(gperiod, now);               // new candidate
    } else if (now - tempoJump_->second >= TEMPO_CONFIRM_SEC) {
        period_ = gperiod;                                       // it held: accept
        tempoJump_.reset();
    }

    double phi = *phi_ + dt / period_;

    // phase (in this counter's beats)
    const double target = (t - tBeat) / gperiod;
    double err = target - phi;
    err -= bpm::pyRound(err);                                    // only the phase matters
    double corr = 0.0;
    if (tempoJump_) {
        corr = 0.0;            // detector tempo is in doubt: don't trust its phase either
    } else if (std::fabs(err) < PHASE_SMALL || following_) {
        corr = err * std::min(1.0, dt * PHASE_RATE);
        phaseJump_.reset();
        if (std::fabs(err) < PHASE_SMALL / 2) following_ = false;
    } else {
        corr = 0.0;
        if (!phaseJump_ ||
            std::fabs(bpm::pyMod((err - phaseJump_->first) + 0.5, 1.0) - 0.5) > PHASE_SMALL) {
            phaseJump_ = std::make_pair(err, now);               // new candidate
        } else if (now - phaseJump_->second >= JUMP_CONFIRM_SEC) {
            following_ = true;                                   // it held: catch up
            corr = err * std::min(1.0, dt * PHASE_RATE);
        }
    }
    const double lim = MAX_SLEW * dt;
    phi += std::max(-lim, std::min(lim, corr));
    phi_ = std::max(*phi_, phi);                                 // never run backwards
}

std::pair<const Actor*, double> DancerEngine::pick(double bpm, const Actor* exclude) {
    std::vector<const Actor*> pool;
    for (const Actor* a : actors_)
        if (!used_.count(a) && a != exclude) pool.push_back(a);
    if (pool.empty()) {
        used_.clear();
        for (const Actor* a : actors_) if (a != exclude) pool.push_back(a);
        if (pool.empty()) pool = actors_;
    }
    const Actor* actor = nullptr;
    double ratio = 1.0;
    if (!bestMatch(pool, bpm, actor, ratio)) { actor = pool.front(); ratio = 1.0; }
    used_.insert(actor);
    return { actor, ratio };
}

void DancerEngine::startFresh(double bpm) {
    // Start moving right away -- never hold a frozen frame 0 while waiting for
    // the "right" beat (that showed as a stutter when the GIF first appeared).
    //  a) Play the intro from frame 0 starting now, slightly sped up / slowed
    //     down (within PRE_STRETCH) so its first beat frame lands on a beat.
    //  b) If no beat is reachable that way, join the GIF "already in progress"
    //     as if it had started on time, skipping < 1 beat of its intro.
    // Either way the beat frames stay on the music's beats.
    const auto [actor, ratio] = pick(bpm);
    const double phi = *phi_;
    const double pre = naturalPhi(actor, ratio, actor->beatTimes.front());
    double bestB = 0, bestCost = std::numeric_limits<double>::infinity();
    for (const double b : { std::floor(phi + pre), std::ceil(phi + pre) }) {
        const double span = b - phi;
        if (pre <= 1e-9 || span < 0.15) continue;           // too short to play an intro
        const double cost = std::fabs(std::log(span / pre));
        if (cost <= std::log(PRE_STRETCH) && cost < bestCost) { bestCost = cost; bestB = b; }
    }
    if (bestCost < std::numeric_limits<double>::infinity())
        cur_.emplace(actor, ratio, bestB, bestB - phi);     // a) intro starts now
    else
        cur_.emplace(actor, ratio, std::floor(phi + pre), pre);  // b) join in progress
    next_.reset();
}

void DancerEngine::planNext(const Actor* actor, double ratio, int cycle) {
    Run& cur = *cur_;
    const Actor* a = cur.actor;
    const double post = naturalPhi(a, cur.ratio, a->duration - a->beatTimes.back());
    const double pre = naturalPhi(actor, ratio, actor->beatTimes.front());
    const double gap = post + pre;
    const double n = std::max(1.0, bpm::pyRound(gap));  // whole beats to fill
    const double c = gap > 1e-9 ? n / gap : 1.0;
    cur.endCycle = cycle;
    cur.postPhi = post * c;
    const double phiB = cur.lastBeatPhi(cycle) + n;
    next_.emplace(actor, ratio, phiB, pre * c);
}

DancerEngine::Out DancerEngine::step(double now, std::optional<int> bpm,
                                     std::optional<bpm::Grid> grid, double offset) {
    if (!grid || !bpm || *bpm == 0 || actors_.empty()) {
        // no beat identified: show nothing; start over when it's back
        if (cur_) reset();
        return {};
    }
    advance(now, *grid, offset);
    const double phi = *phi_;
    const double b = static_cast<double>(*bpm);

    if (!cur_) startFresh(b);
    if (next_ && phi >= next_->phiStart) { cur_ = next_; next_.reset(); }
    if (!cur_->startedAt && phi >= cur_->phiStart) cur_->startedAt = now;

    // decide what follows during the last beat gap before the cycle's last beat
    // frame (deciding later would change frames already shown). Non-repeatable:
    // always change when it ends. Repeatable: change at the end of the loop
    // that ends after MIN_LOOP_PLAY_SEC of playing -- never cut a loop short.
    if (!next_ && phi >= cur_->phiB) {
        Run& cur = *cur_;
        if (!cur.actor->loopable) {
            if (phi >= cur.lastBeatPhi(0) - cur.segPhi()) {
                const auto [a, r] = pick(b, cur.actor);
                planNext(a, r, 0);
            }
        } else if (actors_.size() > 1 && cur.startedAt) {
            const int cycle = std::max(0, static_cast<int>(std::floor(
                (phi - cur.phiB) * cur.ratio / static_cast<double>(cur.actor->segments.size()))));
            const double lb = cur.lastBeatPhi(cycle);
            if (lb - cur.segPhi() <= phi && phi < lb) {
                const double loopEndAt = now + (lb - phi) * period_;
                if (loopEndAt - *cur.startedAt >= MIN_LOOP_PLAY_SEC) {
                    const auto [a, r] = pick(b, cur.actor);
                    planNext(a, r, cycle);
                }
            }
        }
    }
    if (next_ && phi >= next_->phiStart) { cur_ = next_; next_.reset(); }
    Out out;
    out.actor = cur_->actor;
    out.frame = phi < cur_->phiStart ? 0 : cur_->frame(phi);   // waiting for the right moment
    return out;
}

// ---- Dancer --------------------------------------------------------------------
Dancer::Dancer(std::string actorsDir, int maxSide, double showIntensity, double hideIntensity)
    : show_(showIntensity), hide_(hideIntensity) {
    loader_ = std::thread([this, dir = std::move(actorsDir), maxSide] {
        auto actors = loadActors(dir);
        std::vector<const Actor*> ok;
        for (auto& a : actors) {
            if (cancel_) return;
            try { a->decode(maxSide, &cancel_); } catch (...) { a->frames.clear(); }
            if (!a->frames.empty()) ok.push_back(a.get());
        }
        if (cancel_) return;
        actors_ = std::move(actors);
        if (!ok.empty()) {
            engine_ = std::make_unique<DancerEngine>(std::move(ok));
            ready_.store(true);
        }
    });
}

Dancer::~Dancer() {
    cancel_ = true;
    if (loader_.joinable()) loader_.join();
}

bool Dancer::redZone(const std::optional<double>& x) {
    if (!x)                          inRed_ = false;
    else if (inRed_ && *x < hide_)   inRed_ = false;
    else if (!inRed_ && *x >= show_) inRed_ = true;
    return inRed_;
}

const gif::Image* Dancer::update(double now, const std::optional<int>& bpmShown,
                                 const std::optional<bpm::Grid>& grid,
                                 const std::optional<double>& intensity, double offsetSec) {
    if (!ready_.load()) return nullptr;
    const bool show = redZone(intensity);
    const auto out = engine_->step(now, show ? bpmShown : std::nullopt,
                                   show ? grid : std::nullopt, offsetSec);
    if (!out.actor || out.actor->frames.empty()) return nullptr;
    const int f = std::max(0, std::min(out.frame, static_cast<int>(out.actor->frames.size()) - 1));
    return &out.actor->frames[static_cast<std::size_t>(f)];
}

} // namespace dancer
