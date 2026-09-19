#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include "image.h"
#include "metrics.h"
#include "psnr_rgb16.h"
#include "rgb24.h"

struct RawInputSpec {
    AVPixelFormat pixel_format = AV_PIX_FMT_NONE;
    int width = 0;
    int height = 0;
};

// image loading via ffmpeg (ONLY for loading + colorspace conversion)
static std::optional<Image> load_image(const char* path, ColorSpace cs, bool keep_rgb = false, const RawInputSpec* raw = nullptr, bool quiet = false) {
    const int previous_log_level = av_log_get_level();
    if (quiet)
        av_log_set_level(AV_LOG_QUIET);
    struct LogGuard {
        ~LogGuard() {
            if (restore_)
                av_log_set_level(level_);
        }
        int level_;
        bool restore_;
    } log_guard{previous_log_level, quiet};

    AVFormatContext* fmt = nullptr;
    const AVInputFormat* input_format = nullptr;
    AVDictionary* input_options = nullptr;
    struct DictGuard {
        ~DictGuard() { av_dict_free(&dict_); }
        AVDictionary*& dict_;
    } dict_guard{input_options};

    if (raw) {
        input_format = av_find_input_format("rawvideo");
        if (!input_format) {
            if (!quiet)
                std::cerr << "FFmpeg rawvideo demuxer is unavailable\n";
            return std::nullopt;
        }

        const char* pixel_format = av_get_pix_fmt_name(raw->pixel_format);
        if (!pixel_format) {
            if (!quiet)
                std::cerr << "Invalid raw pixel format\n";
            return std::nullopt;
        }
        const std::string video_size = std::to_string(raw->width) + "x" + std::to_string(raw->height);
        av_dict_set(&input_options, "pixel_format", pixel_format, 0);
        av_dict_set(&input_options, "video_size", video_size.c_str(), 0);
    }

    if (avformat_open_input(&fmt, path, input_format, &input_options) < 0) {
        if (!quiet)
            std::cerr << "Cannot open: " << path << '\n';
        return std::nullopt;
    }

    struct FmtGuard {
        ~FmtGuard() { avformat_close_input(&fmt_); }
        AVFormatContext* fmt_;
    } fmt_guard{fmt};

    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        if (!quiet)
            std::cerr << "No stream info: " << path << '\n';
        return std::nullopt;
    }

    const int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vi < 0) {
        if (!quiet)
            std::cerr << "No video stream: " << path << '\n';
        return std::nullopt;
    }

    const AVCodec* codec = avcodec_find_decoder(fmt->streams[vi]->codecpar->codec_id);
    if (!codec) {
        if (!quiet)
            std::cerr << "No decoder: " << path << '\n';
        return std::nullopt;
    }

    AVCodecContext* cc = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(cc, fmt->streams[vi]->codecpar);
    if (avcodec_open2(cc, codec, nullptr) < 0) {
        avcodec_free_context(&cc);
        if (!quiet)
            std::cerr << "Cannot open codec: " << path << '\n';
        return std::nullopt;
    }

    struct CcGuard {
        ~CcGuard() { avcodec_free_context(&cc_); }
        AVCodecContext* cc_;
    } cc_guard{cc};

    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();

    struct FrameGuard {
        ~FrameGuard() {
            av_frame_free(&frame_);
            av_packet_free(&pkt_);
        }
        AVFrame* frame_;
        AVPacket* pkt_;
    } frame_guard{frame, pkt};

    bool got_frame = false;
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == vi)
            if (avcodec_send_packet(cc, pkt) == 0 && avcodec_receive_frame(cc, frame) == 0)
                got_frame = true;
        av_packet_unref(pkt);
        if (got_frame)
            break;
    }

    if (!got_frame) {
        if (!quiet) {
            if (raw)
                std::cerr << "Cannot decode a complete raw frame: " << path << '\n';
            else
                std::cerr << "Cannot decode frame: " << path << '\n';
        }
        return std::nullopt;
    }

    const AVPixelFormat target_fmt = color_space_to_av_pix_fmt(cs);
    const AVPixelFormat src_fmt = static_cast<AVPixelFormat>(frame->format);
    const AVPixFmtDescriptor* src_desc = av_pix_fmt_desc_get(src_fmt);

    // Extract native RGB16 samples before any YUV conversion, resampling, or
    // quantization. Component descriptors cover packed/planar RGB and endianness.
    const bool src_is_rgb16 = src_desc && (src_desc->flags & AV_PIX_FMT_FLAG_RGB) && !(src_desc->flags & AV_PIX_FMT_FLAG_FLOAT) &&
                              src_desc->nb_components >= 3 && src_desc->comp[0].depth == 16 && src_desc->comp[1].depth == 16 &&
                              src_desc->comp[2].depth == 16;
    if (src_is_rgb16) {
        Image img;
        img.width = frame->width;
        img.height = frame->height;
        img.path = path;
        img.rgb16.resize(static_cast<size_t>(img.width) * img.height * 3);
        for (int y = 0; y < img.height; ++y) {
            for (int c = 0; c < 3; ++c) {
                const auto& component = src_desc->comp[c];
                const uint8_t* row = frame->data[component.plane] + static_cast<ptrdiff_t>(y) * frame->linesize[component.plane];
                for (int x = 0; x < img.width; ++x) {
                    const uint8_t* sample = row + static_cast<ptrdiff_t>(x) * component.step + component.offset;
                    img.rgb16[(static_cast<size_t>(y) * img.width + x) * 3 + c] =
                            src_desc->flags & AV_PIX_FMT_FLAG_BE ? AV_RB16(sample) : AV_RL16(sample);
                }
            }
        }
        return img;
    }

    // Always go through sws with explicit BT.709 + full-range output, regardless of
    // source format. Mixed sources (e.g., PNG-as-RGB vs JPEG-as-YUVJ) otherwise pick
    // up different default matrices/ranges and bias the comparison.
    SwsContext* sws = sws_getContext(frame->width, frame->height, src_fmt, frame->width, frame->height, target_fmt, SWS_BICUBIC | SWS_ACCURATE_RND, nullptr,
                                     nullptr, nullptr);
    if (!sws) {
        if (!quiet)
            std::cerr << "sws_getContext failed: " << path << '\n';
        return std::nullopt;
    }

    const bool src_is_rgb = src_desc && (src_desc->flags & AV_PIX_FMT_FLAG_RGB);
    const bool src_is_jpeg_yuv = src_fmt == AV_PIX_FMT_YUVJ420P || src_fmt == AV_PIX_FMT_YUVJ422P || src_fmt == AV_PIX_FMT_YUVJ444P ||
                                 src_fmt == AV_PIX_FMT_YUVJ411P || src_fmt == AV_PIX_FMT_YUVJ440P;
    const bool src_full_range = raw || src_is_rgb || src_is_jpeg_yuv || frame->color_range == AVCOL_RANGE_JPEG;
    const int src_matrix = raw || frame->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_ITU601;

    sws_setColorspaceDetails(sws, sws_getCoefficients(src_matrix), src_full_range ? 1 : 0, sws_getCoefficients(SWS_CS_ITU709), 1 /* full range */, 0, 1 << 16,
                             1 << 16);

    AVFrame* converted = av_frame_alloc();
    converted->width = frame->width;
    converted->height = frame->height;
    converted->format = target_fmt;
    av_frame_get_buffer(converted, 32);
    sws_scale(sws, (const uint8_t* const*)frame->data, frame->linesize, 0, frame->height, converted->data, converted->linesize);
    sws_freeContext(sws);

    struct ConvGuard {
        ~ConvGuard() { av_frame_free(&frame_); }
        AVFrame* frame_;
    } conv_guard{converted};

    Image img;
    img.width = converted->width;
    img.height = converted->height;
    img.colorspace = cs;
    img.path = path;

    int plane_sizes[3];
    for (int i = 0; i < 3; ++i) {
        int w = converted->width;
        int h = converted->height;
        if (cs == ColorSpace::I420 && i >= 1) {
            w = (w + 1) / 2;
            h = (h + 1) / 2;
        }
        plane_sizes[i] = w * h;
    }

    const int total_size = plane_sizes[0] + plane_sizes[1] + plane_sizes[2];
    img.data.resize(static_cast<size_t>(total_size));

    size_t offset = 0;
    for (int i = 0; i < 3; ++i) {
        int w = converted->width;
        int h = converted->height;
        if (cs == ColorSpace::I420 && i >= 1) {
            w = (w + 1) / 2;
            h = (h + 1) / 2;
        }
        const size_t plane_size = static_cast<size_t>(w * h);
        for (int row = 0; row < h; ++row)
            std::memcpy(img.data.data() + offset + row * w, converted->data[i] + row * converted->linesize[i], static_cast<size_t>(w));
        offset += plane_size;
    }

    if (keep_rgb) {
        SwsContext* rgb_sws = sws_getContext(frame->width, frame->height, src_fmt, frame->width, frame->height, AV_PIX_FMT_RGB24,
                                             SWS_BICUBIC | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
        if (!rgb_sws) {
            if (!quiet)
                std::cerr << "sws_getContext failed for RGB: " << path << '\n';
            return std::nullopt;
        }

        auto rgb = std::make_shared<Rgb24>();
        rgb->width = frame->width;
        rgb->height = frame->height;
        rgb->pixels.resize(static_cast<size_t>(frame->width) * frame->height * 3);
        uint8_t* rgb_dst[4] = {rgb->pixels.data(), nullptr, nullptr, nullptr};
        int rgb_dst_stride[4] = {frame->width * 3, 0, 0, 0};
        if (raw)
            sws_setColorspaceDetails(rgb_sws, sws_getCoefficients(SWS_CS_ITU709), 1 /* full range */, sws_getCoefficients(SWS_CS_ITU709), 1, 0, 1 << 16,
                                     1 << 16);
        sws_scale(rgb_sws, (const uint8_t* const*)frame->data, frame->linesize, 0, frame->height, rgb_dst, rgb_dst_stride);
        sws_freeContext(rgb_sws);
        img.rgb24 = std::move(rgb);
    }

    return img;
}

