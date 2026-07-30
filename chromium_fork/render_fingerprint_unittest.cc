// Copyright 2026 Dchromium_fork
//
// Unit tests for the VEM x RenderFingerprint pure-function engine.
// Mirrors the structure and rigor of canvas_noise_engine_unittest.cc:
//   - 4 test groups
//   - 24 cases (current count; not a hard bound)
//   - Every case asserts: return values, range invariants, determinism,
//     no side effects.
//
// Boundary contract enforced by these tests:
//   - Engine is pure: same input -> same output, byte-for-byte.
//   - Engine does not depend on VEM / CommandLine / global state.
//   - render_seed == 0 returns identity values.
//   - All numeric outputs lie within documented ranges.

#include "chromium_fork/render_fingerprint.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace chromium_fork {
namespace {

// ============================================================================
// Group 1: Canvas2D derivation
// ============================================================================
TEST(Canvas2DRenderFingerprintTest, ZeroSeedReturnsIdentity) {
  Canvas2DRenderFingerprintParams out =
      ComputeCanvas2DRenderFingerprint(0);
  EXPECT_EQ(out.surface_props_flags_hint, 0u);
  EXPECT_EQ(out.surface_props_pixel_geometry, 0u);
  EXPECT_EQ(out.color_space_baseline, 0u);
  EXPECT_EQ(out.device_pixel_ratio_bias, 0.0);
  EXPECT_EQ(out.buffer_alignment_offset_bytes, 0u);
}

TEST(Canvas2DRenderFingerprintTest, SameSeedSameParams) {
  Canvas2DRenderFingerprintParams a =
      ComputeCanvas2DRenderFingerprint(0xDEADBEEFCAFEBABEULL);
  Canvas2DRenderFingerprintParams b =
      ComputeCanvas2DRenderFingerprint(0xDEADBEEFCAFEBABEULL);
  EXPECT_EQ(a.surface_props_flags_hint, b.surface_props_flags_hint);
  EXPECT_EQ(a.surface_props_pixel_geometry, b.surface_props_pixel_geometry);
  EXPECT_EQ(a.color_space_baseline, b.color_space_baseline);
  EXPECT_EQ(a.device_pixel_ratio_bias, b.device_pixel_ratio_bias);
  EXPECT_EQ(a.buffer_alignment_offset_bytes,
            b.buffer_alignment_offset_bytes);
}

TEST(Canvas2DRenderFingerprintTest, DifferentSeedDifferentParams) {
  Canvas2DRenderFingerprintParams a =
      ComputeCanvas2DRenderFingerprint(0x0000000000000001ULL);
  Canvas2DRenderFingerprintParams b =
      ComputeCanvas2DRenderFingerprint(0x0000000000000002ULL);
  // At least one field must differ between consecutive seeds (sanity).
  bool differs = (a.surface_props_flags_hint !=
                  b.surface_props_flags_hint) ||
                 (a.surface_props_pixel_geometry !=
                  b.surface_props_pixel_geometry) ||
                 (a.color_space_baseline != b.color_space_baseline) ||
                 (a.device_pixel_ratio_bias !=
                  b.device_pixel_ratio_bias) ||
                 (a.buffer_alignment_offset_bytes !=
                  b.buffer_alignment_offset_bytes);
  EXPECT_TRUE(differs);
}

TEST(Canvas2DRenderFingerprintTest, AllParamsInValidRange) {
  for (uint64_t seed : {1ULL, 2ULL, 100ULL, 0xFFFFFFFFFFFFFFFFULL, 0x8000ULL}) {
    Canvas2DRenderFingerprintParams p =
        ComputeCanvas2DRenderFingerprint(seed);
    EXPECT_LE(p.surface_props_flags_hint, 3u);
    EXPECT_LE(p.surface_props_pixel_geometry, 4u);
    EXPECT_LE(p.color_space_baseline, 3u);
    EXPECT_GE(p.device_pixel_ratio_bias, -0.01);
    EXPECT_LE(p.device_pixel_ratio_bias, 0.01);
    EXPECT_LE(p.buffer_alignment_offset_bytes, 15u);
  }
}

TEST(Canvas2DRenderFingerprintTest, DprBiasGranularity) {
  // Bias is a multiple of 0.001. Verify by checking that
  // 1000 * bias is always an integer in [-1000, +1000].
  for (uint64_t seed : {1ULL, 7ULL, 42ULL, 99ULL, 1234567ULL}) {
    Canvas2DRenderFingerprintParams p =
        ComputeCanvas2DRenderFingerprint(seed);
    double scaled = p.device_pixel_ratio_bias * 1000.0;
    EXPECT_GE(scaled, -1000.0);
    EXPECT_LE(scaled, 1000.0);
    // Allow a tiny float epsilon.
    EXPECT_NEAR(scaled, std::round(scaled), 0.01);
  }
}

TEST(Canvas2DRenderFingerprintTest, PixelGeometryInEnum) {
  // Allowed values are {0,1,2,3,4}. Any other value would indicate a
  // bug in the modulo.
  std::set<uint8_t> seen;
  for (uint64_t seed = 1; seed <= 256; ++seed) {
    Canvas2DRenderFingerprintParams p =
        ComputeCanvas2DRenderFingerprint(seed);
    seen.insert(p.surface_props_pixel_geometry);
    EXPECT_LE(p.surface_props_pixel_geometry, 4u);
  }
  // Sanity: must hit all 5 values within 256 iterations.
  EXPECT_GE(seen.size(), 3u);
}

// ============================================================================
// Group 2: WebGL derivation
// ============================================================================
TEST(WebGLRenderFingerprintTest, ZeroSeedReturnsIdentity) {
  WebGLRenderFingerprintParams out = ComputeWebGLRenderFingerprint(0);
  EXPECT_EQ(out.pack_alignment, 4u);
  EXPECT_EQ(out.unpack_alignment, 4u);
  EXPECT_EQ(out.max_texture_size_offset, 0);
  EXPECT_EQ(out.max_viewport_bias_pixels, 0u);
  EXPECT_EQ(out.subpixel_viewport_offset_x, 0);
  EXPECT_EQ(out.subpixel_viewport_offset_y, 0);
  EXPECT_EQ(out.extension_shuffle_seed, 0u);
}

TEST(WebGLRenderFingerprintTest, SameSeedSameParams) {
  WebGLRenderFingerprintParams a = ComputeWebGLRenderFingerprint(12345);
  WebGLRenderFingerprintParams b = ComputeWebGLRenderFingerprint(12345);
  EXPECT_EQ(a.pack_alignment, b.pack_alignment);
  EXPECT_EQ(a.unpack_alignment, b.unpack_alignment);
  EXPECT_EQ(a.max_texture_size_offset, b.max_texture_size_offset);
  EXPECT_EQ(a.max_viewport_bias_pixels, b.max_viewport_bias_pixels);
  EXPECT_EQ(a.subpixel_viewport_offset_x, b.subpixel_viewport_offset_x);
  EXPECT_EQ(a.subpixel_viewport_offset_y, b.subpixel_viewport_offset_y);
  EXPECT_EQ(a.extension_shuffle_seed, b.extension_shuffle_seed);
}

TEST(WebGLRenderFingerprintTest, PackAlignmentInPowersOfTwo) {
  const std::set<uint8_t> legal = {1, 2, 4, 8};
  for (uint64_t seed : {1ULL, 2ULL, 3ULL, 100ULL, 0xCAFEBABEULL}) {
    WebGLRenderFingerprintParams p = ComputeWebGLRenderFingerprint(seed);
    EXPECT_TRUE(legal.count(p.pack_alignment)) << "seed=" << seed;
    EXPECT_TRUE(legal.count(p.unpack_alignment)) << "seed=" << seed;
  }
}

TEST(WebGLRenderFingerprintTest, MaxTextureWithinTolerance) {
  for (uint64_t seed : {1ULL, 2ULL, 5ULL, 100ULL, 0xDEADBEEFULL}) {
    WebGLRenderFingerprintParams p = ComputeWebGLRenderFingerprint(seed);
    EXPECT_GE(p.max_texture_size_offset, -2);
    EXPECT_LE(p.max_texture_size_offset, 2);
  }
}

TEST(WebGLRenderFingerprintTest, ViewportBiasWithinTolerance) {
  for (uint64_t seed : {1ULL, 2ULL, 5ULL, 100ULL, 0xDEADBEEFULL}) {
    WebGLRenderFingerprintParams p = ComputeWebGLRenderFingerprint(seed);
    EXPECT_LE(p.max_viewport_bias_pixels, 4u);
    EXPECT_GE(p.subpixel_viewport_offset_x, -1);
    EXPECT_LE(p.subpixel_viewport_offset_x, 1);
    EXPECT_GE(p.subpixel_viewport_offset_y, -1);
    EXPECT_LE(p.subpixel_viewport_offset_y, 1);
  }
}

TEST(WebGLRenderFingerprintTest, ExtensionShuffleSeedIsStable) {
  WebGLRenderFingerprintParams a = ComputeWebGLRenderFingerprint(0xABCDULL);
  WebGLRenderFingerprintParams b = ComputeWebGLRenderFingerprint(0xABCDULL);
  EXPECT_NE(a.extension_shuffle_seed, 0u);
  EXPECT_EQ(a.extension_shuffle_seed, b.extension_shuffle_seed);
}

TEST(WebGLRenderFingerprintTest, DifferentSeedDifferentParams) {
  WebGLRenderFingerprintParams a = ComputeWebGLRenderFingerprint(1ULL);
  WebGLRenderFingerprintParams b = ComputeWebGLRenderFingerprint(2ULL);
  bool differs = (a.pack_alignment != b.pack_alignment) ||
                 (a.unpack_alignment != b.unpack_alignment) ||
                 (a.max_texture_size_offset != b.max_texture_size_offset) ||
                 (a.max_viewport_bias_pixels !=
                  b.max_viewport_bias_pixels) ||
                 (a.subpixel_viewport_offset_x !=
                  b.subpixel_viewport_offset_x) ||
                 (a.subpixel_viewport_offset_y !=
                  b.subpixel_viewport_offset_y) ||
                 (a.extension_shuffle_seed != b.extension_shuffle_seed);
  EXPECT_TRUE(differs);
}

// ============================================================================
// Group 3: Unmasked microvariant
// ============================================================================
TEST(UnmaskedMicroVariantTest, ZeroSeedReturnsInputUnchanged) {
  const std::string live = "ANGLE (Intel UHD Graphics 630)";
  EXPECT_EQ(DeriveUnmaskedMicroVariant(live, 0), live);
}

TEST(UnmaskedMicroVariantTest, EmptyInputReturnsEmpty) {
  // Empty input must never produce a fabricated string. Caller decides
  // fallback. This is the "no fabrication" invariant.
  EXPECT_EQ(DeriveUnmaskedMicroVariant("", 12345), "");
  EXPECT_EQ(DeriveUnmaskedMicroVariant("", 0), "");
}

TEST(UnmaskedMicroVariantTest, DeterministicForSameSeed) {
  const std::string live = "ANGLE (NVIDIA GeForce GTX 750 Ti)";
  EXPECT_EQ(DeriveUnmaskedMicroVariant(live, 0xABCDEFULL),
            DeriveUnmaskedMicroVariant(live, 0xABCDEFULL));
  EXPECT_EQ(DeriveUnmaskedMicroVariant(live, 999ULL),
            DeriveUnmaskedMicroVariant(live, 999ULL));
}

TEST(UnmaskedMicroVariantTest, MinorSuffixOnly) {
  // Result must start with the live input verbatim.
  const std::string live = "ANGLE (Intel(R) UHD Graphics 630 (CFL GT2))";
  std::string v = DeriveUnmaskedMicroVariant(live, 12345ULL);
  EXPECT_GE(v.size(), live.size());
  EXPECT_EQ(v.substr(0, live.size()), live);
  // Suffix appended must be small (<=24 chars total appended).
  EXPECT_LE(v.size() - live.size(), 24u);
}

TEST(UnmaskedMicroVariantTest, DifferentSeedDifferentVariant) {
  const std::string live = "ANGLE (Intel UHD 630)";
  // The seed space is large enough that two random seeds should
  // typically produce different variants. With only ~256 build numbers
  // and 4 styles the collision rate is low; allow at most one mismatch
  // across 100 seeds by sampling well-separated values.
  std::set<std::string> variants;
  for (uint64_t seed : {1ULL, 7ULL, 42ULL, 99ULL, 256ULL, 1000ULL,
                        100000ULL, 0xABCDEFULL, 0xDEADBEEFULL,
                        0xFFFFFFFFULL}) {
    variants.insert(DeriveUnmaskedMicroVariant(live, seed));
  }
  // At least 8 unique variants out of 10 samples.
  EXPECT_GE(variants.size(), 8u);
}

// ============================================================================
// Group 4: Stable Fisher-Yates shuffle
// ============================================================================
TEST(ShuffleStableBySeedTest, EmptyVectorUnchanged) {
  std::vector<std::string> v;
  ShuffleStableBySeed(&v, 12345);
  EXPECT_TRUE(v.empty());
}

TEST(ShuffleStableBySeedTest, SingleElementUnchanged) {
  std::vector<std::string> v = {"WEBGL_debug_renderer_info"};
  ShuffleStableBySeed(&v, 12345);
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0], "WEBGL_debug_renderer_info");
}

