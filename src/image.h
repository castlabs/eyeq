#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/pixfmt.h>
}

enum class ColorSpace {
    I420,
};

struct Rgb24;

[[maybe_unused]]
static const char* color_space_name(ColorSpace cs) {
    switch (cs) {
    case ColorSpace::I420:
        return "I420";
    }
    return "unknown";
}

[[maybe_unused]]
static AVPixelFormat color_space_to_av_pix_fmt(ColorSpace cs) {
    switch (cs) {
    case ColorSpace::I420:
        return AV_PIX_FMT_YUV420P;
    }
    return AV_PIX_FMT_NONE;
}

struct Image {
    int width = 0;
    int height = 0;
    ColorSpace colorspace = ColorSpace::I420;
    std::vector<uint8_t> data;
    std::string path;
    std::shared_ptr<Rgb24> rgb24;
    // Native 16-bit RGB, interleaved in R/G/B order. When populated, data is empty
    // and only the RGB PSNR path may consume this image.
    std::vector<uint16_t> rgb16;

    std::span<const uint8_t> plane(int idx) const {
        int w = width, h = height;
        if (colorspace == ColorSpace::I420 && idx >= 1) {
            w = (w + 1) / 2;
            h = (h + 1) / 2;
        }
        const uint8_t* start = data.data() + plane_offset(idx);
        return {start, static_cast<size_t>(w * h)};
    }

    int plane_offset(int idx) const {
        if (colorspace == ColorSpace::I420) {
            int y_size = width * height;
            int uv_size = ((width + 1) / 2) * ((height + 1) / 2);
            if (idx == 0)
                return 0;
            if (idx == 1)
                return y_size;
            if (idx == 2)
                return y_size + uv_size;
        }
        return 0;
    }

    int plane_width(int idx) const {
        if (colorspace == ColorSpace::I420 && idx >= 1)
            return (width + 1) / 2;
        return width;
    }

    int plane_height(int idx) const {
        if (colorspace == ColorSpace::I420 && idx >= 1)
            return (height + 1) / 2;
        return height;
    }

    size_t plane_stride(int idx) const { return static_cast<size_t>(plane_width(idx)); }
};
