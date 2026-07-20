# eyeq

Objective image quality measurement tool.

## Prerequisites

Ubuntu / Debian:

```bash
sudo apt install build-essential clang cmake libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libfftw3-dev libhwy-dev pkg-config meson ninja-build nasm
```

`libvmaf` is not packaged in Ubuntu, so it's built from source into a project-local prefix:

```bash
./scripts/install-vmaf.sh
```

The script clones Netflix/vmaf at a pinned tag, builds it with Meson, and installs to `third_party/vmaf-install/` (gitignored). Run once per clone; CMake auto-detects it via `PKG_CONFIG_PATH`. If you already have libvmaf installed system-wide, skip this step.

macOS with Homebrew:

```bash
brew install cmake pkgconf ffmpeg fftw highway libvmaf
```

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

## Install

To install the `eyeq` binary system-wide (default prefix `/usr/local`):

```bash
./scripts/install.sh                       # asks for sudo if prefix needs it
./scripts/install.sh --prefix ~/.local     # user-local install, no sudo
```

Or directly via cmake:

```bash
sudo cmake --install build
```

Uninstall with `sudo rm /usr/local/bin/eyeq`.

## Usage

```bash
./build/eyeq [options] <reference> <distorted>
```

An example:
```bash
./build/eyeq --all meridian.png meridian.jpg
PSNR: 43.3639
PSNR (Y): 41.9193
SSIM: 0.990009
MS-SSIM: 0.987025
PSNR-HVS: 38.9623
XPSNR: 44.3294
XPSNR (Y): 42.9677
FSIM: 0.991833
FSIMc: 0.991704
MDSI: 0.012252
VMAF: 78.9845
VMAF-NEG: 76.9834
SSIMULACRA2: 58.309
```

### Options

| Flag         | Description |
| ------------ | ----------- |
| `--all`      | Enable every metric |
| `--format NAME` | FFmpeg pixel format to use when an input cannot be decoded as ordinary media |
| `--width N`  | Width of a raw input; may be omitted when inherited from a decoded peer |
| `--height N` | Height of a raw input; may be omitted when inherited from a decoded peer |
| `--psnr`     | Peak Signal-to-Noise Ratio, full frame (YUV 4:2:0 weighted 4:1:1) |
| `--psnr-y`   | Peak Signal-to-Noise Ratio, Y plane only |
| `--ssim`     | Structural Similarity Index (Y channel) |
| `--ms-ssim`  | Multi-Scale SSIM (Y channel) |
| `--psnr-hvs` | PSNR with Human Visual System weighting |
| `--xpsnr`    | Extended Perceptually Weighted PSNR, full frame (YUV 4:2:0 weighted 4:1:1; Fraunhofer HHI; algorithm ported from FFmpeg's `vf_xpsnr.c`) |
| `--xpsnr-y`  | Extended Perceptually Weighted PSNR, Y plane only |
| `--fsim`     | Feature Similarity Index, luminance only (Zhang et al. 2011; phase congruency + Scharr gradient) |
| `--fsimc`    | FSIM with chromatic component (YIQ I/Q channels) |
| `--mdsi`     | Mean Deviation Similarity Index (Nafchi et al. 2016; lower is better, 0 = identical) |
| `--vmaf`     | VMAF (model `vmaf_v0.6.1`) |
| `--vmaf-neg` | VMAF-NEG (model `vmaf_v0.6.1neg`; less gameable by enhancement filters like sharpening) |
| `--dssim`    | Multi-scale L\*a\*b\* structural dissimilarity (clean-room reimplementation of Lesinski's DSSIM; lower = better, 0 = identical) |
| `--ssimulacra2`, `--ssim2` | SSIMULACRA 2.1 perceptual quality (native port of the Cloudinary/JPEG XL metric math with a Highway-accelerated blur path; no libjxl/lcms/PNG temp-file path) |

libvmaf caps PSNR at 60 dB when planes are identical.

No flags defaults to `--psnr` only.

### Raw inputs

For each input, eyeq first asks FFmpeg to decode it as ordinary image or video media. If that fails, the input is retried as headerless raw video. Detection does not depend on the filename extension, so names such as `.yuv`, `.rgb`, `.y8`, `.nv12`, `.yuy2`, `.raw`, and `.bin` all work.

Use `--format` with an FFmpeg pixel-format name. Names are passed through strictly: use `yuv420p`, not `i420`. Supported formats include `nv12`, `gray12le`, `p010le`, and planar 4:2:0, 4:2:2, and 4:4:4 at 10, 12, and 16 bits, as well as other software formats accepted by FFmpeg and libswscale.

Two raw inputs require dimensions:

```bash
./build/eyeq --all --format nv12 --width 1920 --height 1080 ref.nv12 distorted.raw
./build/eyeq --format yuv444p12le --width 2048 --height 1080 ref.rgb distorted.bin
```

When only one input needs raw fallback, its dimensions are inherited from the decoded peer unless explicitly supplied:

```bash
./build/eyeq --format yuv444p12le ref.rgb distorted.jpg
./build/eyeq --format gray12le ref.y8 distorted.png
```

Without `--format`, raw fallback defaults to `yuv420p`.

Raw inputs carry no color metadata. They are currently interpreted as BT.709, full range, and converted to the existing 8-bit I420/RGB24 metric pipeline. Pre-convert raw inputs that use another matrix or range; for example, to normalize limited-range BT.709 while retaining 10-bit samples:

```bash
ffmpeg -f rawvideo -pixel_format yuv420p10le -video_size 2048x1080 -i input.yuv \
  -vf "scale=in_color_matrix=bt709:in_range=tv:out_color_matrix=bt709:out_range=full" \
  -pix_fmt yuv420p10le -f rawvideo output.yuv
```

### Examples

Single metric (default PSNR):

```bash
./build/eyeq ref.png distorted.png
```

All metrics:

```bash
./build/eyeq --all ref.jpg distorted.jpg
```
