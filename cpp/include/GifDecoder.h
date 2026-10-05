// GifDecoder.h -- small self-contained animated-GIF decoder + resampler.
//
// Decodes every frame (LZW, interlacing, local/global palettes, transparency)
// and composites it onto the logical screen with the frame disposal methods,
// so each callback receives the full frame as displayed -- the same thing
// Pillow's `im.seek(i); im.convert("RGBA")` gives the Python dancer.
// Platform independent.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gif {

struct Image {
    int w = 0, h = 0;
    std::vector<std::uint8_t> rgba;   // w*h*4
};

// Calls onFrame(canvas, index) for each frame (straight-alpha RGBA). Stops
// after maxFrames frames, or early if onFrame returns false. Returns the
// number of frames decoded; on a malformed file returns what was decoded so
// far and sets *error.
int decode(const std::uint8_t* data, std::size_t size,
           const std::function<bool(const Image&, int)>& onFrame,
           int maxFrames = 1 << 30, std::string* error = nullptr);

// Resample a straight-alpha RGBA image to w x h with an area-aware bilinear
// (triangle) filter like Pillow's Image.resize(BILINEAR). The result is
// PREMULTIPLIED RGBA, ready for compositing.
Image resizePremultiplied(const Image& src, int w, int h);

} // namespace gif
