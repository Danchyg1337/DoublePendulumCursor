// Renderer.h -- draws the pendulum into a reusable BGRA pixel buffer.
//
// Allocation-free per frame: buffers are sized once in the constructor (from
// the loaded cfg::g geometry) and reused, a big part of the CPU win over the
// Python version. Output is top-down, straight-alpha BGRA8 -- exactly the
// layout a Win32 32-bpp DIB section wants, so the cursor layer memcpys it in.
#pragma once

#include "Config.h"
#include <cstdint>
#include <vector>

class Renderer {
public:
    // Sizes internal buffers from the *current* cfg::g -- construct AFTER the
    // config file has been loaded.
    Renderer();

    // Render the pose for the given angles; returns size()*size() BGRA8 pixels.
    // The pointer is stable for the life of the Renderer.
    const std::uint8_t* render(double theta1, double theta2);

    int size() const { return n_; }          // canvas edge in pixels
    int stride() const { return n_ * 4; }
    const std::uint8_t* data() const { return bgra_.data(); }

private:
    void clear();
    void blend(int x, int y, double coverage, const cfg::Rgb& c);
    void fillCircle(double cx, double cy, double radius, const cfg::Rgb& c);
    void drawSegment(double x0, double y0, double x1, double y1,
                     double width, const cfg::Rgb& c);
    void pack();

    int                       n_;      // canvas size (square)
    std::vector<float>        accum_;  // premultiplied rgba (r,g,b,a per pixel)
    std::vector<std::uint8_t> bgra_;   // straight-alpha BGRA8 output
};
