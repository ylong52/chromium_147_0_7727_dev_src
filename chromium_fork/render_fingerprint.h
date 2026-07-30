// Copyright 2026 Dchromium_fork
//
// render_fingerprint: TEST-ONLY fixture generator (2026-07-26 revision).
//
// This module is a pure-function engine that derives per-domain
// sub-parameters from a single uint64_t "render seed". It exists
// strictly to generate deterministic fixture data for:
//
//   - Offline unit tests (render_fingerprint_unittest.cc).
//   - Golden / regression fixtures used to audit cross-profile device
//     entropy in CI.
//   - Internal tooling that needs reproducible per-profile RenderSeed
//     derivations.
//
// IT IS NOT WIRED INTO PRODUCTION BLINK / ANGLE / WEBGL / CANVAS2D.
// Production readback paths (getImageData / readPixels / toDataURL /
// toBlob) MUST remain byte-for-byte identical to upstream Chromium.
// See AGENTS.md / Chromium开发规则.md for the maintainer policy that
// "页面可见 Canvas/WebGL 输出不修改". The build system enforces this
// boundary: render_fingerprint lives in its own source_set with
// visibility restricted to the test target only. Any future attempt
// to include it from Blink / canvas_readback_noise / content will
// fail the GN visibility check.
//
// Boundary invariants (any violation = bug):
//   1. No CommandLine / VEM / RenderSeedStore / global state reads.
//   2. No LOG() / VLOG() / DLOG() / D副作用 (only pure return values).
//   3. No global mutable state.
//   4. Same input -> same output, byte-for-byte, across all platforms.
//   5. The input seed 0 is reserved for "disabled"; functions must handle
//      it without crashing, returning identity values.
//
// Output invariants:
//   - Canvas2DParams: all numeric fields within documented ranges; no
//     field depends on real hardware state.
//   - WebGLParams: extension order derived from a Fisher-Yates with a
//     seed-stable RNG; the order is deterministic given the seed but
//     different across seeds.
//   - UnmaskedMicroVariant: minor suffix append to the live UNMASKED
//     string; the live prefix is preserved (no hardcoded vendor/renderer
//     models); only safe glyphs are appended. Available for golden
//     fixture generation only.
//
// Why "UnmaskedMicroVariant": kept as a public API for the offline
// golden fixture generator, which produces strings of the form "ANGLE
// (Intel UHD 630) rev 42" deterministically per seed. Not used inside
// the renderer process at runtime; please do not add a new caller
// without explicit AGENTS.md policy review.

#ifndef SRC_CHROMIUM_FORK_RENDER_FINGERPRINT_H_
#define SRC_CHROMIUM_FORK_RENDER_FINGERPRINT_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace chromium_fork {

// Bumped when any derivation rule changes incompatibly. Independent of
// canvas_noise_engine's kCanvasNoiseAlgorithmVersion.
constexpr uint32_t kRenderFingerprintAlgorithmVersion = 1;

// Distinct sub-domains for salt derivation. Mirrors the structure of
// canvas_noise_engine's kCanvasNoiseDomainCanvas2D / kCanvasNoiseDomainWebGL
// but is a separate namespace because the two engines serve different
// purposes and should evolve independently.
enum : uint8_t {
  kRenderFingerprintDomainCanvas2D = 0,
  kRenderFingerprintDomainWebGL    = 1,
  kRenderFingerprintDomainUnmasked = 2,
};

// Canvas 2D test fixture parameters. These are intended ONLY for offline
// fixture generation (see file top comment); the engine never reads or
// writes any real SkSurface / SkImageInfo. Field ranges are kept narrow
// so a sloppy caller cannot drive a downstream SkSurface into undefined
// territory. NEVER wire these into production Blink.
struct Canvas2DRenderFingerprintParams {
  // SkSurfaceProps::Flags bits. OR-able into a uint32_t.
  // Native Chrome sets these to a stable value; we OR in 0..3 extra
  // hint bits derived from the seed. Hint bits only affect rendering
  // quality, never correctness.
  uint32_t surface_props_flags_hint = 0;

  // pixel geometry enum (matches SkPixelGeometry):
  //   0 = kUnknown_SkPixelGeometry (default)
  //   1 = kRGB_SkPixelGeometry
  //   2 = kBGR_SkPixelGeometry
  //   3 = kRGB_V_SkPixelGeometry
  //   4 = kBGR_V_SkPixelGeometry
  uint8_t surface_props_pixel_geometry = 0;

  // Color space baseline enum (kept narrow; wider range would risk
  // changing the canvas's perceived color profile, which is observable
  // by fingerprint tests). Values map to sk_sp<SkColorSpace> handles
  // constructed at the injection site.
  //   0 = sRGB (most common)
  //   1 = sRGB-ish (linear sRGB)
  //   2 = Display P3
  //   3 = Rec.2020
  uint8_t color_space_baseline = 0;

