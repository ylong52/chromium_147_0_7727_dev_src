// Copyright 2026 Dchromium_fork

#include "chromium_fork/test_support/canvas_test_bitmap.h"

#include "chromium_fork/canvas_noise_engine.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"

namespace chromium_fork::test_support {
namespace {

// Default CanvasNoiseParams used by the helpers. Mirrors the engine
// defaults except for max_pixels which is raised so that 4K fixtures
// aren't truncated by the per-pixel cap during fuzz-style tests.
CanvasNoiseParams DefaultTestParams() {
  CanvasNoiseParams params;
  params.sample_rate = 1.0;
  params.max_pixels = UINT32_MAX;
  params.delta_min = -2;
  params.delta_max = 2;
  params.max_total_delta = 6;
  return params;
}

// Allocate a writable bitmap of the requested size. Returns empty
// SkBitmap on allocation failure. Color space propagates from `info`
// so downstream tests can verify the helper preserves it.
SkBitmap AllocateBitmap(const SkImageInfo& info) {
  SkBitmap bitmap;
  if (!bitmap.tryAllocPixels(info)) {
    return SkBitmap();
  }
  return bitmap;
}

// Shared readback path: copy pixels from `image` into `dst_pixmap`.
// Returns true on success, false on null image / peekPixels / readPixels
// failure. The caller is responsible for allocating the destination.
bool ReadImageInto(const SkImage* image, const SkPixmap& dst_pixmap) {
  if (!image) {
    return false;
  }
  // peekPixels() cannot fail for a writable SkBitmap's pixmap, but we
  // still check defensively.
  if (!dst_pixmap.addr()) {
    return false;
  }
  return image->readPixels(dst_pixmap, 0, 0);
}

}  // namespace

SkBitmap CopyToWritableRgba8PremulBitmap(sk_sp<const SkImage> image,
                                         uint64_t session_seed,
                                         uint64_t per_canvas_salt,
                                         uint32_t algorithm_version) {
  if (!image) {
    return SkBitmap();
  }

  // Force the destination into RGBA8 + Premul. Skia preserves color
  // space across the format change. This is the only path that
  // silently ignores the input color/alpha type - callers that want
  // strict format preservation must use the *PreservingInfo variant.
  const SkImageInfo dst_info = image->imageInfo()
                                   .makeColorType(kN32_SkColorType)
                                   .makeAlphaType(kPremul_SkAlphaType);

  SkBitmap dst = AllocateBitmap(dst_info);
  if (dst.empty()) {
    return SkBitmap();
  }

  SkPixmap dst_pixmap;
  if (!dst.peekPixels(&dst_pixmap)) {
    return SkBitmap();
  }
  if (!ReadImageInto(image.get(), dst_pixmap)) {
    return SkBitmap();
  }

  // ApplyNoiseToPixmap's return value is a tuple of (validation ok,
  // any_modified). On false: either validation failed (and pixmap is
  // guaranteed unmodified) OR no pixels were modified. We treat both
  // as failure for the helper contract: "no usable bitmap".
  const CanvasNoiseParams params = DefaultTestParams();
  const bool ok = ApplyNoiseToPixmap(dst_pixmap, session_seed, per_canvas_salt,
                                     algorithm_version, params);
  if (!ok) {
    return SkBitmap();
  }
  return dst;
}

SkBitmap CopyToWritableBitmapPreservingInfo(sk_sp<const SkImage> image,
                                            uint64_t session_seed,
                                            uint64_t per_canvas_salt,
                                            uint32_t algorithm_version) {
  if (!image) {
    return SkBitmap();
  }

  // Preserve the source image's color type and alpha type. If the
  // resulting pixmap is not RGBA8/BGRA8 + Premul/Opaque, the engine
  // will reject it and we return empty.
  const SkImageInfo dst_info = image->imageInfo();

  SkBitmap dst = AllocateBitmap(dst_info);
  if (dst.empty()) {
    return SkBitmap();
  }

  SkPixmap dst_pixmap;
  if (!dst.peekPixels(&dst_pixmap)) {
    return SkBitmap();
  }
  if (!ReadImageInto(image.get(), dst_pixmap)) {
    return SkBitmap();
  }

  const CanvasNoiseParams params = DefaultTestParams();
  const bool ok = ApplyNoiseToPixmap(dst_pixmap, session_seed, per_canvas_salt,
                                     algorithm_version, params);
  if (!ok) {
    return SkBitmap();
  }
  return dst;
}

SkBitmap CopyBitmapWithoutNoise(sk_sp<const SkImage> image) {
  if (!image) {
    return SkBitmap();
  }
  const SkImageInfo dst_info = image->imageInfo()
                                   .makeColorType(kN32_SkColorType)
                                   .makeAlphaType(kPremul_SkAlphaType);

  SkBitmap dst = AllocateBitmap(dst_info);
  if (dst.empty()) {
    return SkBitmap();
  }

  SkPixmap dst_pixmap;
  if (!dst.peekPixels(&dst_pixmap)) {
    return SkBitmap();
  }
  if (!ReadImageInto(image.get(), dst_pixmap)) {
    return SkBitmap();
  }
  return dst;
}

}  // namespace chromium_fork::test_support
