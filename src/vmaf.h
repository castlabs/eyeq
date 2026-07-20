#pragma once

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

extern "C" {
#include <libvmaf/libvmaf.h>
#include <libvmaf/model.h>
#include <libvmaf/picture.h>
}

#include "metrics.h"

// RAII wrapper managing the full VMAF lifecycle: context, model, pictures
struct VmafSession {
    VmafContext* ctx = nullptr;
    VmafModel* model = nullptr;
    VmafPicture ref_pic = {};
    VmafPicture dist_pic = {};
    bool ref_valid = false;
    bool dist_valid = false;

    VmafSession() = default;
    VmafSession(const VmafSession&) = delete;
    VmafSession& operator=(const VmafSession&) = delete;

    VmafSession(VmafSession&& o) noexcept
      : ctx(std::exchange(o.ctx, nullptr)),
        model(std::exchange(o.model, nullptr)),
        ref_pic(o.ref_pic),
        dist_pic(o.dist_pic),
        ref_valid(std::exchange(o.ref_valid, false)),
        dist_valid(std::exchange(o.dist_valid, false)) {}

    VmafSession& operator=(VmafSession&& o) noexcept {
        if (this != &o) {
            cleanup();
            ctx = std::exchange(o.ctx, nullptr);
            model = std::exchange(o.model, nullptr);
            ref_pic = o.ref_pic;
            dist_pic = o.dist_pic;
            ref_valid = std::exchange(o.ref_valid, false);
            dist_valid = std::exchange(o.dist_valid, false);
        }
        return *this;
    }

    ~VmafSession() { cleanup(); }

    bool init() {
        VmafConfiguration cfg = {};
        cfg.log_level = VMAF_LOG_LEVEL_NONE;
        cfg.n_threads = 0;
        cfg.n_subsample = 1;
        cfg.cpumask = 0;
        cfg.gpumask = 0;
        return vmaf_init(&ctx, cfg) == 0;
    }

    bool load_model(const char* model_name = "vmaf_v0.6.1") {
        VmafModelConfig cfg = {};
        cfg.name = "vmaf";
        cfg.flags = VMAF_MODEL_FLAGS_DEFAULT;
        if (vmaf_model_load(&model, &cfg, model_name) != 0)
            return false;
        if (vmaf_use_features_from_model(ctx, model) != 0)
            return false;
        return true;
    }

    bool alloc_pictures(int w, int h) {
        if (vmaf_picture_alloc(&ref_pic, VMAF_PIX_FMT_YUV420P, 8, w, h) != 0)
            return false;
        ref_valid = true;
        if (vmaf_picture_alloc(&dist_pic, VMAF_PIX_FMT_YUV420P, 8, w, h) != 0) {
            vmaf_picture_unref(&ref_pic);
            ref_valid = false;
            return false;
        }
        dist_valid = true;
        return true;
    }

    bool use_feature(const char* name) { return vmaf_use_feature(ctx, name, nullptr) == 0; }

    std::optional<float> get_feature_score(const char* name, int index) {
        double score = 0.0;
        if (vmaf_feature_score_at_index(ctx, name, &score, index) != 0)
            return std::nullopt;
        return static_cast<float>(score);
    }

    void copy_plane_data(const Image& ref, const Image& dist) {
        copy_image_data(ref, ref_pic);
        copy_image_data(dist, dist_pic);
    }

    static void copy_image_data(const Image& src, VmafPicture& dst) {
        for (int i = 0; i < 3; ++i) {
            const int rows = std::min(src.plane_height(i), static_cast<int>(dst.h[i]));
            const size_t row_bytes = std::min(static_cast<size_t>(src.plane_width(i)), static_cast<size_t>(dst.w[i]));
            const uint8_t* src_row = src.data.data() + src.plane_offset(i);
            uint8_t* dst_row = static_cast<uint8_t*>(dst.data[i]);
            for (int row = 0; row < rows; ++row) {
                std::memcpy(dst_row, src_row, row_bytes);
                src_row += src.plane_stride(i);
                dst_row += dst.stride[i];
            }
        }
    }

    bool feed_pictures(int index) {
        int ret = vmaf_read_pictures(ctx, &ref_pic, &dist_pic, index);
        if (ret == 0) {
            ref_valid = false;
            dist_valid = false;
        }
        return ret == 0;
    }

    bool flush() { return vmaf_read_pictures(ctx, nullptr, nullptr, 0) == 0; }

    std::optional<float> get_score(int index) {
        double score = 0.0;
        if (vmaf_score_at_index(ctx, model, &score, index) != 0)
            return std::nullopt;
        return static_cast<float>(score);
    }

private:
    void cleanup() {
        if (ref_valid)
            vmaf_picture_unref(&ref_pic);
        if (dist_valid)
            vmaf_picture_unref(&dist_pic);
        if (model)
            vmaf_model_destroy(model);
        if (ctx)
            vmaf_close(ctx);
    }
};

class VmafBase : public Metrics {
public:
    using Metrics::Metrics;

protected:
    std::optional<float> compute(const Image& ref, const Image& dist, const char* model_name) noexcept {
        if (colorspace_ != ColorSpace::I420)
            return std::nullopt;

        VmafSession runner;
        if (!runner.init())
            return std::nullopt;
        if (!runner.load_model(model_name))
            return std::nullopt;
        if (!runner.alloc_pictures(ref.width, ref.height))
            return std::nullopt;

        runner.copy_plane_data(ref, dist);
        if (!runner.feed_pictures(0))
            return std::nullopt;
        if (!runner.flush())
            return std::nullopt;
        return runner.get_score(0);
    }
};

class Vmaf : public VmafBase {
public:
    using VmafBase::VmafBase;

    const char* name() const override { return "VMAF"; }

    std::optional<std::vector<Score>> measure(const Image& ref, const Image& dist) noexcept override {
        auto s = compute(ref, dist, "vmaf_v0.6.1");
        if (!s)
            return std::nullopt;
        return std::vector<Score>{{"VMAF", *s}};
    }
};

class VmafNeg : public VmafBase {
public:
    using VmafBase::VmafBase;

    const char* name() const override { return "VMAF-NEG"; }

    std::optional<std::vector<Score>> measure(const Image& ref, const Image& dist) noexcept override {
        auto s = compute(ref, dist, "vmaf_v0.6.1neg");
        if (!s)
            return std::nullopt;
        return std::vector<Score>{{"VMAF-NEG", *s}};
    }
};
