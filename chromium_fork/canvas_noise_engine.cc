// Copyright 2026 Dchromium_fork

#include "chromium_fork/canvas_noise_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace chromium_fork {
namespace {

// MurmurHash3 64-bit finalizer (xoshiro-like scramble, no dependency on
// the input length). Deterministic across platforms.
uint64_t MixHash(uint64_t value) {
  value ^= value >> 30;
  value *= UINT64_C(0xbf58476d1ce4e5b9);
  value ^= value >> 27;
  value *= UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

// FNV-1a over the raw bytes of a 64-bit value. Used for salt derivation
// and for coordinate hashing.
uint64_t FoldU64(uint64_t value) {
  uint64_t hash = UINT64_C(1469598103934665603);  // FNV offset basis
  for (int i = 0; i < 8; ++i) {
    uint8_t byte = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    hash ^= byte;
    hash *= UINT64_C(1099511628211);  // FNV prime
  }
  return hash;
}

// Pick a channel delta in [min, max] uniformly from the bits of |seed|.
int ChannelDelta(uint64_t seed, int min_val, int max_val) {
  if (min_val > max_val) {
    std::swap(min_val, max_val);
  }
  const uint32_t range = static_cast<uint32_t>(max_val - min_val + 1);
  if (range == 0) return min_val;
  return min_val + static_cast<int>(seed % range);
}

// Skip pixels that would visually break: transparent (alpha<16), pure
// black (R/G/B<=2), or pure white (R/G/B>=253). The guard prevents
// pixel-hash artifacts on solid backgrounds from being mistaken for
// "tampered" pixels by sampling sites.
bool IsProtectedPixel(const uint8_t* pixel, bool bgra) {
  const uint8_t alpha = pixel[3];
  if (alpha < 16) {
    return true;
  }
  const uint8_t r = bgra ? pixel[2] : pixel[0];
  const uint8_t g = pixel[1];
  const uint8_t b = bgra ? pixel[0] : pixel[2];
  const bool near_black = r <= 2 && g <= 2 && b <= 2;
  const bool near_white = r >= 253 && g >= 253 && b >= 253;
  return near_black || near_white;
}

// Apply per-channel deltas while preserving the premultiplied-alpha
// invariant `RGB <= alpha`. Each channel is clamped to [0, alpha] before
// being written back, so the post-condition holds even when the caller
// would otherwise push a component above its alpha (which would be a
// malformed premul pixel).
void ApplyDelta(uint8_t* pixel,
                bool bgra,
                int red_delta,
                int green_delta,
                int blue_delta) {
  const uint8_t alpha = pixel[3];

  uint8_t* red = bgra ? &pixel[2] : &pixel[0];
  uint8_t* green = &pixel[1];
  uint8_t* blue = bgra ? &pixel[0] : &pixel[2];

  const int r_new = std::clamp<int>(*red + red_delta, 0, alpha);
  const int g_new = std::clamp<int>(*green + green_delta, 0, alpha);
  const int b_new = std::clamp<int>(*blue + blue_delta, 0, alpha);

  *red = static_cast<uint8_t>(r_new);
  *green = static_cast<uint8_t>(g_new);
  *blue = static_cast<uint8_t>(b_new);
}

}  // namespace

bool IsCanvasNoiseApplyable(const SkPixmap& pixmap,
                            const CanvasNoiseParams& params) {
  // 1. width / height
  if (pixmap.width() <= 0 || pixmap.height() <= 0) {
    return false;
  }
  // 2. pixel address
  if (pixmap.addr() == nullptr) {
    return false;
  }
  // 3. rowBytes: must be non-zero and large enough to hold one row of
  //    4-byte pixels. Use a 64-bit intermediate to defend against
  //    width overflow.
  const size_t row_bytes = static_cast<size_t>(pixmap.rowBytes());
  if (row_bytes == 0) {
    return false;
  }
  const int64_t width = pixmap.width();
  if (static_cast<int64_t>(row_bytes) < width * 4) {
    return false;
  }
  // 4. color type: only 8-bit single-plane RGBA / BGRA
  const SkColorType ct = pixmap.colorType();
  if (ct != kRGBA_8888_SkColorType && ct != kBGRA_8888_SkColorType) {
    return false;
  }
  // 5. alpha type: Premul, Opaque, or Unpremul.
  //   Unpremul is converted to Premul in-place in ApplyNoiseToPixmap.
  const SkAlphaType at = pixmap.alphaType();
  if (at != kPremul_SkAlphaType && at != kOpaque_SkAlphaType &&
      at != kUnpremul_SkAlphaType) {
    return false;
  }
  // 6. parameter ranges
  if (params.sample_rate < 0.0 || params.sample_rate > 1.0) {
    return false;
  }
  if (params.max_pixels == 0) {
    return false;
  }
  if (params.delta_min < -255 || params.delta_max > 255 ||
      params.delta_min > params.delta_max) {
    return false;
  }
  if (params.max_total_delta <= 0 || params.max_total_delta > 765) {
    return false;
  }
  return true;
}

uint64_t DerivePerCanvasSalt(uint64_t session_seed,
                             uint64_t canvas_identity,
                             uint32_t algorithm_version,
                             uint8_t domain_id) {
  // Legacy v1 algorithm (3-argument form): seed -> identity -> version.
  // domain_id == kCanvasNoiseDomainCanvas2D (0) must reproduce this path
  // byte-for-byte so existing golden data stays valid.
  //
  // Non-zero domain_id (e.g. kCanvasNoiseDomainWebGL) is folded in to
  // produce a disjoint salt space. The fold order is significant and
  // changing it changes the salt even with identical inputs, which is
  // what we want for algorithm versioning.
  uint64_t mix = FoldU64(session_seed);
  mix = MixHash(mix ^ FoldU64(canvas_identity));
  mix = MixHash(mix ^ FoldU64(static_cast<uint64_t>(algorithm_version)));
  if (domain_id != kCanvasNoiseDomainCanvas2D) {
    mix = MixHash(mix ^ FoldU64(static_cast<uint64_t>(domain_id)));
  }
  return mix;
}

bool ApplyNoiseToPixmap(SkPixmap pixmap,
                        uint64_t session_seed,
                        uint64_t per_canvas_salt,
                        uint32_t algorithm_version,
                        const CanvasNoiseParams& params) {
  if (!IsCanvasNoiseApplyable(pixmap, params)) {
    return false;
  }

  const SkColorType ct = pixmap.colorType();
  const bool bgra = (ct == kBGRA_8888_SkColorType);

  // Handle Unpremul: convert in-place to Premul before noise.
  // Standard: R_premul = R_unpremul * A / 255 (rounded).
  const SkAlphaType at = pixmap.alphaType();
  if (at == kUnpremul_SkAlphaType) {
    const int un_w = pixmap.width();
    const int un_h = pixmap.height();
    for (int y = 0; y < un_h; ++y) {
      uint8_t* row = static_cast<uint8_t*>(pixmap.writable_addr(0, y));
      for (int x = 0; x < un_w; ++x) {
        uint8_t* pixel = row + (x * 4);
        uint8_t r = pixel[bgra ? 2 : 0];
        uint8_t g = pixel[1];
        uint8_t b = pixel[bgra ? 0 : 2];
        uint8_t a = pixel[3];
        if (a == 255) {
          // Opaque: R_premul = R (identity)
        } else if (a == 0) {
          // Fully transparent: R_premul = 0
          if (!bgra) { pixel[0] = 0; }
          pixel[1] = 0;
          if (bgra) { pixel[2] = 0; }
        } else {
          // Standard: R_premul = R_unpremul * A / 255
          if (!bgra) { pixel[0] = static_cast<uint8_t>((r * a + 127) / 255); }
          pixel[1] = static_cast<uint8_t>((g * a + 127) / 255);
          if (bgra) { pixel[2] = static_cast<uint8_t>((b * a + 127) / 255); }
        }
      }
    }
    // Note: pixmap alphaType is still Unpremul in the SkPixmap wrapper,
    // but the actual pixel data is now Premul. We operate on raw bytes
    // directly in the noise loop, so it works correctly.
  }

  constexpr uint64_t kSampleBuckets = 1000000;
  const uint64_t sample_threshold =
      static_cast<uint64_t>(params.sample_rate * kSampleBuckets);

  const int width = pixmap.width();
  const int height = pixmap.height();

  // Pre-fold canvas-level entropy once so the inner loop only does one
  // MixHash per pixel coordinate (the rest is XORed into a running value).
  const uint64_t canvas_seed = MixHash(
      per_canvas_salt ^
      FoldU64(static_cast<uint64_t>(width) << 16 ^
              static_cast<uint64_t>(height)) ^
      FoldU64(static_cast<uint64_t>(algorithm_version) << 32));

  uint32_t modified = 0;
  for (int y = 0; y < height && modified < params.max_pixels; ++y) {
    uint8_t* row = static_cast<uint8_t*>(pixmap.writable_addr(0, y));
    for (int x = 0; x < width && modified < params.max_pixels; ++x) {
      const uint64_t coord_seed = MixHash(
          canvas_seed ^
          FoldU64(static_cast<uint64_t>(x)) ^
          FoldU64(static_cast<uint64_t>(y)));

      if (coord_seed % kSampleBuckets >= sample_threshold) {
        continue;
      }

      uint8_t* pixel = row + (x * 4);
      if (IsProtectedPixel(pixel, bgra)) {
        continue;
      }

      int red_delta = ChannelDelta(MixHash(coord_seed ^ 0x01),
                                   params.delta_min, params.delta_max);
      int green_delta = ChannelDelta(MixHash(coord_seed ^ 0x02),
                                     params.delta_min, params.delta_max);
      int blue_delta = ChannelDelta(MixHash(coord_seed ^ 0x03),
                                    params.delta_min, params.delta_max);

      // Cap the per-pixel absolute sum so a single pixel cannot drift
      // dramatically (e.g. gray->saturated red).
      while (std::abs(red_delta) + std::abs(green_delta) +
             std::abs(blue_delta) > params.max_total_delta) {
        if (blue_delta != 0) {
          blue_delta = 0;
        } else if (green_delta != 0) {
          green_delta = 0;
        } else {
          red_delta = 0;
        }
      }

      if (red_delta == 0 && green_delta == 0 && blue_delta == 0) {
        continue;
      }

      // ApplyDelta enforces RGB <= alpha internally, so a fully opaque
      // pixel (alpha=255) keeps full headroom while a partially transparent
      // pixel is auto-clamped to its premul ceiling.
      ApplyDelta(pixel, bgra, red_delta, green_delta, blue_delta);
      ++modified;
    }
  }
  return modified != 0;
}

}  // namespace chromium_fork