  // Device pixel ratio bias in [-0.01, +0.01]. Applied multiplicatively
  // to the host device pixel ratio. Range is intentionally tiny so that
  // layout pixel-snapping stays consistent (no off-by-one rounding).
  double device_pixel_ratio_bias = 0.0;

  // Buffer alignment offset (0..15 bytes). Applied at the start of the
  // pixel storage as a slack padding; downstream GL upload paths strip
  // it before reading. This affects the bytes layout but not the
  // visible image.
  uint8_t buffer_alignment_offset_bytes = 0;
};

// WebGL test fixture parameters. As with Canvas2D above, these are
// golden-fixture data only. NEVER wire into production WebGL / ANGLE.
struct WebGLRenderFingerprintParams {
  // GL_PACK_ALIGNMENT: one of {1, 2, 4, 8}. Native Chrome uses 4; we
  // may shift within legal values to vary the readPixels stride.
  uint8_t pack_alignment = 4;

  // GL_UNPACK_ALIGNMENT: same constraint as pack_alignment.
  uint8_t unpack_alignment = 4;

  // Offset applied to the reported MAX_TEXTURE_SIZE. The reported
  // value = actual - max_texture_size_offset. Range [-2, +2]; clamped
  // to keep reported >= native minimum. The intent is to mimic the
  // natural small variance seen across driver versions.
  int8_t max_texture_size_offset = 0;

  // Offset applied to MAX_VIEWPORT_DIMS by clamping the reported max
  // viewport to (real_max - offset). Range [0, 4] pixels per axis.
  uint8_t max_viewport_bias_pixels = 0;

  // Sub-pixel viewport origin offset in [-1, +1] pixels per axis.
  // Applied to the projection matrix at context creation; downstream
  // GL clips so the visible image is identical, but the GPU's internal
  // viewport state differs.
  int8_t subpixel_viewport_offset_x = 0;
  int8_t subpixel_viewport_offset_y = 0;

  // GL extension enumeration seed. Used at the end of
  // getSupportedExtensions() to apply a stable Fisher-Yates shuffle.
  // The engine returns a derived seed; the actual shuffle happens at
  // the injection site because it operates on a runtime data structure.
  uint64_t extension_shuffle_seed = 0;
};

// Computes Canvas2D parameters from |render_seed|. Pure function.
// |render_seed| == 0 returns identity (zeros / defaults); downstream
// injection sites use this as the "disabled" sentinel.
Canvas2DRenderFingerprintParams ComputeCanvas2DRenderFingerprint(
    uint64_t render_seed);

// Computes WebGL parameters from |render_seed|. Pure function.
WebGLRenderFingerprintParams ComputeWebGLRenderFingerprint(
    uint64_t render_seed);

// Derives a minor variant of |live_unmasked_renderer| (or vendor).
//
// Rules:
//   - If |render_seed| == 0, returns |live_unmasked_renderer| unchanged.
//   - Otherwise appends a small ASCII suffix of the form " (rev XX)" or
//     " (build XXXX)" where the digits are derived from the seed. The
//     live prefix is preserved verbatim (no rewriting of vendor strings,
//     no fabrication of GPU models).
//   - The variant length is bounded to ~24 chars to keep the resulting
//     string plausible. The variant is stable per seed.
//
// USAGE: golden fixture generation only. NOT wired into the VEM
// "profile.webgl.{vendor,renderer}" fallback path; offline tooling and
// unit tests use this API to build deterministic UNMASKED strings for
// snapshot regression. Do not add a new caller without first
// re-reading the AGENTS.md policy review.
std::string DeriveUnmaskedMicroVariant(std::string_view live_unmasked,
                                       uint64_t render_seed);

// Stable Fisher-Yates shuffle using a seed-derived RNG. Operates in
// place on |items|. Pure (RNG state is internal, deterministic per seed).
// Exposed as a free function so the offline golden fixture generator
// (under render_fingerprint_unittest.cc::ShuffleStableBySeedTest) can
// invoke it with a runtime Vector<String>.
//
// NOT used for security-sensitive randomness; the RNG is a simple split-
// mix64-based generator and is fully deterministic per seed.
void ShuffleStableBySeed(std::vector<std::string>* items, uint64_t seed);

// Returns true iff |seed| == 0 (i.e. the entire feature is disabled).
inline bool IsRenderFingerprintDisabled(uint64_t seed) {
  return seed == 0;
}

}  // namespace chromium_fork

#endif  // SRC_CHROMIUM_FORK_RENDER_FINGERPRINT_H_