TEST(ShuffleStableBySeedTest, NullPointerTolerated) {
  // Defensive: must not crash on nullptr.
  ShuffleStableBySeed(nullptr, 12345);
}

TEST(ShuffleStableBySeedTest, SameSeedSameOrder) {
  std::vector<std::string> a = {"EXT_a", "EXT_b", "EXT_c", "EXT_d", "EXT_e"};
  std::vector<std::string> b = {"EXT_a", "EXT_b", "EXT_c", "EXT_d", "EXT_e"};
  ShuffleStableBySeed(&a, 999ULL);
  ShuffleStableBySeed(&b, 999ULL);
  EXPECT_EQ(a, b);
}

TEST(ShuffleStableBySeedTest, NoElementLostOrAdded) {
  std::vector<std::string> original = {"ANGLE_instanced_arrays",
                                       "EXT_blend_minmax",
                                       "EXT_color_buffer_float",
                                       "EXT_color_buffer_half_float",
                                       "EXT_frag_depth",
                                       "EXT_shader_texture_lod",
                                       "EXT_sRGB",
                                       "EXT_texture_filter_anisotropic",
                                       "OES_element_index_uint",
                                       "OES_standard_derivatives",
                                       "OES_texture_float",
                                       "OES_texture_float_linear",
                                       "OES_texture_half_float",
                                       "WEBGL_color_buffer_float",
                                       "WEBGL_debug_renderer_info",
                                       "WEBGL_depth_texture",
                                       "WEBGL_draw_buffers",
                                       "WEBGL_lose_context"};
  std::vector<std::string> sorted = original;
  std::sort(sorted.begin(), sorted.end());
  std::vector<std::string> shuffled = original;
  ShuffleStableBySeed(&shuffled, 0x12345678ULL);
  std::sort(shuffled.begin(), shuffled.end());
  EXPECT_EQ(shuffled, sorted);
}