struct Options {
    std::vector<std::string_view> metrics;
    std::string_view ref_path;
    std::string_view dist_path;
    int width = 0;
    int height = 0;
    AVPixelFormat raw_format = AV_PIX_FMT_NONE;
    bool all = false;
    bool logc4 = false;
    std::optional<double> psnr_peak;
};

static constexpr std::string_view kAllMetrics[] = {"psnr", "psnr-y", "ssim", "ms-ssim", "psnr-hvs", "xpsnr", "xpsnr-y",
                                                   "fsim", "fsimc",  "mdsi", "vmaf",    "vmaf-neg", "dssim", "ssimulacra2"};
static constexpr std::string_view kRgb16Metrics[] = {"psnr", "psnr-r", "psnr-g", "psnr-b"};
static constexpr std::string_view kRgb16Labels[] = {"PSNR", "PSNR (R)", "PSNR (G)", "PSNR (B)"};

static bool metric_uses_rgb(std::string_view metric) {
    return metric == "fsim" || metric == "fsimc" || metric == "mdsi" || metric == "dssim" || metric == "ssimulacra2";
}

static void add_metric(Options& opts, std::string_view metric) {
    if (std::find(opts.metrics.begin(), opts.metrics.end(), metric) == opts.metrics.end())
        opts.metrics.push_back(metric);
}

