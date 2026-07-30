// Copyright 2026 Dchromium_fork
//
// Phase P4 / P5 / P8 (2026-07-25): unified Canvas readback noise entry point.
//
// Replaces the original metadata-only implementation (2026-07-24) with one
// that is fully wired to the CanvasSessionSeedManager:
//
//   SessionSeed        -> canvas_session_seed_manager.GetConfig().session_seed
//   PerCanvasSalt      -> DerivePerCanvasSalt(session_seed, identity, version)
//                          where identity = caller's CanvasObjectIdentity
//   Allowed origins    -> exact match against url::Origin string
//   Algorithm version  -> from CanvasTestConfig; affects hash output
//
// Official Build policy: when ENABLE_CANVAS_TEST_NOISE=0 the entire body
// is dead-stripped and the entry returns false unconditionally. The
// callers' `if (applied)` blocks then skip the noisy-bitmap creation, so
// there is zero runtime cost and zero image_bitmap churn in Official
// Builds.

#include "chromium_fork/canvas_readback_noise.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

#include "chromium_fork/canvas_anti_fraud.h"
#include "chromium_fork/canvas_noise_engine.h"
#include "chromium_fork/canvas_readback_noise.h"
#include "chromium_fork/switches.h"
#include "chromium_fork/canvas_session_seed_manager.h"
#include "chromium_fork/chromium_fork_buildflags.h"
#include "chromium_fork/farble_seed.h"
#include "chromium_fork/switches.h"
#include "chromium_fork/virtual_environment_manager.h"

#include "chromium_fork/switches.h"

