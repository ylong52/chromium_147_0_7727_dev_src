// Copyright 2026 Dchromium_fork
//
// Phase W2 (2026-07-27): WebGL readback noise production implementation.
//
// Reuses the existing canvas_noise_engine for the actual noise application,
// but uses a different domain_id (kCanvasNoiseDomainWebGL) to ensure
// pixel-level isolation from Canvas 2D noise.
//
// Seed derivation chain:
//   SessionSeed (CanvasSessionSeedManager)
//     → PerCanvasSalt (session_seed, canvas_identity, version, domain=WebGL)
//       → ApplyNoiseToPixmap
//
// Runtime gate (2026-07-27 production spec):
//   - The compile-time gate is ENABLE_CANVAS_TEST_NOISE; when it is 0 the
//     .cc body is dead-stripped and WebGL readback output matches native
//     Chrome byte-for-byte (Official Build policy).
//   - At runtime the production WebGL gate is the commandline switch
//     --webgl-noise-enabled=1. When absent / set to 0 the readback hook
//     in ReadPixelsHelper() short-circuits and the WebGL output matches
//     native Chrome byte-for-byte.
//   - Origin allowlist is intentionally NOT consulted by the WebGL path:
//     once the runtime gate is enabled, every WebGL readPixels() call
//     in the current process is noise-transformed.

#include "chromium_fork/webgl_readback_noise.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

#include "chromium_fork/canvas_noise_engine.h"
#include "chromium_fork/canvas_readback_noise.h"
#include "chromium_fork/canvas_session_seed_manager.h"
#include "chromium_fork/chromium_fork_buildflags.h"
#include "chromium_fork/switches.h"
#include "chromium_fork/virtual_environment_manager.h"

#include "base/command_line.h"
#include "base/hash/hash.h"