static void print_help(std::ostream& os) {
    os << "Usage: eyeq [options] <reference> <distorted>\n"
          "\n"
          "Options:\n"
          "  -h, --help     Show this message and exit\n"
          "  --all          Enable every supported metric for the input type\n"
          "  --format NAME  Set the FFmpeg pixel format used when normal decoding fails\n"
          "  --logc4        Decode both RGB16 inputs from LogC4 to linear light\n"
          "  --psnr-peak N  RGB16 PSNR peak in linear units (default: 1.0)\n"
          "\n"
          "Metrics (range; direction):\n"
          "  --psnr         PSNR, combined RGB16 or full YUV frame              [dB; higher = better]\n"
          "  --psnr-y       PSNR, Y plane only                                  [dB; higher = better]\n"
          "  --psnr-r       PSNR, R channel only (RGB16 inputs)                  [dB; higher = better]\n"
          "  --psnr-g       PSNR, G channel only (RGB16 inputs)                  [dB; higher = better]\n"
          "  --psnr-b       PSNR, B channel only (RGB16 inputs)                  [dB; higher = better]\n"
          "  --ssim         Structural Similarity Index (Y)                     [0..1; higher = better, 1 = identical]\n"
          "  --ms-ssim      Multi-Scale SSIM (Y)                                [0..1; higher = better, 1 = identical]\n"
          "  --psnr-hvs     PSNR with Human Visual System weighting             [dB; higher = better]\n"
          "  --xpsnr        Extended Perceptually Weighted PSNR (HHI)           [dB; higher = better]\n"
          "  --xpsnr-y      XPSNR, Y plane only                                 [dB; higher = better]\n"
          "  --fsim         Feature Similarity Index, luminance only            [0..1; higher = better, 1 = identical]\n"
          "  --fsimc        FSIM with chromatic component (YIQ)                 [0..1; higher = better, 1 = identical]\n"
          "  --mdsi         Mean Deviation Similarity Index                     [~0..0.5; LOWER = better, 0 = identical]\n"
          "  --vmaf         VMAF (model vmaf_v0.6.1)                            [~0..100; higher = better]\n"
          "  --vmaf-neg     VMAF-NEG, less gameable by enhancement (vmaf_v0.6.1neg) [~0..100; higher = better]\n"
          "  --dssim        Multi-scale L*a*b* structural dissimilarity         [>=0; LOWER = better, 0 = identical]\n"
          "  --ssimulacra2  SSIMULACRA 2.1 perceptual quality                   [-inf..100; higher = better, 100 = identical]\n"
          "  --ssim2        Alias for --ssimulacra2\n"
          "\n"
          "Raw inputs:\n"
          "  --width N      Width in pixels\n"
          "  --height N     Height in pixels\n"
          "  --format uses FFmpeg pixel-format names (for example yuv420p, nv12, gray12le, rgb48le).\n"
          "  Each input is decoded normally first, then retried as raw if FFmpeg cannot decode it.\n"
          "  Raw dimensions come from --width/--height or a decoded peer. Without --format, raw fallback uses yuv420p.\n"
          "\n"
          "RGB16 inputs retain all 16 bits and default to combined PSNR and PSNR (R/G/B).\n"
          "  --psnr selects combined RGB PSNR, with equal channel weighting; identical inputs yield inf.\n"
          "  Samples are normalized by 65535 and assumed linear unless --logc4 is supplied.\n"
          "  LogC4 decoding preserves negative values and highlights above 1; PSNR can be negative.\n"
          "  Both inputs must be RGB16 in the same gamut/encoding. Only PSNR metrics are supported.\n"
          "Other inputs default to --psnr only and are converted to YUV 4:2:0, BT.709, full range.\n"
          "  YUV PSNR uses 4:1:1 plane weights; libvmaf caps identical planes at 60 dB.\n"
          "Raw YUV is assumed already in that space (no metadata to do otherwise).\n";
}

