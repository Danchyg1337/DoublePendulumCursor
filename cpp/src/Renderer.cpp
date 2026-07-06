#include "Renderer.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr int   N = cfg::CANVAS;
constexpr double INV255 = 1.0 / 255.0;

inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
} // namespace

Renderer::Renderer()
    : accum_(static_cast<std::size_t>(N) * N * 4, 0.0f),
      bgra_(static_cast<std::size_t>(N) * N * 4, 0) {}

void Renderer::clear() {
    std::fill(accum_.begin(), accum_.end(), 0.0f);
}

// Source-over compositing of one opaque colour at the given coverage
// (== source alpha) into the premultiplied accumulation buffer.
void Renderer::blend(int x, int y, double coverage, const cfg::Rgb& c) {
    if (x < 0 || y < 0 || x >= N || y >= N) return;
    const double a = clamp01(coverage);
    if (a <= 0.0) return;

    const std::size_t i = (static_cast<std::size_t>(y) * N + x) * 4;
    const double sr = c.r * INV255 * a;   // premultiplied source
    const double sg = c.g * INV255 * a;
    const double sb = c.b * INV255 * a;
    const double inv = 1.0 - a;

    accum_[i + 0] = static_cast<float>(sr + accum_[i + 0] * inv);
    accum_[i + 1] = static_cast<float>(sg + accum_[i + 1] * inv);
    accum_[i + 2] = static_cast<float>(sb + accum_[i + 2] * inv);
    accum_[i + 3] = static_cast<float>(a  + accum_[i + 3] * inv);
}

void Renderer::fillCircle(double cx, double cy, double radius, const cfg::Rgb& c) {
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius - 1.0)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius - 1.0)));
    const int x1 = std::min(N - 1, static_cast<int>(std::ceil(cx + radius + 1.0)));
    const int y1 = std::min(N - 1, static_cast<int>(std::ceil(cy + radius + 1.0)));

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const double dx = (x + 0.5) - cx;
            const double dy = (y + 0.5) - cy;
            const double dist = std::sqrt(dx * dx + dy * dy);
            // 1px-wide anti-aliased edge.
            blend(x, y, radius + 0.5 - dist, c);
        }
    }
}

// Round-capped, anti-aliased thick line via signed distance to the segment.
void Renderer::drawSegment(double ax, double ay, double bx, double by,
                           double width, const cfg::Rgb& c) {
    const double half = width * 0.5;
    const double pad = half + 1.0;
    const int x0 = std::max(0, static_cast<int>(std::floor(std::min(ax, bx) - pad)));
    const int y0 = std::max(0, static_cast<int>(std::floor(std::min(ay, by) - pad)));
    const int x1 = std::min(N - 1, static_cast<int>(std::ceil(std::max(ax, bx) + pad)));
    const int y1 = std::min(N - 1, static_cast<int>(std::ceil(std::max(ay, by) + pad)));

    const double vx = bx - ax;
    const double vy = by - ay;
    const double len2 = vx * vx + vy * vy;

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const double px = (x + 0.5) - ax;
            const double py = (y + 0.5) - ay;
            double t = (len2 > 1e-12) ? (px * vx + py * vy) / len2 : 0.0;
            t = clamp01(t);
            const double dx = px - t * vx;
            const double dy = py - t * vy;
            const double dist = std::sqrt(dx * dx + dy * dy);
            blend(x, y, half + 0.5 - dist, c);
        }
    }
}

// Convert premultiplied accumulation to straight-alpha BGRA8.
void Renderer::pack() {
    for (std::size_t p = 0; p < static_cast<std::size_t>(N) * N; ++p) {
        const std::size_t i = p * 4;
        const double a = accum_[i + 3];
        std::uint8_t B = 0, G = 0, R = 0, A = 0;
        if (a > 0.0) {
            const double invA = 1.0 / a;
            R = static_cast<std::uint8_t>(std::lround(clamp01(accum_[i + 0] * invA) * 255.0));
            G = static_cast<std::uint8_t>(std::lround(clamp01(accum_[i + 1] * invA) * 255.0));
            B = static_cast<std::uint8_t>(std::lround(clamp01(accum_[i + 2] * invA) * 255.0));
            A = static_cast<std::uint8_t>(std::lround(clamp01(a) * 255.0));
        }
        bgra_[i + 0] = B;
        bgra_[i + 1] = G;
        bgra_[i + 2] = R;
        bgra_[i + 3] = A;
    }
}

const std::uint8_t* Renderer::render(double theta1, double theta2) {
    clear();

    const double px = cfg::HOTSPOT_X;
    const double py = cfg::HOTSPOT_Y;
    const double x1 = px + cfg::L1 * std::sin(theta1);
    const double y1 = py + cfg::L1 * std::cos(theta1);
    const double x2 = x1 + cfg::L2 * std::sin(theta2);
    const double y2 = y1 + cfg::L2 * std::cos(theta2);

    // Draw order matches the original: rods, then pivot, then the two bobs.
    drawSegment(px, py, x1, y1, cfg::ROD_WIDTH, cfg::COLOR_ROD);
    drawSegment(x1, y1, x2, y2, cfg::ROD_WIDTH, cfg::COLOR_ROD);
    fillCircle(px, py, cfg::PIVOT_RADIUS, cfg::COLOR_PIVOT);
    fillCircle(x1, y1, cfg::BOB_RADIUS,  cfg::COLOR_BOB1);
    fillCircle(x2, y2, cfg::BOB_RADIUS,  cfg::COLOR_BOB2);

    pack();
    return bgra_.data();
}
