// Copyright 2026 Dchromium_fork
//
// Maintenance policy (2026-07-25 22:XX — engine-only maintenance boundary):
//
// This header exposes the pure-function noise engine used by the
// standalone test target (`chromium_fork_noise_engine_unittests`) and by
// the optional `test_support/canvas_test_bitmap.{h,cc}` helper. It is
// **deliberately not linked into the Blink production rendering path**.
//
// Contract:
//   - All randomness is derived from the caller-supplied
//     (session_seed, canvas_identity, algorithm_version, domain_id).
//   - The engine never reads CommandLine, VEM, Blink, or any global state.
//   - The engine never logs. Failure paths return false without touching
//     the input pixmap.
//   - Validation is strict: any unsupported color type, alpha type,
//     out-of-range parameter, or non-premultiplied pixel triggers an
//     early return.
//
// Build-system gate (NOT a #if at this header): when
// `enable_canvas_test_noise = false` is set in `src/chromium_fork/BUILD.gn`
// (the default, and the only value accepted in Official Builds) the .cc
// file for this engine is NOT compiled and the functions below are not
// emitted into any binary. There is no "defined but no-op" fallback
// inside the .cc - the symbols simply do not exist when the feature is
// disabled.

#ifndef SRC_CHROMIUM_FORK_CANVAS_NOISE_ENGINE_H_
#define SRC_CHROMIUM_FORK_CANVAS_NOISE_ENGINE_H_

#include <cstdint>

#include "include/core/SkPixmap.h"

namespace chromium_fork {

// Algorithm version baked into the noise mix. Bumped when the hash chain
// or sampling formula changes - increasing this guarantees that the same
// session/canvas/salt tuple yields a different pixel hash, which is what
// makes algorithm-version drift observable in CI.
constexpr uint32_t kCanvasNoiseAlgorithmVersion = 1;

// Domain identifiers used to keep salt spaces disjoint across the canvas
// readback families that may eventually consume this engine. Only values
// defined here are legal; unknown domain IDs will be rejected by the
// caller-side salt derivation. `kCanvasNoiseDomainCanvas2D` is the
// legacy value and must remain zero so historical golden data stays
// reproducible.
enum : uint8_t {
  kCanvasNoiseDomainCanvas2D = 0,
  kCanvasNoiseDomainWebGL = 1,
};

// Tunable parameters, all sourced by the caller (test fixture).
// Validation rules (apply before calling ApplyNoiseToPixmap):
//   - sample_rate       in [0.0, 1.0]
//   - max_pixels        in [1, width*height]
//   - delta_min <= delta_max, both in [-255, 255]
//   - max_total_delta   in (0, 765]  (R+G+B sum)
struct CanvasNoiseParams {
  // Defaults chosen so that the engine operates in the 0.5-1.5 per-mille
  // perturbation band on standard 1080x1920 RGBA canvases (P3 / 2026-07-25):
  //   sample_rate = 0.001 (~0.1% candidate pixels)
  //   max_pixels  = 2500   (uncapped against typical canvas area)
  //   delta       = ±1     (1/255 ~= 3.92 per-mille per channel)
  //   max_total_delta = 1  (R+G+B <= 1/765 ~= 1.31 per-mille per pixel)
  // Aggregate effective perturbation: 2500/2.07M * 1.31 per-mille ~= 1.58 per-mille.
  double sample_rate = 0.001;
  uint32_t max_pixels = 2500;
  int delta_min = -1;
  int delta_max = 1;
  int max_total_delta = 1;
};

// Derive the per-Canvas salt that mixes the session seed with a
// caller-supplied identity token (typically the address of the
// CanvasRenderingContextHost - HTMLCanvasElement or OffscreenCanvas).
// Algorithm-version and domain_id are folded in so that a bump of either
// produces a different salt even with identical session_seed and identity.
//
// The output is stable for a fixed
// (session_seed, identity, algorithm_version, domain_id) tuple.
// This is a pure function; no process state is consulted.
//
// `domain_id` defaults to `kCanvasNoiseDomainCanvas2D` for legacy
// compatibility: existing golden data computed with the three-argument
// form must continue to reproduce byte-for-byte.
uint64_t DerivePerCanvasSalt(uint64_t session_seed,
                             uint64_t canvas_identity,
                             uint32_t algorithm_version,
                             uint8_t domain_id = kCanvasNoiseDomainCanvas2D);

// Convenience validator used by callers to gate ApplyNoiseToPixmap.
// Strict checks performed (in order):
//   1. width / height > 0
//   2. pixel address is non-null
//   3. rowBytes > 0
//   4. color type is RGBA8 or BGRA8
//   5. alpha type is Premul or Opaque
//   6. sample_rate, delta_min/max, max_total_delta, max_pixels are in range
bool IsCanvasNoiseApplyable(const SkPixmap& pixmap,
                            const CanvasNoiseParams& params);

// Apply deterministic RGB channel perturbations to |pixmap|.
//
// Pure function: depends only on the explicit arguments. Will not touch
// the alpha channel, near-transparent, near-black, near-white, or any
// pixel whose RGB component is already at the alpha ceiling. The
// invariant `RGB <= alpha` is preserved at every modified pixel.
//
// Returns true if at least one pixel was modified, false otherwise
// (including when validation fails). On a `false` return the input
// pixmap is guaranteed to be unmodified.
bool ApplyNoiseToPixmap(SkPixmap pixmap,
                        uint64_t session_seed,
                        uint64_t per_canvas_salt,
                        uint32_t algorithm_version,
                        const CanvasNoiseParams& params);

}  // namespace chromium_fork

#endif  // SRC_CHROMIUM_FORK_CANVAS_NOISE_ENGINE_H_
