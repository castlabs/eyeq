"""End-to-end RGB16 PSNR tests; requires a built eyeq and ffmpeg on PATH."""

import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


BINARY = Path(sys.argv.pop(1) if len(sys.argv) > 1 else "build/eyeq").resolve()
LABELS = ("PSNR", "PSNR (R)", "PSNR (G)", "PSNR (B)")


class Rgb16Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.width, self.height = 5, 3  # Odd dimensions exercise decoded row padding.
        self.ref = [((i * 3571) + 17) % 65536 for i in range(45)]
        self.dist = [max(0, value - (i % 3 + 1)) for i, value in enumerate(self.ref)]

    def raw(self, name, values, fmt="rgb48le"):
        path = self.root / name
        if fmt.startswith("gbrp"):
            values = values[1::3] + values[2::3] + values[0::3]
        byte_order = ">" if fmt.endswith("be") else "<"
        path.write_bytes(struct.pack(byte_order + "H" * len(values), *values))
        return path

    def run_eyeq(self, *args, success=True):
        result = subprocess.run([str(BINARY), *map(str, args)], capture_output=True, text=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def scores(self, *args):
        output = self.run_eyeq(*args).stdout
        return {label: float(value.split()[0]) for label, value in (line.split(": ") for line in output.splitlines())}

    def raw_scores(self, ref=None, dist=None, *flags, fmt="rgb48le"):
        a = self.raw("reference.bin", self.ref if ref is None else ref, fmt)
        b = self.raw("distorted.bin", self.dist if dist is None else dist, fmt)
        return self.scores("--format", fmt, "--width", self.width, "--height", self.height, *flags, a, b)

    def assert_scores(self, actual, expected):
        self.assertEqual(set(actual), set(expected))
        for label, score in expected.items():
            if math.isinf(score):
                self.assertEqual(actual[label], score)
            else:
                self.assertAlmostEqual(actual[label], score, delta=0.0006, msg=label)

    def test_one_code_value_error_is_preserved(self):
        score = 20 * math.log10(65535)
        self.assert_scores(self.raw_scores([32768] * 45, [32769] * 45), dict.fromkeys(LABELS, score))

    def test_combined_uses_mse_and_equal_channel_weights(self):
        errors = (1, 2, 3)
        expected = {LABELS[c + 1]: 20 * math.log10(65535 / errors[c]) for c in range(3)}
        expected["PSNR"] = 10 * math.log10(65535**2 / (14 / 3))
        self.assert_scores(self.raw_scores(), expected)

    def test_identical(self):
        self.assert_scores(self.raw_scores(self.ref, self.ref), dict.fromkeys(LABELS, math.inf))
        self.assert_scores(self.raw_scores(self.ref, self.ref, "--logc4"), dict.fromkeys(LABELS, math.inf))

    def test_maximum_error_does_not_overflow(self):
        self.assert_scores(self.raw_scores([0] * 45, [65535] * 45), dict.fromkeys(LABELS, 0.0))

    def test_single_channel_change(self):
        ref = [1000, 2000, 3000] * 15
        dist = [1000, 2000, 3100] * 15
        blue = 20 * math.log10(65535 / 100)
        expected = {"PSNR": blue + 10 * math.log10(3), "PSNR (R)": math.inf, "PSNR (G)": math.inf, "PSNR (B)": blue}
        self.assert_scores(self.raw_scores(ref, dist), expected)

    def test_metric_selection(self):
        self.assertEqual(set(self.raw_scores(None, None, "--psnr")), {"PSNR"})
        self.assertEqual(set(self.raw_scores(None, None, "--psnr-g", "--psnr-b")), {"PSNR (G)", "PSNR (B)"})
        self.assertEqual(set(self.raw_scores(None, None, "--all")), set(LABELS))

    def test_endianness_and_planar_channel_order(self):
        expected = self.raw_scores()
        for fmt in ("rgb48be", "gbrp16le", "gbrp16be"):
            with self.subTest(fmt=fmt):
                self.assert_scores(self.raw_scores(fmt=fmt), expected)

    def test_logc4_linear_light_and_unclipped_range(self):
        # Independent scalar oracle, including negative scene values and highlights.
        def linear(code):
            return (2 ** ((code / 65535 * 1023 - 95) * 14 / 928 + 6) - 64) * 117.45 / 262128

        ref = [0, 30000, 65535] * 15
        dist = [5000, 29000, 60000] * 15
        errors = [(linear(a) - linear(b)) ** 2 for a, b in zip(ref[:3], dist[:3])]
        expected = {LABELS[c + 1]: -10 * math.log10(errors[c]) for c in range(3)}
        expected["PSNR"] = -10 * math.log10(sum(errors) / 3)
        self.assert_scores(self.raw_scores(ref, dist, "--logc4"), expected)
        self.assertLess(expected["PSNR (B)"], 0)

    def test_logc4_reference_table(self):
        # ARRI appendix B: 0% code -> -0.0181; 100% code -> 469.8000.
        expected = -20 * math.log10(469.8000 - (-0.0181))
        scores = self.raw_scores([0] * 45, [65535] * 45, "--logc4")
        self.assert_scores(scores, dict.fromkeys(LABELS, expected))

    def test_explicit_peak(self):
        for flags in ((), ("--logc4",)):
            before = self.raw_scores(None, None, *flags)
            after = self.raw_scores(None, None, *flags, "--psnr-peak", "10")
            self.assert_scores(after, {label: value + 20 for label, value in before.items()})

    def tiff(self, raw, name):
        path = self.root / name
        subprocess.run([
            "ffmpeg", "-v", "error", "-f", "rawvideo", "-pixel_format", "rgb48le",
            "-video_size", f"{self.width}x{self.height}", "-i", str(raw),
            "-frames:v", "1", "-pix_fmt", "rgb48le", "-y", str(path),
        ], check=True, capture_output=True)
        return path

    def test_tiff_and_raw_equivalence_and_inferred_dimensions(self):
        a = self.raw("a.rgb", self.ref)
        b = self.raw("b.rgb", self.dist)
        a_tif = self.tiff(a, "a.tif")
        b_tif = self.tiff(b, "b.tif")
        for flags in ((), ("--logc4",)):
            expected = self.raw_scores(None, None, *flags)
            self.assert_scores(self.scores(*flags, a_tif, b_tif), expected)
            self.assert_scores(self.scores(*flags, "--format", "rgb48le", a_tif, b), expected)
            self.assert_scores(self.scores(*flags, "--format", "rgb48le", a, b_tif), expected)
            self.assert_scores(self.scores(*flags, "--format", "rgb48le", a_tif, a), dict.fromkeys(LABELS, math.inf))

    def test_unsupported_metric(self):
        a = self.tiff(self.raw("a.rgb", self.ref), "a.tif")
        result = self.run_eyeq("--ssim", a, a, success=False)
        self.assertIn("not supported for RGB16", result.stderr)

    def test_invalid_peak(self):
        for peak in ("0", "-1", "nan", "inf", "garbage", "1junk"):
            result = self.run_eyeq("--psnr-peak", peak, "a", "b", success=False)
            self.assertIn("positive finite", result.stderr)

    def test_missing_dimensions_and_truncated_input(self):
        a = self.raw("a.rgb", self.ref)
        result = self.run_eyeq("--format", "rgb48le", a, a, success=False)
        self.assertIn("--width and --height are required", result.stderr)
        b = self.root / "truncated.bin"
        b.write_bytes(b"\0" * 11)
        result = self.run_eyeq("--format", "rgb48le", "--width", 5, "--height", 3, a, b, success=False)
        self.assertIn("Cannot decode a complete raw frame", result.stderr)

    def test_mismatched_dimensions(self):
        a = self.tiff(self.raw("a.rgb", self.ref), "a.tif")
        self.width, self.height = 3, 5
        b = self.tiff(self.raw("b.rgb", self.dist), "b.tif")
        result = self.run_eyeq(a, b, success=False)
        self.assertIn("Dimension mismatch", result.stderr)

    def test_reject_mixed_depth_and_logc4_on_8bit(self):
        a = self.tiff(self.raw("a.rgb", self.ref), "a.tif")
        png = self.root / "eight.png"
        subprocess.run(["ffmpeg", "-v", "error", "-i", str(a), "-pix_fmt", "rgb24", str(png)], check=True, capture_output=True)
        result = self.run_eyeq(a, png, success=False)
        self.assertIn("both inputs", result.stderr)
        for flag in ("--logc4", "--psnr-r"):
            result = self.run_eyeq(flag, png, png, success=False)
            self.assertIn("16-bit RGB", result.stderr)


if __name__ == "__main__":
    unittest.main()
