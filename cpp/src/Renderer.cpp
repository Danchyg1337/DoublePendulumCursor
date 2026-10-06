#include "Renderer.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr double INV255 = 1.0 / 255.0;

// 5x7 pixel font for the debug overlay ('#' = on).
const char* glyph(char ch) {
    switch (ch) {
        case '0': return " ### #   ##  ### # ###  ##   # ### ";
        case '1': return "  #   ##    #    #    #    #   ### ";
        case '2': return " ### #   #    #   #   #   #   #####";
        case '3': return "#####   #   #     #     ##   # ### ";
        case '4': return "   #   ##  # # #  # #####   #    # ";
        case '5': return "######    ####     #    ##   # ### ";
        case '6': return "  ##  #   #    #### #   ##   # ### ";
        case '7': return "#####    #   #   #   #    #    #   ";
        case '8': return " ### #   ##   # ### #   ##   # ### ";
        case '9': return " ### #   ##   # ####    #   #  ##  ";
        case '.': return "                          ##   ##  ";
        case '-': return "               #####               ";
        case ':': return "      ##   ##        ##   ##       ";
        case '*': return "     # # # ### ##### ### # # #     ";
        case 'A': return " ### #   ##   #######   ##   ##   #";
        case 'B': return "#### #   ##   ##### #   ##   ##### ";
        case 'D': return "#### #   ##   ##   ##   ##   ##### ";
        case 'E': return "######    #    #### #    #    #####";
        case 'F': return "######    #    #### #    #    #    ";
        case 'G': return " ### #   ##    # ####   ##   # ### ";
        case 'H': return "#   ##   ##   #######   ##   ##   #";
        case 'I': return " ###   #    #    #    #    #   ### ";
        case 'L': return "#    #    #    #    #    #    #####";
        case 'M': return "#   ### ### # ## # ##   ##   ##   #";
        case 'N': return "#   ###  ## # ##  ###   ##   ##   #";
        case 'O': return " ### #   ##   ##   ##   ##   # ### ";
        case 'P': return "#### #   ##   ##### #    #    #    ";
        case 'R': return "#### #   ##   ##### # #  #  # #   #";
        case 'S': return " #####    #     ###     #    ##### ";
        case 'T': return "#####  #    #    #    #    #    #  ";
        case 'W': return "#   ##   ##   ## # ## # ### ###   #";
        default:  return nullptr;
    }
}
inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
} // namespace

Renderer::Renderer()
    : n_(cfg::g.canvas()),
      accum_(static_cast<std::size_t>(n_) * n_ * 4, 0.0f),
      bgra_(static_cast<std::size_t>(n_) * n_ * 4, 0) {}

void Renderer::clear() {
    std::fill(accum_.begin(), accum_.end(), 0.0f);
}

// Source-over compositing of one opaque colour at the given coverage
// (== source alpha) into the premultiplied accumulation buffer.
void Renderer::blend(int x, int y, double coverage, const cfg::Rgb& c) {
    if (x < 0 || y < 0 || x >= n_ || y >= n_) return;
    const double a = clamp01(coverage);
    if (a <= 0.0) return;

    const std::size_t i = (static_cast<std::size_t>(y) * n_ + x) * 4;
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
    const int x1 = std::min(n_ - 1, static_cast<int>(std::ceil(cx + radius + 1.0)));
    const int y1 = std::min(n_ - 1, static_cast<int>(std::ceil(cy + radius + 1.0)));

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const double dx = (x + 0.5) - cx;
            const double dy = (y + 0.5) - cy;
            const double dist = std::sqrt(dx * dx + dy * dy);
            blend(x, y, radius + 0.5 - dist, c);   // 1px anti-aliased edge
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
    const int x1 = std::min(n_ - 1, static_cast<int>(std::ceil(std::max(ax, bx) + pad)));
    const int y1 = std::min(n_ - 1, static_cast<int>(std::ceil(std::max(ay, by) + pad)));

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
    for (std::size_t p = 0; p < static_cast<std::size_t>(n_) * n_; ++p) {
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

// Copy a premultiplied RGBA image into the (cleared) accumulation buffer.
void Renderer::drawOverlay(const gif::Image& im, int x0, int y0) {
    for (int y = 0; y < im.h; ++y) {
        const int cy = y0 + y;
        if (cy < 0 || cy >= n_) continue;
        for (int x = 0; x < im.w; ++x) {
            const int cx = x0 + x;
            if (cx < 0 || cx >= n_) continue;
            const std::uint8_t* p = &im.rgba[(static_cast<std::size_t>(y) * im.w + x) * 4];
            float* d = &accum_[(static_cast<std::size_t>(cy) * n_ + cx) * 4];
            d[0] = static_cast<float>(p[0] * INV255);
            d[1] = static_cast<float>(p[1] * INV255);
            d[2] = static_cast<float>(p[2] * INV255);
            d[3] = static_cast<float>(p[3] * INV255);
        }
    }
}

// Debug text: a dark 1-px (scaled) outline, then the glyphs on top.
void Renderer::drawText(const std::string& str, int x0, int y0, int scale, const cfg::Rgb& c) {
    static const cfg::Rgb outline{ 0, 0, 0 };
    for (int pass = 0; pass < 2; ++pass) {
        int x = x0;
        for (char ch : str) {
            const char* g = glyph(ch);
            if (g) {
                for (int i = 0; i < 35 && g[i]; ++i) {
                    if (g[i] != '#') continue;
                    const int gx = x + (i % 5) * scale, gy = y0 + (i / 5) * scale;
                    if (pass == 0) {
                        for (int dy = -1; dy <= scale; ++dy)
                            for (int dx = -1; dx <= scale; ++dx) blend(gx + dx, gy + dy, 0.85, outline);
                    } else {
                        for (int dy = 0; dy < scale; ++dy)
                            for (int dx = 0; dx < scale; ++dx) blend(gx + dx, gy + dy, 1.0, c);
                    }
                }
            }
            x += 6 * scale;
        }
    }
}

const std::uint8_t* Renderer::render(double theta1, double theta2, const gif::Image* overlay,
                                     const std::vector<TextLine>* text) {
    clear();
    const cfg::Settings& s = cfg::g;
    if (overlay)
        drawOverlay(*overlay, s.hotspot() + s.GIF_OFFSET_X, s.hotspot() + s.GIF_OFFSET_Y);

    const double px = s.hotspot();
    const double py = s.hotspot();
    const double x1 = px + s.L1 * std::sin(theta1);
    const double y1 = py + s.L1 * std::cos(theta1);
    const double x2 = x1 + s.L2 * std::sin(theta2);
    const double y2 = y1 + s.L2 * std::cos(theta2);

    // Draw order matches the original: rods, then pivot, then the two bobs.
    drawSegment(px, py, x1, y1, s.ROD_WIDTH, s.COLOR_ROD);
    drawSegment(x1, y1, x2, y2, s.ROD_WIDTH, s.COLOR_ROD);
    fillCircle(px, py, s.PIVOT_RADIUS, s.COLOR_PIVOT);
    fillCircle(x1, y1, s.BOB_RADIUS,   s.COLOR_BOB1);
    fillCircle(x2, y2, s.BOB_RADIUS,   s.COLOR_BOB2);

    if (text) {                       // debug lines above-right of the tip
        const int scale = 2, lineH = 8 * scale + 2;
        int y = std::max(1, s.hotspot() - static_cast<int>(text->size()) * lineH - 6);
        for (const TextLine& l : *text) {
            drawText(l.text, s.hotspot() + 14, y, scale, l.color);
            y += lineH;
        }
    }

    pack();
    return bgra_.data();
}
