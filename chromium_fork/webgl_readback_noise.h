// Copyright 2026 Dchromium_fork
//
// Phase W2 (2026-07-27): WebGL readback noise entry point.
//
// This module provides deterministic pixel-level noise injection for WebGL
// readPixels() calls, complementing the existing Canvas 2D noise path.
//
// Architecture (per Docs/5.多个维度的对抗加强设计.md §2B):
//
//   GPU Framebuffer → DXGI/OpenGL Readback → Noise Transform → JS TypedArray
//
// Key design constraints (per §5 Anti-Fraud Design):
//   1. Zero JS injection: pure C++ Blink Native Binding modification
//   2. Bitwise hash only: no I/O, strings, or trig functions in hot path
//   3. Performance budget: < 0.1ms for typical readPixels (1920x1080)
//   4. Edge case handling: isContextLost(), null buffer, overflow protection
//   5. Single source of truth: shared noise algorithm via canvas_noise_engine.h
//
// Seed derivation:
//   - SessionSeed from CanvasSessionSeedManager
//   - PerCanvasSalt derived from (session_seed, canvas_identity, domain_id=WebGL)
//   - domain_id = kCanvasNoiseDomainWebGL (distinct from Canvas2D)
//
// Runtime gate (2026-07-27 production spec):
//   - Compile-time: enabled only when ENABLE_CANVAS_TEST_NOISE=1
//     (otherwise the entire .cc body is dead-stripped; matches Official
//     Build where WebGL output MUST equal native Chrome byte-for-byte).
//   - Runtime: enabled only when --webgl-noise-enabled=1 is present on
//     the command line. When absent (the production default) the hook
//     returns false and WebGL readPixels output matches native Chrome.
//   - Origin allowlist: intentionally NOT consulted. Once the runtime
//     gate is enabled, every WebGL readPixels() call in the current
//     process is noise-transformed. The CanvasTestSessionSeedManager
//     retains the allowlist for the Canvas 2D path, which is unaffected.
//
// Build policy: when ENABLE_CANVAS_TEST_NOISE=0 the entire body is dead-
// stripped. No runtime cost and no behaviour difference in Official Builds.

#ifndef SRC_CHROMIUM_FORK_WEBGL_READBACK_NOISE_H_
#define SRC_CHROMIUM_FORK_WEBGL_READBACK_NOISE_H_

#include <cstdint>
#include <string_view>

#include "include/core/SkPixmap.h"

namespace chromium_fork {

// Apply deterministic noise to WebGL readPixels output.
//
// @param pixmap      SkPixmap wrapping the destination RGBA8/BGRA8 buffer
// @param origin      eTLD+1 of the page that initiated the readPixels
//                    (passed in for diagnostic and to keep the API shape
//                    stable with the existing hook site; the production
//                    WebGL gate does NOT filter on origin)
// @param canvas_id   Stable identity for this WebGL canvas (e.g. object address)
//
// Returns true if noise was applied, false otherwise (compile-time gate
// closed, runtime gate off, session-seed not yet available in this
// process, or engine validation failure). When false, the pixmap is
// guaranteed unmodified.
//
// Performance: O(width * height) with pure bitwise operations. < 0.1ms for
// typical 1920x1080 RGBA readback on modern hardware.
bool ApplyWebGLReadbackNoise(SkPixmap pixmap,
                             std::string_view origin,
                             uint64_t canvas_id);

// Convenience overload that derives canvas identity from origin only.
// Use this when a stable per-canvas identity is not available.
bool ApplyWebGLReadbackNoiseSimple(SkPixmap pixmap,
                                   std::string_view origin);

// Cheap gate check that callers can use to skip readPixels allocation
// entirely when the WebGL noise path is not going to run.
//
// Returns true only when:
//   - the compile-time gate (ENABLE_CANVAS_TEST_NOISE) is open,
//   - the runtime gate (--webgl-noise-enabled=1) is open, and
//   - a session_seed is available in the current process via
//     CanvasTestSessionSeedManager.
//
// Origin argument is retained for API symmetry but is intentionally
// NOT consulted by the production WebGL gate.
bool ShouldApplyWebGLReadbackNoise(std::string_view origin);

}  // namespace chromium_fork

#endif  // SRC_CHROMIUM_FORK_WEBGL_READBACK_NOISE_H_
