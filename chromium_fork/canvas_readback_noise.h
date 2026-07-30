// Copyright 2026 Dchromium_fork

#ifndef SRC_CHROMIUM_FORK_CANVAS_READBACK_NOISE_H_
#define SRC_CHROMIUM_FORK_CANVAS_READBACK_NOISE_H_

#include <cstdint>
#include <string_view>

#include "include/core/SkPixmap.h"

namespace chromium_fork {

// Applies the explicitly configured, test-only perturbation to an RGBA8/BGRA8
// readback. The function is a no-op unless VEM is initialized, the test flag is
// enabled, and |origin| is in the exact configured allowlist.
//
// Compile-time gate: when BUILDFLAG(ENABLE_CANVAS_TEST_NOISE)=0 the entire
// body is dead-stripped and the function returns false. This is the only
// behaviour difference between forked and Official Builds.
bool ApplyCanvasReadbackNoise(SkPixmap pixmap, std::string_view origin);

// New entry point (Phase P4 / P0-3, 2026-07-25): callers supply the
// PerCanvasSalt explicitly so that the same Canvas object reuses the salt
// across all of its readback entry points (getImageData / toDataURL / toBlob
// / OffscreenCanvas.convertToBlob). The caller is responsible for caching
// the salt (e.g. via BaseRenderingContext2D::GetOrComputePerCanvasSalt) so
// the same origin+identity always yields the same pixel hash within a
// session. The salt must be derived via DerivePerCanvasSalt.
bool ApplyCanvasReadbackNoiseWithSalt(SkPixmap pixmap,
                                      std::string_view origin,
                                      uint64_t per_canvas_salt);

// Legacy entry point that derives identity from the origin string. Used
// when the caller cannot provide a stable CanvasObjectIdentity. New code
// should prefer ApplyCanvasReadbackNoiseWithSalt.
bool ApplyCanvasReadbackNoiseWithIdentity(SkPixmap pixmap,
                                          std::string_view origin,
                                          uint64_t canvas_identity);

// Cheap gate check that callers can use to skip the readPixels + bitmap
// allocation entirely when the noise path is not going to run.
bool ShouldApplyCanvasReadbackNoise(std::string_view origin);

}  // namespace chromium_fork

#endif  // SRC_CHROMIUM_FORK_CANVAS_READBACK_NOISE_H_
