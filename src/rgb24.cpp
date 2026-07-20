#include "rgb24.h"

#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace {

std::optional<Rgb24> from_image_i420(const Image& img) {
    if (img.colorspace != ColorSpace::I420)
        return std::nullopt;

    SwsContext* sws = sws_getContext(img.width, img.height, AV_PIX_FMT_YUV420P, img.width, img.height, AV_PIX_FMT_RGB24, SWS_BICUBIC | SWS_ACCURATE_RND,
                                     nullptr, nullptr, nullptr);
    if (!sws)
        return std::nullopt;

    // Match the main loader's normalization (BT.709, full range).
    sws_setColorspaceDetails(sws, sws_getCoefficients(SWS_CS_ITU709), 1 /* full range */, sws_getCoefficients(SWS_CS_ITU709), 1, 0, 1 << 16, 1 << 16);

    const uint8_t* src[4] = {
            img.data.data() + img.plane_offset(0),
            img.data.data() + img.plane_offset(1),
            img.data.data() + img.plane_offset(2),
            nullptr,
    };
    int src_stride[4] = {img.width, (img.width + 1) / 2, (img.width + 1) / 2, 0};

    Rgb24 out;
    out.width = img.width;
    out.height = img.height;
    out.pixels.resize(static_cast<size_t>(img.width) * img.height * 3);
    uint8_t* dst[4] = {out.pixels.data(), nullptr, nullptr, nullptr};
    int dst_stride[4] = {img.width * 3, 0, 0, 0};

    sws_scale(sws, src, src_stride, 0, img.height, dst, dst_stride);
    sws_freeContext(sws);
    return out;
}

std::optional<Rgb24> from_path(const std::string& path) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0)
        return std::nullopt;
    struct FmtGuard {
        ~FmtGuard() { avformat_close_input(&fmt_); }
        AVFormatContext* fmt_;
    } fmt_guard{fmt};

    if (avformat_find_stream_info(fmt, nullptr) < 0)
        return std::nullopt;

    const int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vi < 0)
        return std::nullopt;

    const AVCodec* codec = avcodec_find_decoder(fmt->streams[vi]->codecpar->codec_id);
    if (!codec)
        return std::nullopt;

    AVCodecContext* cc = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(cc, fmt->streams[vi]->codecpar);
    if (avcodec_open2(cc, codec, nullptr) < 0) {
        avcodec_free_context(&cc);
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
    if (!got_frame)
        return std::nullopt;

    SwsContext* sws = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), frame->width, frame->height, AV_PIX_FMT_RGB24,
                                     SWS_BICUBIC | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
    if (!sws)
        return std::nullopt;

    Rgb24 out;
    out.width = frame->width;
    out.height = frame->height;
    out.pixels.resize(static_cast<size_t>(frame->width) * frame->height * 3);

    uint8_t* dst[4] = {out.pixels.data(), nullptr, nullptr, nullptr};
    int dst_stride[4] = {frame->width * 3, 0, 0, 0};
    sws_scale(sws, (const uint8_t* const*)frame->data, frame->linesize, 0, frame->height, dst, dst_stride);
    sws_freeContext(sws);

    return out;
}

} // namespace

std::optional<Rgb24> load_rgb24(const Image& img) {
    if (img.rgb24)
        return *img.rgb24;
    if (!img.data.empty())
        return from_image_i420(img);
    return from_path(img.path);
}