static bool parse_dimension(const char* option, const char* value, int& out) {
    const std::string_view text(value);
    int parsed = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (ec != std::errc{} || ptr != text.data() + text.size() || parsed <= 0) {
        std::cerr << option << " requires a positive integer\n";
        return false;
    }
    out = parsed;
    return true;
}

static bool parse_args(Options& opts, int argc, char* argv[]) {
    if (argc <= 1) {
        print_help(std::cout);
        std::exit(0);
    }

    std::vector<std::string_view> positional;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_help(std::cout);
            std::exit(0);
        } else if (arg == "--psnr")
            add_metric(opts, "psnr");
        else if (arg == "--psnr-y")
            add_metric(opts, "psnr-y");
        else if (arg == "--psnr-r" || arg == "--psnr-g" || arg == "--psnr-b")
            add_metric(opts, arg.substr(2));
        else if (arg == "--ssim")
            add_metric(opts, "ssim");
        else if (arg == "--ms-ssim")
            add_metric(opts, "ms-ssim");
        else if (arg == "--psnr-hvs")
            add_metric(opts, "psnr-hvs");
        else if (arg == "--xpsnr")
            add_metric(opts, "xpsnr");
        else if (arg == "--xpsnr-y")
            add_metric(opts, "xpsnr-y");
        else if (arg == "--mdsi")
            add_metric(opts, "mdsi");
        else if (arg == "--fsim")
            add_metric(opts, "fsim");
        else if (arg == "--fsimc")
            add_metric(opts, "fsimc");
        else if (arg == "--vmaf")
            add_metric(opts, "vmaf");
        else if (arg == "--vmaf-neg")
            add_metric(opts, "vmaf-neg");
        else if (arg == "--dssim")
            add_metric(opts, "dssim");
        else if (arg == "--ssimulacra2" || arg == "--ssim2")
            add_metric(opts, "ssimulacra2");
        else if (arg == "--width") {
            if (i + 1 >= argc) {
                std::cerr << "--width requires a value\n";
                return false;
            }
            if (!parse_dimension("--width", argv[++i], opts.width))
                return false;
        } else if (arg == "--height") {
            if (i + 1 >= argc) {
                std::cerr << "--height requires a value\n";
                return false;
            }
            if (!parse_dimension("--height", argv[++i], opts.height))
                return false;
        } else if (arg == "--format") {
            if (i + 1 >= argc) {
                std::cerr << "--format requires a value\n";
                return false;
            }
            const char* format_name = argv[++i];
            const AVPixelFormat pixel_format = av_get_pix_fmt(format_name);
            if (pixel_format == AV_PIX_FMT_NONE) {
                std::cerr << "Unknown FFmpeg pixel format: " << format_name << '\n';
                return false;
            }
            if (!sws_isSupportedInput(pixel_format)) {
                std::cerr << "FFmpeg pixel format is not supported as a conversion input: " << format_name << '\n';
                return false;
            }
            opts.raw_format = pixel_format;
        } else if (arg == "--logc4") {
            opts.logc4 = true;
        } else if (arg == "--psnr-peak") {
            if (i + 1 >= argc) {
                std::cerr << "--psnr-peak requires a value\n";
                return false;
            }
            const char* value = argv[++i];
            char* end = nullptr;
            const double peak = std::strtod(value, &end);
            if (end == value || *end != '\0' || !std::isfinite(peak) || peak <= 0.0) {
                std::cerr << "--psnr-peak requires a positive finite number\n";
                return false;
            }
            opts.psnr_peak = peak;
        } else if (arg == "--all") {
            opts.all = true;
        } else if (arg.starts_with("--")) {
            std::cerr << "Unknown option: " << arg << '\n';
            return false;
        } else
            positional.push_back(arg);
    }

    if (positional.size() < 2) {
        print_help(std::cerr);
        return false;
    }

    opts.ref_path = positional[0];
    opts.dist_path = positional[1];
    return true;
}

