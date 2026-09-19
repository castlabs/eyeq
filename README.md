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
| `--all`      | Enable every supported metric for the input type |
| `--format NAME` | FFmpeg pixel format to use when an input cannot be decoded as ordinary media |
| `--width N`  | Width of a raw input; may be omitted when inherited from a decoded peer |
| `--height N` | Height of a raw input; may be omitted when inherited from a decoded peer |
| `--logc4`    | Decode both RGB16 inputs from LogC4 to linear light before measuring PSNR |
| `--psnr-peak N` | RGB16 PSNR peak in linear units (default: 1.0) |
| `--psnr`     | Peak Signal-to-Noise Ratio, full frame (equal RGB weights for RGB16; YUV 4:2:0 weighted 4:1:1 otherwise) |
| `--psnr-r`, `--psnr-g`, `--psnr-b` | PSNR for an individual RGB16 channel |
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

For the 8-bit YUV pipeline, libvmaf caps PSNR at 60 dB when planes are identical. RGB16 PSNR is uncapped and reports `inf (identical)` for zero error.

No metric flags defaults to all four PSNR scores for RGB16, or `--psnr` only for other inputs. `--all` selects all supported metrics for the input type.

### 16-bit RGB and LogC4

16-bit-per-channel RGB TIFFs are detected automatically; for raw inputs, specify `--format rgb48le`. Both retain every sample bit. This path supports combined PSNR and PSNR (R), PSNR (G), and PSNR (B). Both inputs must have 16-bit RGB channels, matching dimensions, and the same gamut and encoding. There is no YUV conversion, chroma subsampling, or gamut transform. Alpha, if present, is ignored. Other metrics are rejected for RGB16.

For linear RGB16, samples are normalized from 0–65535 to 0–1:

```bash
./build/eyeq reference.tif distorted.tif
./build/eyeq --format rgb48le --width 2048 --height 858 reference.rgb distorted.rgb
```

For LogC4, add `--logc4` to decode **both** inputs into relative scene-linear RGB using [ARRI's LogC4 specification, section 4.1.2](https://www.arri.com/resource/blob/278790/f3318e8c9c65617d8c5ca3f8b3e32051/2023-05-arri-logc4-specification-data.pdf). The flag is explicit: encoding is not inferred from filenames or TIFF metadata.

```bash
./build/eyeq --logc4 reference.tif distorted.tif
./build/eyeq --logc4 --format rgb48le --width 2048 --height 858 reference.rgb distorted.rgb
# A TIFF supplies the dimensions for its raw peer:
./build/eyeq --logc4 --format rgb48le reference.tif distorted.rgb
```

LogC4 decoding preserves negative linear values and highlights above 1.0. PSNR uses `10 * log10(peak² / MSE)`, with **peak = 1.0 in linear units** by default for both encodings. Values outside that range are not clipped, so LogC4 comparisons can have negative PSNR. Use `--psnr-peak N` to choose a different fixed normalization (for example, `469.8` for the approximate linear value represented by LogC4 code 1.0); the peak is never estimated from image content.

Combined MSE is `(MSE_R + MSE_G + MSE_B) / 3`, not an average of channel dB scores. Use `--psnr` for only the combined score, or select individual channels with `--psnr-r`, `--psnr-g`, and `--psnr-b`.

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

Raw inputs carry no color metadata. RGB16 uses the native path described above. Other raw inputs are interpreted as BT.709, full range, and converted to the existing 8-bit I420/RGB24 metric pipeline. Pre-convert raw inputs that use another matrix or range; for example, to normalize limited-range BT.709 while retaining 10-bit samples:

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

### RGB16 regression tests

With Python 3 and FFmpeg available, run the numerical and input-format checks against a built binary:

```bash
python3 tests/test_rgb16.py ./build/eyeq
```