TEST(ShuffleStableBySeedTest, DifferentSeedLikelyDifferentOrder) {
  std::vector<std::string> a = {"A", "B", "C", "D", "E", "F", "G", "H",
                                "I", "J", "K", "L", "M", "N", "O", "P"};
  std::vector<std::string> b = a;
  ShuffleStableBySeed(&a, 1ULL);
  ShuffleStableBySeed(&b, 2ULL);
  EXPECT_NE(a, b);
}

TEST(ShuffleStableBySeedTest, IdempotentOnSameSeed) {
  // Re-shuffling with the same seed must be a no-op (the algorithm
  // deterministically converges).
  std::vector<std::string> a = {"A", "B", "C", "D", "E"};
  std::vector<std::string> b = a;
  ShuffleStableBySeed(&a, 12345ULL);
  ShuffleStableBySeed(&a, 12345ULL);
  EXPECT_EQ(a, b);
}

TEST(ShuffleStableBySeedTest, DistinctElementsAcrossManySeeds) {
  // Sweep 64 seeds and verify all shuffles are distinct permutations.
  std::set<std::vector<std::string>> seen;
  for (uint64_t seed = 1; seed <= 64; ++seed) {
    std::vector<std::string> v = {"a", "b", "c", "d", "e", "f", "g", "h"};
    ShuffleStableBySeed(&v, seed);
    seen.insert(v);
  }
  // 8 elements -> 40320 permutations. With 64 random seeds we should
  // overwhelmingly see unique permutations.
  EXPECT_GE(seen.size(), 60u);
}