namespace chromium_fork {
namespace {

// Derive per-canvas salt for WebGL domain.
// Combines session_seed with canvas_identity and WebGL domain tag.
uint64_t DeriveWebGLPerCanvasSalt(uint64_t session_seed,
                                  uint64_t canvas_identity,
                                  uint32_t algorithm_version) {
  return DerivePerCanvasSalt(session_seed, canvas_identity, algorithm_version,
                             kCanvasNoiseDomainWebGL);
}

// Load noise parameters from VEM (VirtualEnvironmentManager).
// Returns default safe params if VEM is not available.
CanvasNoiseParams LoadParamsFromVem() {
  CanvasNoiseParams params;
  const VirtualEnvironmentManager* vem = GetVirtualEnvironmentManager();
  if (!vem || !vem->is_initialized()) {
    return params;
  }

  // VEM provides canvas test noise parameters; use them for WebGL as well.
  params.sample_rate =
      std::clamp(vem->canvas_test_noise_sample_rate(), 0.0, 1.0);
  params.max_pixels = vem->canvas_test_noise_max_pixels();
  params.delta_min = vem->canvas_test_noise_delta_min();
  params.delta_max = vem->canvas_test_noise_delta_max();
  params.max_total_delta = vem->canvas_test_noise_max_total_delta();

  // Reject degenerate configs with safe defaults.
  if (params.sample_rate <= 0.0 || params.max_pixels == 0 ||
      params.delta_min > params.delta_max ||
      params.delta_min < -255 || params.delta_max > 255 ||
      params.max_total_delta <= 0 || params.max_total_delta > 765) {
    return CanvasNoiseParams{};
  }

  return params;
}

// Fast hash for canvas identity when origin string is the only available
// identity source. Uses base::PersistentHash which is cross-platform stable
// and deterministic (unlike std::hash which may vary between STL versions).
// Mirrors the pattern in farble_seed.cc::GetAntiFraudSeed64.
uint64_t HashOriginForCanvasIdentity(const std::string_view origin) {
  const uint64_t raw = base::PersistentHash(origin);
  // Apply MurmurHash3 finalizer to extend to 64-bit with good distribution.
  // Same constants as canvas_noise_engine.cc::MixHash.
  uint64_t value = raw;
  value ^= value >> 30;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27;
  value *= 0x94d049bb133111ebULL;
  value ^= value >> 31;
  return value;
}

// Runtime gate resolver (2026-07-27 production spec).
//
// Strict flag-on detection. Returns:
//   -1 if --webgl-noise-enabled is present and explicitly set to
//       "0" / "false" (force off)
//   +1 if --webgl-noise-enabled is present and explicitly set to
//       "1" / "true" (force on)
//    0 if switch is absent.
// We treat any other value as "absent" (operator typo -> no-op, not
// silent failure). Production default is 0 (matches native Chrome).
int ReadWebGLNoiseRuntimeGate() {
  const base::CommandLine& cmdline = *base::CommandLine::ForCurrentProcess();
  if (!cmdline.HasSwitch(switches::kForkWebGLNoiseEnabled)) {
    return 0;
  }
  const std::string v =
      cmdline.GetSwitchValueASCII(switches::kForkWebGLNoiseEnabled);
  if (v.empty() || v == "1" || v == "true") {
    return 1;
  }
  if (v == "0" || v == "false") {
    return -1;
  }
  return 0;
}

}  // namespace

bool ApplyWebGLReadbackNoise(SkPixmap pixmap,
                             std::string_view origin,
                             uint64_t canvas_id) {
#if BUILDFLAG(ENABLE_CANVAS_TEST_NOISE)
  // Runtime gate first. Production default is OFF so that WebGL output
  // matches native Chrome byte-for-byte when the operator has not opted
  // in via --webgl-noise-enabled.
  if (ReadWebGLNoiseRuntimeGate() != 1) {
    return false;
  }

  // Session-seed readiness check (independent of the Canvas 2D allowlist).
  // The seed has been resolved by the Browser main process and propagated
  // to children via --canvas-anti-fraud-seed; reaching this branch in a
  // child process means the seed has been inherited correctly.
  auto* mgr = CanvasTestSessionSeedManager::GetInstance();
  if (!mgr->IsSeedAvailable()) {
    return false;
  }

  // Derive per-canvas salt with WebGL domain tag.
  const CanvasTestConfig cfg = mgr->GetConfig();
  const uint64_t per_canvas_salt =
      DeriveWebGLPerCanvasSalt(cfg.session_seed, canvas_id,
                               cfg.algorithm_version);

  // Load VEM parameters for noise application.
  const CanvasNoiseParams params = LoadParamsFromVem();

  // Apply noise using the shared engine.
  (void)origin;  // origin retained in the API for diagnostic / future use.
  return ApplyNoiseToPixmap(pixmap, cfg.session_seed, per_canvas_salt,
                            cfg.algorithm_version, params);
#else
  // Official Build: gate closed. WebGL readPixels matches native Chrome
  // byte-for-byte. No work, no allocation, no logging.
  (void)pixmap;
  (void)origin;
  (void)canvas_id;
  return false;
#endif  // BUILDFLAG(ENABLE_CANVAS_TEST_NOISE)
}

bool ApplyWebGLReadbackNoiseSimple(SkPixmap pixmap,
                                  std::string_view origin) {
#if BUILDFLAG(ENABLE_CANVAS_TEST_NOISE)
  const uint64_t canvas_id = HashOriginForCanvasIdentity(origin);
  return ApplyWebGLReadbackNoise(pixmap, origin, canvas_id);
#else
  (void)pixmap;
  (void)origin;
  return false;
#endif  // BUILDFLAG(ENABLE_CANVAS_TEST_NOISE)
}

bool ShouldApplyWebGLReadbackNoise(std::string_view origin) {
#if BUILDFLAG(ENABLE_CANVAS_TEST_NOISE)
  (void)origin;  // origin not part of the production gate.
  if (ReadWebGLNoiseRuntimeGate() != 1) {
    return false;
  }
  auto* mgr = CanvasTestSessionSeedManager::GetInstance();
  return mgr->IsSeedAvailable();
#else
  (void)origin;
  return false;
#endif  // BUILDFLAG(ENABLE_CANVAS_TEST_NOISE)
}

}  // namespace chromium_fork
