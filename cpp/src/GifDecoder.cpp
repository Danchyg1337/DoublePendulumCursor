#include "GifDecoder.h"

#include <algorithm>
#include <cmath>

namespace gif {

namespace {

struct Reader {
    const std::uint8_t* p;
    std::size_t n, i = 0;
    bool ok = true;
    std::uint8_t u8() {
        if (i >= n) { ok = false; return 0; }
        return p[i++];
    }
    int u16() { int a = u8(); int b = u8(); return a | (b << 8); }
    void skip(std::size_t k) { if (i + k > n) { ok = false; i = n; } else i += k; }
    // Concatenate data sub-blocks until the 0 terminator.
    void subBlocks(std::vector<std::uint8_t>* out) {
        while (ok) {
            const std::uint8_t len = u8();
            if (len == 0 || !ok) return;
            if (i + len > n) { ok = false; return; }
            if (out) out->insert(out->end(), p + i, p + i + len);
            i += len;
        }
    }
};

// LZW-decode `data` into `out` (expected count indices). Returns false on
// corruption (whatever was decoded is kept).
bool lzw(const std::vector<std::uint8_t>& data, int minCode, std::vector<std::uint8_t>& out,
         std::size_t expected) {
    if (minCode < 1 || minCode > 11) return false;
    const int clear = 1 << minCode, eoi = clear + 1;
    std::vector<std::uint16_t> prefix(4096, 0);
    std::vector<std::uint8_t> suffix(4096, 0), stack;
    stack.reserve(4096);
    for (int c = 0; c < clear; ++c) suffix[c] = static_cast<std::uint8_t>(c);

    int codeSize = minCode + 1, next = eoi + 1, old = -1;
    std::uint8_t first = 0;
    std::uint32_t acc = 0;
    int bits = 0;
    std::size_t pos = 0;
    out.clear();
    out.reserve(expected);

    while (out.size() < expected) {
        while (bits < codeSize) {
            if (pos >= data.size()) return out.size() >= expected;
            acc |= static_cast<std::uint32_t>(data[pos++]) << bits;
            bits += 8;
        }
        int code = static_cast<int>(acc & ((1u << codeSize) - 1));
        acc >>= codeSize;
        bits -= codeSize;

        if (code == clear) { codeSize = minCode + 1; next = eoi + 1; old = -1; continue; }
        if (code == eoi) break;
        if (old < 0) {
            if (code >= clear) return false;
            out.push_back(static_cast<std::uint8_t>(code));
            first = static_cast<std::uint8_t>(code);
            old = code;
            continue;
        }
        const int in = code;
        stack.clear();
        if (code >= next) {
            if (code > next) return false;
            stack.push_back(first);
            code = old;
        }
        while (code >= clear) {
            stack.push_back(suffix[code]);
            code = prefix[code];
            if (stack.size() > 4096) return false;
        }
        first = suffix[code];
        stack.push_back(first);
        for (auto it = stack.rbegin(); it != stack.rend() && out.size() < expected; ++it) out.push_back(*it);
        if (next < 4096) {
            prefix[next] = static_cast<std::uint16_t>(old);
            suffix[next] = first;
            ++next;
            if (next == (1 << codeSize) && codeSize < 12) ++codeSize;
        }
        old = in;
    }
    return true;
}

void readPalette(Reader& r, int count, std::vector<std::uint8_t>& pal) {
    pal.resize(static_cast<std::size_t>(count) * 3);
    for (auto& c : pal) c = r.u8();
}

} // namespace

int decode(const std::uint8_t* data, std::size_t size,
           const std::function<bool(const Image&, int)>& onFrame,
           int maxFrames, std::string* error) {
    auto fail = [&](const char* msg, int count) {
        if (error) *error = msg;
        return count;
    };
    Reader r{data, size};
    if (size < 13 || std::string(reinterpret_cast<const char*>(data), 3) != "GIF")
        return fail("not a GIF", 0);
    r.skip(6);
    Image canvas;
    canvas.w = r.u16();
    canvas.h = r.u16();
    const int packed = r.u8();
    r.u8();                      // background index (we dispose to transparent)
    r.u8();                      // aspect
    if (canvas.w <= 0 || canvas.h <= 0) return fail("bad GIF size", 0);
    canvas.rgba.assign(static_cast<std::size_t>(canvas.w) * canvas.h * 4, 0);
    std::vector<std::uint8_t> global, local;
    if (packed & 0x80) readPalette(r, 2 << (packed & 7), global);

    int count = 0;
    int disposal = 0, transparent = -1;
    // what to undo before the next frame
    int prevDisposal = 0, px = 0, py = 0, pw = 0, ph = 0;
    std::vector<std::uint8_t> saved, lzwData, idx;

    while (r.ok && count < maxFrames) {
        const std::uint8_t block = r.u8();
        if (!r.ok || block == 0x3B) break;                      // trailer
        if (block == 0x21) {                                    // extension
            const std::uint8_t label = r.u8();
            if (label == 0xF9) {                                // graphic control
                std::vector<std::uint8_t> gce;
                r.subBlocks(&gce);
                if (gce.size() >= 4) {
                    disposal = (gce[0] >> 2) & 7;
                    transparent = (gce[0] & 1) ? gce[3] : -1;
                }
            } else {
                r.subBlocks(nullptr);
            }
            continue;
        }
        if (block != 0x2C) return fail("unexpected GIF block", count);

        const int fx = r.u16(), fy = r.u16(), fw = r.u16(), fh = r.u16();
        const int fp = r.u8();
        const std::vector<std::uint8_t>* pal = &global;
        if (fp & 0x80) { readPalette(r, 2 << (fp & 7), local); pal = &local; }
        const bool interlaced = (fp & 0x40) != 0;
        const int minCode = r.u8();
        lzwData.clear();
        r.subBlocks(&lzwData);
        if (!r.ok) return fail("truncated GIF", count);

        // undo the previous frame per its disposal method
        if (prevDisposal == 2) {
            for (int y = std::max(0, py); y < std::min(canvas.h, py + ph); ++y)
                for (int x = std::max(0, px); x < std::min(canvas.w, px + pw); ++x)
                    std::fill_n(&canvas.rgba[(static_cast<std::size_t>(y) * canvas.w + x) * 4], 4, std::uint8_t{0});
        } else if (prevDisposal == 3 && !saved.empty()) {
            canvas.rgba = saved;
        }
        if (disposal == 3) saved = canvas.rgba;

        const std::size_t npx = static_cast<std::size_t>(fw) * fh;
        lzw(lzwData, minCode, idx, npx);           // tolerate truncated data
        const int ncolors = static_cast<int>(pal->size() / 3);
        // interlaced row order
        std::vector<int> rows(static_cast<std::size_t>(fh));
        if (interlaced) {
            int k = 0;
            const int starts[4] = {0, 4, 2, 1}, steps[4] = {8, 8, 4, 2};
            for (int pass = 0; pass < 4; ++pass)
                for (int y = starts[pass]; y < fh; y += steps[pass]) rows[k++] = y;
        } else {
            for (int y = 0; y < fh; ++y) rows[y] = y;
        }
        for (std::size_t i = 0; i < idx.size(); ++i) {
            const int c = idx[i];
            if (c == transparent || c >= ncolors) continue;
            const int x = fx + static_cast<int>(i % fw);
            const int y = fy + rows[i / fw];
            if (x < 0 || y < 0 || x >= canvas.w || y >= canvas.h) continue;
            std::uint8_t* d = &canvas.rgba[(static_cast<std::size_t>(y) * canvas.w + x) * 4];
            d[0] = (*pal)[c * 3];
            d[1] = (*pal)[c * 3 + 1];
            d[2] = (*pal)[c * 3 + 2];
            d[3] = 255;
        }
        const bool more = onFrame(canvas, count);
        ++count;
        prevDisposal = disposal; px = fx; py = fy; pw = fw; ph = fh;
        disposal = 0; transparent = -1;            // GCE applies to one image
        if (!more) break;
    }
    return count;
}

// ---- resampling ---------------------------------------------------------------
namespace {
struct Taps { int start; std::vector<double> w; };

std::vector<Taps> coefficients(int inSize, int outSize) {
    const double scale = static_cast<double>(inSize) / outSize;
    const double fscale = std::max(scale, 1.0);
    const double support = 1.0 * fscale;                  // triangle filter
    std::vector<Taps> taps(static_cast<std::size_t>(outSize));
    for (int o = 0; o < outSize; ++o) {
        const double center = (o + 0.5) * scale;
        int lo = static_cast<int>(std::floor(center - support));
        int hi = static_cast<int>(std::ceil(center + support));
        lo = std::max(lo, 0);
        hi = std::min(hi, inSize);
        Taps t{lo, {}};
        double sum = 0;
        for (int i = lo; i < hi; ++i) {
            const double x = std::fabs((i + 0.5 - center) / fscale);
            const double w = x < 1.0 ? 1.0 - x : 0.0;
            t.w.push_back(w);
            sum += w;
        }
        if (sum > 0) for (double& w : t.w) w /= sum;
        taps[o] = std::move(t);
    }
    return taps;
}
} // namespace

Image resizePremultiplied(const Image& src, int w, int h) {
    Image out;
    out.w = w; out.h = h;
    out.rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
    if (src.w <= 0 || src.h <= 0 || w <= 0 || h <= 0) return out;

    // premultiply into float
    std::vector<float> pm(static_cast<std::size_t>(src.w) * src.h * 4);
    for (std::size_t i = 0; i < pm.size(); i += 4) {
        const float a = src.rgba[i + 3] / 255.0f;
        pm[i + 0] = src.rgba[i + 0] * a;
        pm[i + 1] = src.rgba[i + 1] * a;
        pm[i + 2] = src.rgba[i + 2] * a;
        pm[i + 3] = src.rgba[i + 3];
    }
    const auto tx = coefficients(src.w, w);
    const auto ty = coefficients(src.h, h);
    // horizontal pass
    std::vector<float> tmp(static_cast<std::size_t>(w) * src.h * 4, 0.0f);
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            const Taps& t = tx[x];
            for (std::size_t k = 0; k < t.w.size(); ++k) {
                const float* s = &pm[(static_cast<std::size_t>(y) * src.w + t.start + k) * 4];
                for (int c = 0; c < 4; ++c) acc[c] += static_cast<float>(t.w[k]) * s[c];
            }
            std::copy(acc, acc + 4, &tmp[(static_cast<std::size_t>(y) * w + x) * 4]);
        }
    // vertical pass
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            const Taps& t = ty[y];
            for (std::size_t k = 0; k < t.w.size(); ++k) {
                const float* s = &tmp[((t.start + k) * static_cast<std::size_t>(w) + x) * 4];
                for (int c = 0; c < 4; ++c) acc[c] += static_cast<float>(t.w[k]) * s[c];
            }
            std::uint8_t* d = &out.rgba[(static_cast<std::size_t>(y) * w + x) * 4];
            const float a = std::min(255.0f, std::max(0.0f, acc[3]));
            for (int c = 0; c < 3; ++c)
                d[c] = static_cast<std::uint8_t>(std::lround(std::min(a, std::max(0.0f, acc[c]))));
            d[3] = static_cast<std::uint8_t>(std::lround(a));
        }
    return out;
}

} // namespace gif