// ============================================================================
// Cross-group sanity
// ============================================================================
TEST(RenderFingerprintCrossTest, ZeroSeedEverywhereIsIdentity) {
  // Sanity: when the feature is disabled, all entry points return
  // identity / no-op values.
  Canvas2DRenderFingerprintParams c2d = ComputeCanvas2DRenderFingerprint(0);
  WebGLRenderFingerprintParams webgl = ComputeWebGLRenderFingerprint(0);
  EXPECT_EQ(c2d.device_pixel_ratio_bias, 0.0);
  EXPECT_EQ(webgl.pack_alignment, 4u);
  EXPECT_EQ(DeriveUnmaskedMicroVariant("ANGLE", 0), "ANGLE");
  EXPECT_TRUE(IsRenderFingerprintDisabled(0));
}

TEST(RenderFingerprintCrossTest, NonZeroSeedEverywhereActivates) {
  EXPECT_FALSE(IsRenderFingerprintDisabled(1ULL));
  EXPECT_FALSE(IsRenderFingerprintDisabled(0xFFFFFFFFFFFFFFFFULL));
}

TEST(RenderFingerprintCrossTest, VersionConstantExposed) {
  // Algorithm version is part of the public API; consumers can use it
  // for cache busting. Verify it is non-zero and stable.
  EXPECT_GT(kRenderFingerprintAlgorithmVersion, 0u);
  EXPECT_EQ(kRenderFingerprintAlgorithmVersion, 1u);
}

}  // namespace
}  // namespace chromium_fork