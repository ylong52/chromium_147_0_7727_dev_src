// Copyright 2026 Dchromium_fork

#include "chromium_fork/render_fingerprint.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace chromium_fork {

namespace {

// SplitMix64-style hash: deterministic 64-bit mixing function. Used to
// derive per-domain sub-seeds from the single RenderSeed. Pure.
inline uint64_t Mix(uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

// Domain-folded Mix: produces a per-domain sub-seed. Mirrors the
// canvas_noise_engine's DerivePerCanvasSalt but in a separate namespace
// (independent evolution).
uint64_t MixForDomain(uint64_t render_seed, uint8_t domain_id) {
  return Mix(Mix(render_seed) ^ static_cast<uint64_t>(domain_id));
}

// Tiny stateful generator for ShuffleStableBySeed. Deterministic per
// seed; not for security use.
class SplitMix64Rng {
 public:
  explicit SplitMix64Rng(uint64_t seed) : state_(seed | 1ULL) {}
  uint64_t Next() {
    state_ += 0x9E3779B97F4A7C15ULL;
    uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  uint32_t NextU32() { return static_cast<uint32_t>(Next() >> 32); }

 private:
  uint64_t state_;
};

// Clamp helper that mirrors std::clamp semantics for older toolchains.
template <typename T>
inline T ClampTo(T v, T lo, T hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Modulo that handles negative values correctly (Python-style).
inline int32_t ModPositive(int64_t v, int32_t m) {
  int64_t r = v % m;
  if (r < 0) r += m;
  return static_cast<int32_t>(r);
}

}  // namespace

// ============================================================================
// Canvas 2D derivation
// ============================================================================
Canvas2DRenderFingerprintParams ComputeCanvas2DRenderFingerprint(
    uint64_t render_seed) {
  Canvas2DRenderFingerprintParams out;
  if (render_seed == 0) {
    return out;  // All zeros == identity.
  }

  const uint64_t s = MixForDomain(render_seed, kRenderFingerprintDomainCanvas2D);
  SplitMix64Rng rng(s);

  // SkSurfaceProps::Flags bits we may set (lower 4 bits are the AA hint
  // family; setting any of them is harmless to output correctness).
  // Range: 0..3. Maps to SkSurfaceProps::kAAHintMask or similar.
  out.surface_props_flags_hint = rng.NextU32() & 0x3u;

  // Pixel geometry: 0..4 inclusive (matches SkPixelGeometry enum range).
  out.surface_props_pixel_geometry =
      static_cast<uint8_t>(rng.NextU32() % 5u);

  // Color space baseline: 0..3 inclusive. 4-7 are reserved for future
  // expansion; clamped to keep the enum width stable.
  out.color_space_baseline = static_cast<uint8_t>(rng.NextU32() % 4u);

  // device_pixel_ratio_bias in [-0.01, +0.01]. Step 1/1000.
  // Generate an integer in [-1000, +1000] and divide by 1000.0.
  const int32_t bias_int =
      static_cast<int32_t>(rng.NextU32() % 2001u) - 1000;
  out.device_pixel_ratio_bias = static_cast<double>(bias_int) / 1000.0;

  // Buffer alignment: 0..15 inclusive.
  out.buffer_alignment_offset_bytes =
      static_cast<uint8_t>(rng.NextU32() % 16u);

  return out;
}

// ============================================================================
// WebGL derivation
// ============================================================================
WebGLRenderFingerprintParams ComputeWebGLRenderFingerprint(
    uint64_t render_seed) {
  WebGLRenderFingerprintParams out;
  if (render_seed == 0) {
    return out;  // All identity (4 / 4 / 0 / 0 / 0 / 0 / 0 / 0).
  }

  const uint64_t s = MixForDomain(render_seed, kRenderFingerprintDomainWebGL);
  SplitMix64Rng rng(s);

  // pack_alignment: pick from {1, 2, 4, 8}. Use mod 4 then map.
  // 0->1, 1->2, 2->4, 3->8.
  const uint8_t pack_choice = static_cast<uint8_t>(rng.NextU32() & 0x3u);
  static constexpr uint8_t kPackTable[4] = {1, 2, 4, 8};
  out.pack_alignment = kPackTable[pack_choice];

  const uint8_t unpack_choice = static_cast<uint8_t>(rng.NextU32() & 0x3u);
  out.unpack_alignment = kPackTable[unpack_choice];

  // max_texture_size_offset: -2..+2 inclusive (signed).
  const int32_t mt_int = static_cast<int32_t>(rng.NextU32() % 5u) - 2;
  out.max_texture_size_offset = static_cast<int8_t>(mt_int);

  // max_viewport_bias_pixels: 0..4 inclusive.
  out.max_viewport_bias_pixels = static_cast<uint8_t>(rng.NextU32() % 5u);

  // subpixel viewport offset: -1..+1 inclusive.
  out.subpixel_viewport_offset_x =
      static_cast<int8_t>((rng.NextU32() % 3u) - 1);
  out.subpixel_viewport_offset_y =
      static_cast<int8_t>((rng.NextU32() % 3u) - 1);

  // extension_shuffle_seed: a fresh Mix of the domain seed.
  out.extension_shuffle_seed = Mix(s);

  return out;
}

// ============================================================================
// Unmasked microvariant
// ============================================================================
namespace {

// Append a small variant suffix to the live string. Variant has the
// form " <sep><tag> <digits>" where <sep> in {' ', '-'} and <tag> in
// {"rev", "build"}. The total appended length is bounded to <=24 chars.
//
// Examples (deterministic per seed):
//   "ANGLE (Intel UHD 630)" + seed -> "ANGLE (Intel UHD 630) rev 42"
//   "ANGLE (Intel UHD 630)" + seed -> "ANGLE (Intel UHD 630) build 17"
//
// Returns the input string unchanged if |render_seed| == 0.
std::string AppendVariant(std::string_view live, uint64_t render_seed) {
  if (render_seed == 0) {
    return std::string(live);
  }
  const uint64_t s =
      MixForDomain(render_seed, kRenderFingerprintDomainUnmasked);
  SplitMix64Rng rng(s);

  const uint32_t style = rng.NextU32() & 0x3u;
  // Cycle through 4 styles for visual variety while keeping the tag set
  // small (so the variant doesn't look "obviously generated").
  // 0: " rev N"
  // 1: " build N"
  // 2: " rN"
  // 3: "-rev.N"
  std::string suffix;
  switch (style) {
    case 0: {
      const uint32_t n = rng.NextU32() % 64u;
      suffix = " rev ";
      suffix += std::to_string(n);
      break;
    }
    case 1: {
      const uint32_t n = rng.NextU32() % 256u;
      suffix = " build ";
      suffix += std::to_string(n);
      break;
    }
    case 2: {
      const uint32_t n = rng.NextU32() % 32u;
      suffix = " r";
      suffix += std::to_string(n);
      break;
    }
    case 3: {
      const uint32_t n = rng.NextU32() % 32u;
      suffix = "-rev.";
      suffix += std::to_string(n);
      break;
    }
  }

  // Hard cap: keep variant plausible.
  if (suffix.size() > 24) {
    suffix.resize(24);
  }

  std::string result;
  result.reserve(live.size() + suffix.size());
  result.append(live);
  result.append(suffix);
  return result;
}

}  // namespace

std::string DeriveUnmaskedMicroVariant(std::string_view live_unmasked,
                                       uint64_t render_seed) {
  // Empty input -> empty output (no fabrication). Caller decides whether
  // to use the variant or fall through to GL_RENDERER / GL_VENDOR.
  if (live_unmasked.empty()) {
    return std::string();
  }
  return AppendVariant(live_unmasked, render_seed);
}

// ============================================================================
// Stable Fisher-Yates shuffle
// ============================================================================
void ShuffleStableBySeed(std::vector<std::string>* items, uint64_t seed) {
  if (!items || items->size() < 2) return;
  SplitMix64Rng rng(seed | 1ULL);
  const size_t n = items->size();
  for (size_t i = n - 1; i > 0; --i) {
    const uint64_t r = rng.Next();
    // Deterministic mod-permuted shuffle. NOTE: r % limit is NOT true
    // unbiased rejection sampling; we accept the small modulo bias here
    // because (a) this is for test fixtures, not security randomness,
    // and (b) per-seed determinism is the only contract that matters.
    // If/when this is ever used for anything with a statistical
    // distribution requirement, replace with rejection sampling.
    const uint64_t limit = (static_cast<uint64_t>(i) + 1);
    const uint64_t j = r % limit;
    if (j != i) {
      std::swap((*items)[i], (*items)[j]);
    }
  }
}

}  // namespace chromium_fork