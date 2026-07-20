#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "image.h"

struct Rgb24 {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

// Loads an RGB24 view of the input image. Cached RGB preserves the source's
// chroma resolution; otherwise an in-memory I420 buffer is converted via sws_scale.
std::optional<Rgb24> load_rgb24(const Image& img);
