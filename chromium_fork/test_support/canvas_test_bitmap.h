// Copyright 2026 Dchromium_fork

#ifndef SRC_CHROMIUM_FORK_TEST_SUPPORT_CANVAS_TEST_BITMAP_H_
#define SRC_CHROMIUM_FORK_TEST_SUPPORT_CANVAS_TEST_BITMAP_H_

#include <cstdint>

#include "include/core/SkBitmap.h"
#include "include/core/SkImage.h"
#include "include/core/SkRefCnt.h"

namespace chromium_fork::test_support {

// Test-only deep-copy of |image| into a writable bitmap that has had
// `ApplyNoiseToPixmap` applied. The destination format is forced to
// kN32 (RGBA8 / BGRA8) + kPremul, so this helper is the right choice
// whenever the test cares about the noise output but does not care
// about the input color/alpha type.
//
// Returns an empty SkBitmap if:
//   - image is null,
//   - tryAllocPixels() fails,
//   - peekPixels() / readPixels() fails,
//   - ApplyNoiseToPixmap() returns false (validation failure OR
//     zero pixels modified; in both cases we return empty so the
//     caller's failure contract is "no usable bitmap").
//
// When ApplyNoiseToPixmap() returns false due to validation failure,
// the input pixmap is guaranteed unmodified by the engine. We still
// return empty, so the caller sees the failure uniformly.
//
// Contract:
//   - The source SkImage's pixel data is read but never written.
//   - The returned bitmap owns an independent raster allocation; the
//     caller may freely modify it without side effects on the input.
SkBitmap CopyToWritableRgba8PremulBitmap(sk_sp<const SkImage> image,
                                         uint64_t session_seed,
                                         uint64_t per_canvas_salt,
                                         uint32_t algorithm_version);

// Test-only deep-copy that preserves the input image's color type and
// alpha type. Used to verify that the engine rejects formats other
// than RGBA8 / BGRA8 + Premul / Opaque. If the input is not in the
// accepted set, ApplyNoiseToPixmap() will fail validation and this
// helper returns empty.
//
// This is the right helper for the "RejectsRGBAF16" /
// "RejectsUnpremultiplied" tests: it surfaces the engine's own
// validation rather than silently coercing the format beforehand.
SkBitmap CopyToWritableBitmapPreservingInfo(sk_sp<const SkImage> image,
                                            uint64_t session_seed,
                                            uint64_t per_canvas_salt,
                                            uint32_t algorithm_version);

// Test-only deep-copy without noise. Used by the copy-correctness
// tests to verify that the helper preserves color space and alpha
// type, and does not alias the source.
//
// Returns empty on null image / allocation / readPixels failure.
SkBitmap CopyBitmapWithoutNoise(sk_sp<const SkImage> image);

}  // namespace chromium_fork::test_support

#endif  // SRC_CHROMIUM_FORK_TEST_SUPPORT_CANVAS_TEST_BITMAP_H_