int main(int argc, char* argv[]) {
    av_log_set_level(AV_LOG_ERROR);

    Options opts;
    if (!parse_args(opts, argc, argv))
        return 1;

    const ColorSpace cs = ColorSpace::I420;

    const std::string ref_path(opts.ref_path);
    const std::string dist_path(opts.dist_path);
    const bool any_rgb_metric = opts.all || std::any_of(opts.metrics.begin(), opts.metrics.end(), metric_uses_rgb);

    const AVPixelFormat raw_format = opts.raw_format != AV_PIX_FMT_NONE ? opts.raw_format : AV_PIX_FMT_YUV420P;
    auto load_raw = [&](const std::string& path, int width, int height) {
        const RawInputSpec spec{raw_format, width, height};
        return load_image(path.c_str(), cs, any_rgb_metric, &spec);
    };

    // Try ordinary media decoding first, regardless of filename. An input that
    // FFmpeg cannot decode is retried as headerless raw video.
    std::optional<Image> ref = load_image(ref_path.c_str(), cs, any_rgb_metric, nullptr, true);
    std::optional<Image> dist = load_image(dist_path.c_str(), cs, any_rgb_metric, nullptr, true);

    if (!ref && !dist && (opts.width <= 0 || opts.height <= 0)) {
        std::cerr << "Both inputs require raw fallback; --width and --height are required\n";
        return 1;
    }

    if (!ref) {
        const int width = opts.width > 0 ? opts.width : dist->width;
        const int height = opts.height > 0 ? opts.height : dist->height;
        ref = load_raw(ref_path, width, height);
        if (!ref)
            return 1;
    }
    if (!dist) {
        const int width = opts.width > 0 ? opts.width : ref->width;
        const int height = opts.height > 0 ? opts.height : ref->height;
        dist = load_raw(dist_path, width, height);
    }

    if (!ref || !dist)
        return 1;

    if (ref->width != dist->width || ref->height != dist->height) {
        std::cerr << "Dimension mismatch: " << ref->width << "x" << ref->height << " vs " << dist->width << "x" << dist->height << '\n';
        return 1;
    }

    if (!ref->rgb16.empty() || !dist->rgb16.empty()) {
        if (ref->rgb16.empty() || dist->rgb16.empty()) {
            std::cerr << "RGB16 PSNR requires both inputs to have 16-bit RGB channels\n";
            return 1;
        }
        if (opts.all || opts.metrics.empty())
            for (const auto metric: kRgb16Metrics)
                add_metric(opts, metric);
        for (const auto metric: opts.metrics) {
            if (std::find(std::begin(kRgb16Metrics), std::end(kRgb16Metrics), metric) == std::end(kRgb16Metrics)) {
                std::cerr << "Metric --" << metric << " is not supported for RGB16 inputs; use --psnr, --psnr-r, --psnr-g, or --psnr-b\n";
                return 1;
            }
        }
        const auto scores = psnr_rgb16(ref->rgb16, dist->rgb16, opts.logc4, opts.psnr_peak.value_or(1.0));
        for (const auto metric: opts.metrics) {
            const size_t index = std::find(std::begin(kRgb16Metrics), std::end(kRgb16Metrics), metric) - std::begin(kRgb16Metrics);
            std::cout << kRgb16Labels[index] << ": ";
            if (std::isinf(scores[index]))
                std::cout << "inf (identical)\n";
            else
                std::cout << scores[index] << '\n';
        }
        return 0;
    }

    if (opts.logc4 || opts.psnr_peak) {
        std::cerr << "--logc4 and --psnr-peak require 16-bit RGB inputs\n";
        return 1;
    }
    for (const auto metric: opts.metrics) {
        if (metric == "psnr-r" || metric == "psnr-g" || metric == "psnr-b") {
            std::cerr << "--" << metric << " requires 16-bit RGB inputs\n";
            return 1;
        }
    }
    if (opts.all)
        for (const auto metric: kAllMetrics)
            add_metric(opts, metric);
    if (opts.metrics.empty())
        add_metric(opts, "psnr");

    int failed = 0;
    for (const auto& metric_name: opts.metrics) {
        auto metric = MetricsFactory::create(metric_name, cs, ref->width, ref->height);
        if (!metric) {
            std::cerr << "Unknown metric: " << metric_name << '\n';
            ++failed;
            continue;
        }

        auto scores = metric->measure(*ref, *dist);
        if (!scores) {
            std::cerr << metric_name << ": computation failed\n";
            ++failed;
            continue;
        }

        for (const auto& s: *scores) {
            if (std::isinf(s.value))
                std::cout << s.label << ": inf (identical)\n";
            else
                std::cout << s.label << ": " << s.value << '\n';
        }
    }

    return failed > 0 ? 1 : 0;
}
