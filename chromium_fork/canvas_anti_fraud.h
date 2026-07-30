// Copyright 2026 Dchromium_fork
//
// Canvas anti-fraud algorithm (2026-07-26, audit-revision 2026-07-26.b).
//
// Purpose:
//   Apply deterministic, sparse, identity-stable LSB perturbations to RGBA8 /
//   BGRA8 / Gray8 canvas readback pixels. The contract is:
//
//     * Same (effective_seed, canvas_identity, dst_addr) -> IDENTICAL pixel
//       hash across repeated calls (intra-process stability + cross-readback
//       idempotence).
//     * Different effective_seed -> different pixel hash (cross-process
//       variation requirement).
//     * Premultiplied-alpha layouts are clamped so post-perturbation channels
//       still satisfy R <= A, G <= A, B <= A (no illegal premul pixels).
//
// Algorithm:
//   1. State = effective_seed XOR PerCanvasSalt(src_x, src_y, w, h, info).
//      We DO NOT fold in pixel content (audit-finding #8): the first call
//      mutates pixels, the second call must mutate the SAME bits back to
//      zero so the pixel hash is stable across readbacks.
//   2. xorshift64 PRNG (3-round Marsaglia, period 2^64-1).
//   3. ~0.17% of pixels get LSB toggled on R/G/B (alpha untouched for
//      RGBA/BGRA; Gray8 / RGB565 use byte-level XOR with safe semantics).
//
// The 0.17% rate and the "RGB LSB only, alpha untouched" property are tuned
// to look like distributed JPEG quantization noise rather than a fixed-
// pattern perturbation. This is the anti-fraud rationale: detection sites
// that fingerprint "edge-of-image fixed-pattern noise" lose their signal
// here.
//
// Boundary compliance (Chromium开发规则.md §⚠️ 关键约束):
//   - Pure function: depends only on the explicit arguments.
//   - Does NOT log, read CommandLine, VEM, Blink, or any global state.
//   - All integer arithmetic is bounds-checked; out-of-bounds requests
//     return without modifying the input.
//
// Audit 2026-07-26.b corrections (vs initial 2026-07-26 draft):
//   - #4 premul-alpha protection: XOR result is clamped to <= alpha byte.
//   - #6 bounds-check: width/height/rowBytes/x*bpp/y*rowBytes validated.
//   - #7 unique-coordinate: rejection-sampling pool without duplicates.
//   - #8 idempotence: dropped pixel content from PRNG seed.
//   - #23 BUILDFLAG gate: noise body is dead-stripped in Official Build;
//     ShouldApplyMeasureTextNoise returns false unconditionally.

#ifndef SRC_CHROMIUM_FORK_CANVAS_ANTI_FRAUD_H_
#define SRC_CHROMIUM_FORK_CANVAS_ANTI_FRAUD_H_

#include <cstdint>
#include <string_view>

#include "include/core/SkImageInfo.h"

namespace chromium_fork {

// Apply sparse, identity-stable RGB LSB perturbations to the pixel buffer
// pointed to by |addr|. See file-level comment for the contract.
//
// Parameters:
//   addr           : Pixel data base address (already offset by src_x/src_y
//                    if applicable). May be nullptr (function is a no-op).
//   info           : SkImageInfo describing the buffer layout. Only
//                    kRGBA_8888_SkColorType, kBGRA_8888_SkColorType,
//                    kRGB_565_SkColorType, and kGray_8_SkColorType are
//                    supported; others trigger an early return.
//   src_x, src_y   : Sub-rectangle origin within the image. Must be >= 0
//                    and within info.{width(),height()}; out-of-range
//                    values trigger an early return without modification.
//   effective_seed : Per-(process, canvas) seed; see farble_seed.h.
//
// Returns: void. On an unsupported layout / invalid bounds / small image
// (<8x8), the input is left untouched (no side effect).
void ApplyCanvasAntiFraudNoise(const void* addr,
                                const SkImageInfo& info,
                                int src_x,
                                int src_y,
                                uint64_t effective_seed);

// Derive a measureText noise multiplier in (-0.5, 0.5]. The text-length
// and effective_seed both fold into a stable xorshift64 output that the
// caller multiplies by the metric dimension. This produces a per-text
// perturbation that is stable across readbacks within one process but
// varies across processes when the seed changes.
//
// Returns: a double in the open interval (-0.5, 0.5]. The boundary is
// open so ShuffleMetrics(1.0 + noise_x) never returns 0 or 2.
double DeriveMeasureTextNoiseX(uint64_t effective_seed,
                                std::string_view text);

// Global measureText noise gate (2026-07-26, audit-revised).
//
// Returns true iff:
//   * --canvas-anti-fraud-seed is present on the command line (any value),
//     OR
//   * the CanvasTestSessionSeedManager reports IsEnabled() == true AND
//     a non-zero seed has been resolved.
//
// Crucially, this gate does NOT consult the per-origin allowlist; the
// user explicitly opted in via the CLI, so the noise applies to every
// origin whose measureText is invoked. getImageData / toDataURL remain
// under allowlist control.
//
// Thread safety: callers must invoke from the main thread.
bool ShouldApplyMeasureTextNoise();

}  // namespace chromium_fork

#endif  // SRC_CHROMIUM_FORK_CANVAS_ANTI_FRAUD_H_