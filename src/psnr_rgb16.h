#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

// ARRI LogC4 Specification, section 4.1.2 (relative scene-linear decoding):
// https://www.arri.com/resource/blob/278790/f3318e8c9c65617d8c5ca3f8b3e32051/2023-05-arri-logc4-specification-data.pdf
// Unsigned 16-bit inputs normalize to [0, 1], so only the nonnegative code-value
// branch is needed. Decoded negative values and highlights above 1 are preserved.
inline double decode_logc4(uint16_t code) {
    constexpr double a = (262144.0 - 16.0) / 117.45;
    constexpr double b = (1023.0 - 95.0) / 1023.0;
    constexpr double c = 95.0 / 1023.0;
    return (std::exp2(14.0 * (code / 65535.0 - c) / b + 6.0) - 64.0) / a;
}

// Caller validates equal, nonempty RGB buffers. Result order: combined, R, G, B.
inline std::array<double, 4> psnr_rgb16(std::span<const uint16_t> ref, std::span<const uint16_t> dist, bool logc4, double peak) {
    std::vector<double> linear;
    if (logc4) {
        linear.resize(65536);
        for (size_t i = 0; i < linear.size(); ++i)
            linear[i] = decode_logc4(static_cast<uint16_t>(i));
    }

    std::array<double, 3> sse{};
    for (size_t i = 0; i < ref.size(); i += 3) {
        for (size_t c = 0; c < 3; ++c) {
            const double delta = logc4 ? linear[ref[i + c]] - linear[dist[i + c]] : (static_cast<double>(ref[i + c]) - dist[i + c]) / 65535.0;
            sse[c] += delta * delta;
        }
    }

    const auto score = [peak](double error, size_t count) {
        if (error == 0.0)
            return std::numeric_limits<double>::infinity();
        return 20.0 * std::log10(peak) - 10.0 * std::log10(error / static_cast<double>(count));
    };
    const size_t pixels = ref.size() / 3;
    return {score(sse[0] + sse[1] + sse[2], ref.size()), score(sse[0], pixels), score(sse[1], pixels), score(sse[2], pixels)};
}
