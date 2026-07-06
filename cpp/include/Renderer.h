// Renderer.h -- draws the pendulum into a reusable BGRA pixel buffer.
//
// Platform independent and allocation-free per frame: the buffers are sized
// once in the constructor and reused, which is a big part of the CPU win over
// the Python version (which built a fresh PIL image every frame). Output is
// top-down, straight-alpha BGRA8 -- exactly the layout a Win32 32-bpp DIB
// section wants, so the Windows layer can memcpy it straight in.
#pragma once

#include "Config.h"
#include <cstdint>
#include <vector>

class Renderer {
public:
    Renderer();

    // Render the pose for the given angles and return a pointer to CANVAS*CANVAS
    // BGRA8 pixels. The pointer is stable for the life of the Renderer.
    const std::uint8_t* render(double theta1, double theta2);

    static constexpr int size()  { return cfg::CANVAS; }
    static constexpr int stride() { return cfg::CANVAS * 4; }
    const std::uint8_t* data() const { return bgra_.data(); }

private:
    // Anti-aliased primitives, composited source-over in premultiplied space.
    void clear();
    void blend(int x, int y, double coverage, const cfg::Rgb& c);
    void fillCircle(double cx, double cy, double radius, const cfg::Rgb& c);
    void drawSegment(double x0, double y0, double x1, double y1,
                     double width, const cfg::Rgb& c);
    void pack();

    // Premultiplied RGBA accumulation buffer (r, g, b, a per pixel, 0..1).
    std::vector<float>        accum_;
    // Final straight-alpha BGRA8 output.
    std::vector<std::uint8_t> bgra_;
};