namespace chromium_fork {
namespace {

// Dual-mode dispatcher (2026-07-26):
//   case-id "engine_v2" / "engine_060" / "1" -> ApplyCanvasAntiFraudNoise
//   case-id "engine_v1" / "0" / absent        -> ApplyNoiseToPixmap (legacy)
//
// Two different algorithms share the same SessionSeedManager::session_seed
// but use disjoint (case-id-tagged) effective seeds so they cannot collide
// at the pixel hash level even if both are configured in the same session.
enum class NoiseCaseId : uint8_t {
  kEngineV1Legacy = 0,
  kEngineV2AntiFraud = 1,
};

NoiseCaseId ReadCaseIdFromCommandLine() {
  const base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  const std::string v =
      cmd->GetSwitchValueASCII(switches::kForkCanvasTestNoiseCaseId);
  if (v == "engine_v2" || v == "engine_060" || v == "1") {
    return NoiseCaseId::kEngineV2AntiFraud;
  }
  return NoiseCaseId::kEngineV1Legacy;
}

// Fold the case-id into the upper byte of the effective seed so the two
// algorithms produce disjoint pixel hashes for the same canvas content.
uint64_t PackSeedWithCaseId(uint64_t base_seed, NoiseCaseId case_id) {
  return (static_cast<uint64_t>(case_id) << 56) ^ base_seed;
}

// Default parameters when none are supplied by VEM. Conservative:
// 1% pixel sample rate, 1000 max-modified-pixels, ±2 channel delta with
// 6 total abs-sum cap. Tunable via VEM canvas_test_noise_* fields.
CanvasNoiseParams LoadParamsFromVem() {
  CanvasNoiseParams params;
  const VirtualEnvironmentManager* vem = GetVirtualEnvironmentManager();
  if (!vem || !vem->is_initialized()) {
    return params;
  }
  params.sample_rate =
      std::clamp(vem->canvas_test_noise_sample_rate(), 0.0, 1.0);
  params.max_pixels = vem->canvas_test_noise_max_pixels();
  params.delta_min = vem->canvas_test_noise_delta_min();
  params.delta_max = vem->canvas_test_noise_delta_max();
  params.max_total_delta = vem->canvas_test_noise_max_total_delta();

  // Reject degenerate configs. Defaults stay safe.
  if (params.sample_rate <= 0.0 || params.max_pixels == 0 ||
      params.delta_min > params.delta_max ||
      params.delta_min < -255 || params.delta_max > 255 ||
      params.max_total_delta <= 0 || params.max_total_delta > 765) {
    return CanvasNoiseParams{};
  }
  return params;
}

// PerCanvasSalt derivation shared by the legacy and the new entry points.
uint64_t SaltForOrigin(const std::string& origin, uint64_t session_seed,
                       uint32_t algorithm_version) {
  const uint64_t identity =
      static_cast<uint64_t>(std::hash<std::string>{}(origin));
  return DerivePerCanvasSalt(session_seed, identity, algorithm_version);
}

}  // namespace

bool ApplyCanvasReadbackNoise(SkPixmap pixmap, std::string_view origin) {
#if BUILDFLAG(ENABLE_CANVAS_TEST_NOISE)
  const std::string origin_str(origin);

  auto* mgr = CanvasTestSessionSeedManager::GetInstance();
  if (!mgr->IsEnabled() || !mgr->IsOriginAllowed(origin_str)) {
    return false;
  }

  const CanvasTestConfig cfg = mgr->GetConfig();
  const NoiseCaseId case_id = ReadCaseIdFromCommandLine();

  if (case_id == NoiseCaseId::kEngineV2AntiFraud) {
    // New anti-fraud path (2026-07-26): sparse content-aware LSB.
    // effective_seed is derived from (session_seed XOR origin); the
    // algorithm internally folds in a content-hash for stability.
    const uint64_t effective_seed =
        PackSeedWithCaseId(GetAntiFraudSeed64(origin_str), case_id);
    ApplyCanvasAntiFraudNoise(pixmap.writable_addr(), pixmap.info(), 0, 0,
                              effective_seed);
    return true;
  }

  // Legacy path (default): conservative ApplyNoiseToPixmap with the
  // existing engine parameters. Per-canvas salt is derived from
  // std::hash(origin) for cross-callsite identity stability.
  const uint64_t per_canvas_salt =
      SaltForOrigin(origin_str, cfg.session_seed, cfg.algorithm_version);

  const CanvasNoiseParams params = LoadParamsFromVem();
  return ApplyNoiseToPixmap(pixmap, cfg.session_seed, per_canvas_salt,
                            cfg.algorithm_version, params);
#else
  // Official Build: gate closed. The map pixel hash MUST match native
  // Chrome byte-for-byte. No work, no allocation, no logging.
  return false;
#endif  // BUILDFLAG(ENABLE_CANVAS_TEST_NOISE)
}

bool ApplyCanvasReadbackNoiseWithIdentity(SkPixmap pixmap,
                                          std::string_view origin,
                                          uint64_t canvas_identity) {
#if BUILDFLAG(ENABLE_CANVAS_TEST_NOISE)
  const std::string origin_str(origin);

  auto* mgr = CanvasTestSessionSeedManager::GetInstance();
  if (!mgr->IsEnabled() || !mgr->IsOriginAllowed(origin_str)) {
    return false;
  }

  const CanvasTestConfig cfg = mgr->GetConfig();
  const uint64_t per_canvas_salt = DerivePerCanvasSalt(
      cfg.session_seed, canvas_identity, cfg.algorithm_version);
  return ApplyCanvasReadbackNoiseWithSalt(pixmap, origin_str, per_canvas_salt);
#else
  return false;
#endif  // BUILDFLAG(ENABLE_CANVAS_TEST_NOISE)
}

bool ApplyCanvasReadbackNoiseWithSalt(SkPixmap pixmap,
                                      std::string_view origin,
                                      uint64_t per_canvas_salt) {
#if BUILDFLAG(ENABLE_CANVAS_TEST_NOISE)
  const std::string origin_str(origin);

  auto* mgr = CanvasTestSessionSeedManager::GetInstance();
  if (!mgr->IsEnabled() || !mgr->IsOriginAllowed(origin_str)) {
    return false;
  }

  const CanvasTestConfig cfg = mgr->GetConfig();
  const CanvasNoiseParams params = LoadParamsFromVem();
  return ApplyNoiseToPixmap(pixmap, cfg.session_seed, per_canvas_salt,
                            cfg.algorithm_version, params);
#else
  return false;
#endif  // BUILDFLAG(ENABLE_CANVAS_TEST_NOISE)
}

bool ShouldApplyCanvasReadbackNoise(std::string_view origin) {
#if BUILDFLAG(ENABLE_CANVAS_TEST_NOISE)
  const std::string origin_str(origin);
  auto* mgr = CanvasTestSessionSeedManager::GetInstance();
  return mgr->IsEnabled() && mgr->IsOriginAllowed(origin_str);
#else
  return false;
#endif  // BUILDFLAG(ENABLE_CANVAS_TEST_NOISE)
}

}  // namespace chromium_fork
