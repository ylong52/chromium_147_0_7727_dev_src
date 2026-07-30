// Copyright 2026 Dchromium_fork

#include "chromium_fork/switches.h"
#include "chromium_fork/virtual_environment_manager.h"

#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/process/process_handle.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"

#include "chromium_fork/canvas_anti_fraud_seed_store.h"
#include "chromium_fork/canvas_session_seed_manager.h"

namespace chromium_fork {

using namespace switches;

// Voices debug helper (2026-07-27). Defined later in this file.
void ForkVoicesDbg(const std::string& msg);

// =====================================================================
// Free functions
// =====================================================================

bool GetForkSwitch(const base::CommandLine& cmdline,
                   const char* switch_name,
                   std::string* out) {
  if (!cmdline.HasSwitch(switch_name)) return false;
  *out = cmdline.GetSwitchValueASCII(switch_name);
  return true;
}

// Retired VEM render-seed/WebGL-variant derivation.
// Canvas test noise uses CanvasTestSessionSeedManager instead; this helper
// remains disabled as a migration marker and is not part of the build path.
#if 0
// DeriveWebGLVariantId: deterministic uint32 from seed + capability_profile.
//
// Algorithm (per Docs/4.WebGL噪色方案落地方案.md §5.3):
//   variant_id = Low32(Hash64(render_seed, capability_profile_version, "webgl-source-variant-v1"))
//
// Requirements:
//   - Cross-platform/compiler stable (no std::hash, no platform APIs).
//   - Versioned salt prevents accidental cross-version collisions.
//   - Used ONLY for variant selection within an approved capability profile;
//     does NOT create arbitrary GPU models or extensions.
uint32_t DeriveWebGLVariantId(uint64_t render_seed,
                               const std::string& capability_profile) {
  if (render_seed == 0) {
    return 0;  // Disabled.
  }
  if (capability_profile.empty()) {
    return 0;  // Fail-closed: no profile -> no variant.
  }

  // Versioned salt: bump this constant when the derivation algorithm changes.
  constexpr uint64_t kAlgorithmVersion = 1ULL;
  constexpr const char* kSalt = "webgl-source-variant-v1";

  // FNV-1a 64-bit hash (cross-platform stable, no dependencies).
  // Step 1: hash the seed.
  uint64_t hash = 14695981039346656037ULL;  // FNV offset basis.
  const uint64_t prime = 1099511628211ULL;
  hash ^= kAlgorithmVersion;
  hash *= prime;
  hash ^= render_seed;
  hash *= prime;
  // Step 2: hash the salt string.
  for (const char* p = kSalt; *p; ++p) {
    hash ^= static_cast<uint64_t>(*p);
    hash *= prime;
  }
  // Step 3: hash the capability_profile string.
  for (const char* p = capability_profile.c_str(); *p; ++p) {
    hash ^= static_cast<uint64_t>(*p);
    hash *= prime;
  }

  // Return lower 32 bits as the variant_id.
  return static_cast<uint32_t>(hash & 0xFFFFFFFFULL);
}

#endif  // Retired VEM render-seed/WebGL-variant derivation.

// =====================================================================
// Global singleton
// =====================================================================
static VirtualEnvironmentManager* g_vem = nullptr;

VirtualEnvironmentManager* GetVirtualEnvironmentManager() {
  if (!g_vem) {
    g_vem = new VirtualEnvironmentManager();
  }
  return g_vem;
}

// =====================================================================
// FormFactors
// =====================================================================
FormFactors::FormFactors() = default;
FormFactors::~FormFactors() = default;

// =====================================================================
// VirtualEnvironmentData
// =====================================================================
VirtualEnvironmentData::VirtualEnvironmentData() = default;
VirtualEnvironmentData::~VirtualEnvironmentData() = default;

// =====================================================================
// Helpers for reading nested JSON (all use const base::Value*)
// =====================================================================

// Navigate a base::Value dict by a path of string keys. Returns nullptr if not found.
static const base::Value* PathValue(const base::Value* root,
                                    base::span<const char* const> path) {
  if (!root) return nullptr;
  const base::Value* cur = root;
  for (const char* key : path) {
    if (!cur->is_dict()) return nullptr;
    const base::Value* next = cur->GetDict().Find(key);
    if (!next) return nullptr;
    cur = next;
  }
  return cur;
}

// Read a string from a nested dict path.
static std::string PathString(const base::Value* root,
                              base::span<const char* const> path) {
  const base::Value* v = PathValue(root, path);
  if (!v) return std::string();
  if (v->is_string()) return v->GetString();
  return std::string();
}

// Read an int from a nested dict path.
static int PathInt(const base::Value* root,
                   base::span<const char* const> path) {
  const base::Value* v = PathValue(root, path);
  if (!v) return 0;
  if (v->is_int()) return v->GetInt();
  if (v->is_double()) return static_cast<int>(v->GetDouble());
  return 0;
}

// Chromium parses gpu-vendor-id and gpu-device-id switch values as decimal
// uints. Runtime profiles use conventional hexadecimal PCI IDs, so normalize
// them at the VEM boundary instead of falling back to host DXGI enumeration.
static std::string NormalizeGpuIdForSwitch(const std::string& profile_value) {
  if (profile_value.empty()) {
    return std::string();
  }

  unsigned int parsed_value = 0;
  if (base::StringToUint(profile_value, &parsed_value) ||
      base::HexStringToUInt(profile_value, &parsed_value)) {
    return base::NumberToString(parsed_value);
  }

  LOG(ERROR) << "VirtualEnvironmentManager: invalid GPU PCI ID in profile: "
             << profile_value;
  return std::string();
}

// Read a string list from a nested dict path.
static std::vector<std::string> PathStringList(
    const base::Value* root,
    base::span<const char* const> path) {
  const base::Value* v = PathValue(root, path);
  if (!v || !v->is_list()) return {};
  std::vector<std::string> out;
  for (const auto& item : v->GetList()) {
    if (item.is_string()) out.push_back(item.GetString());
  }
  return out;
}

// Read a base::Value as a switch string (empty if null/absent).
static std::string ValueAsSwitchString(const base::Value* v) {
  if (!v) return std::string();
  if (v->is_string()) return v->GetString();
  return std::string();
}

// =====================================================================
// InitFromCommandLine
// =====================================================================
void VirtualEnvironmentManager::InitFromCommandLine() {
  base::CommandLine& cmdline = *base::CommandLine::ForCurrentProcess();

  // UA-CH simulation switches.
  GetForkSwitch(cmdline, kForkPlatform, &data_.platform);
  GetForkSwitch(cmdline, kForkPlatformVersion, &data_.platform_version);
  GetForkSwitch(cmdline, kForkArchitecture, &data_.architecture);
  GetForkSwitch(cmdline, kForkModel, &data_.model);
  GetForkSwitch(cmdline, kForkUAFullVersion, &data_.ua_full_version);
  GetForkSwitch(cmdline, kForkBitness, &data_.bitness);

  std::string wow64_str;
  GetForkSwitch(cmdline, kForkWow64, &wow64_str);
  data_.wow64 = (wow64_str == "1");

  std::string form_factors_str;
  GetForkSwitch(cmdline, kForkFormFactors, &form_factors_str);
  if (!form_factors_str.empty()) {
    std::vector<std::string_view> parts = base::SplitStringPiece(
        form_factors_str, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    for (const auto& pt : parts) {
      data_.form_factors.items.push_back(std::string(pt));
    }
  }

  GetForkSwitch(cmdline, kForkTimezone, &data_.timezone);
  GetForkSwitch(cmdline, kForkAcceptLanguages, &data_.accept_languages);
  // Compatibility input for launchers that still pass Chromium's historical
  // --accept-language argument instead of the VEM switch. The VEM/profile
  // value remains authoritative when both are present.
  if (data_.accept_languages.empty() &&
      cmdline.HasSwitch("accept-language")) {
    data_.accept_languages = cmdline.GetSwitchValueASCII("accept-language");
  }

  std::string cpu_cores_str;
  if (GetForkSwitch(cmdline, kForkCpuCores, &cpu_cores_str)) {
    int v = 0;
    if (base::StringToInt(cpu_cores_str, &v)) {
      data_.cpu_cores = v;
      data_.logical_threads = v;
    }
  }
  std::string memory_mb_str;
  if (GetForkSwitch(cmdline, kForkMemoryMB, &memory_mb_str)) {
    int64_t v = 0;
    if (base::StringToInt64(memory_mb_str, &v)) data_.memory_mb = v;
  }

  std::string screen_avail_height_str;
  if (GetForkSwitch(cmdline, kForkScreenAvailHeight, &screen_avail_height_str)) {
    int v = 0;
    if (base::StringToInt(screen_avail_height_str, &v)) {
      data_.screen_avail_height = v;
    }
  }

  std::string max_touch_points_str;
  if (GetForkSwitch(cmdline, kForkMaxTouchPoints, &max_touch_points_str)) {
    int v = -1;
    if (base::StringToInt(max_touch_points_str, &v)) data_.max_touch_points = v;
  }
  std::string touch_capable_str;
  if (GetForkSwitch(cmdline, kForkTouchCapable, &touch_capable_str)) {
    int v = -1;
    if (base::StringToInt(touch_capable_str, &v)) data_.touch_capable = v;
  }

  // Audio/WebAudio overrides are intentionally ignored. The renderer must
  // observe the host-native audio implementation and device characteristics.

  std::string battery_is_laptop_str;
  if (GetForkSwitch(cmdline, kForkBatteryIsLaptop, &battery_is_laptop_str)) {
    int v = -1;
    if (base::StringToInt(battery_is_laptop_str, &v)) data_.battery_is_laptop = v;
  }
  std::string battery_is_mobile_str;
  if (GetForkSwitch(cmdline, kForkBatteryIsMobile, &battery_is_mobile_str)) {
    int v = -1;
    if (base::StringToInt(battery_is_mobile_str, &v)) data_.battery_is_mobile = v;
  }
  std::string battery_charging_str;
  if (GetForkSwitch(cmdline, kForkBatteryCharging, &battery_charging_str)) {
    int v = -1;
    if (base::StringToInt(battery_charging_str, &v)) data_.battery_charging = v;
  }
  std::string battery_level_min_str;
  if (GetForkSwitch(cmdline, kForkBatteryLevelMin, &battery_level_min_str)) {
    double v = 0.0;
    if (base::StringToDouble(battery_level_min_str, &v)) data_.battery_level_min = v;
  }
  std::string battery_level_max_str;
  if (GetForkSwitch(cmdline, kForkBatteryLevelMax, &battery_level_max_str)) {
    double v = 0.0;
    if (base::StringToDouble(battery_level_max_str, &v)) data_.battery_level_max = v;
  }

  GetForkSwitch(cmdline, kForkWebGLVendor, &data_.webgl_vendor);
  GetForkSwitch(cmdline, kForkWebGLRenderer, &data_.webgl_renderer);
  GetForkSwitch(cmdline, kForkWebGLVersion, &data_.webgl_version);

  // WebGL source-variant parsing is retired together with VEM render_seed.
  // Explicit vendor/renderer/version values remain the only supported VEM
  // WebGL configuration. The disabled block is kept temporarily as a
  // migration marker so stale command-line switches are ignored safely.
#if 0
  // WebGL Variant (2026-07-27).
  // Priority: --disable-dchromium-fork-webgl-variant > --dchromium-fork-webgl-variant-enabled
  // If the disable flag is present, the entire variant block is forced off.
  if (cmdline.HasSwitch(kDisableForkWebGLVariant)) {
    data_.webgl_variant_enabled = false;
  } else {
    std::string variant_enabled_str;
    if (GetForkSwitch(cmdline, kForkWebGLVariantEnabled, &variant_enabled_str)) {
      data_.webgl_variant_enabled = (variant_enabled_str == "1");
    }
  }
  GetForkSwitch(cmdline, kForkWebGLCapabilityProfile, &data_.webgl_capability_profile);
  std::string variant_id_str;
  if (GetForkSwitch(cmdline, kForkWebGLVariantId, &variant_id_str)) {
    uint32_t v = 0;
    if (base::StringToUint(variant_id_str, &v)) {
      data_.webgl_variant_id = v;
    }
  }

#endif  // Retired VEM WebGL source-variant parsing.

  // Geolocation override (Phase 5; 2026-07-21 + Phase 5.b 2026-07-24).
  // Priority rules:
  //   1. Range form: if BOTH latitude_range AND longitude_range switches are present,
  //      InitFromCommandLine samples one random point from the brackets (uniform via
  //      base::RandDouble()). This is the Phase 5.b "no single hardcoded constant"
  //      path used by all 5 regional profiles.
  //   2. Single-point form (legacy): if only kForkGeoLatitude/longitude are present,
  //      those exact values are used. No random sampling.
  //   3. If both forms are present: range form (1) wins.
  // NOTE: equator (lat=0) and prime meridian (lon=0) are VALID coordinates.
  // geo_has_latitude_range / geo_has_longitude_range are the explicit signals;
  // geo_latitude==0.0 alone is NOT used as a sentinel.
  // Phase 5.b: range switches (format "min,max" with comma).
  auto ParseRangeAndSetFlag = [&](const char* switch_name,
                                  double out[2],
                                  bool* flag) {
    if (!cmdline.HasSwitch(switch_name)) return;
    std::string s = cmdline.GetSwitchValueASCII(switch_name);
    std::vector<std::string_view> parts = base::SplitStringPiece(
        s, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    if (parts.size() != 2) return;
    double lo = 0.0, hi = 0.0;
    if (!base::StringToDouble(std::string(parts[0]), &lo)) return;
    if (!base::StringToDouble(std::string(parts[1]), &hi)) return;
    if (hi < lo) std::swap(lo, hi);
    out[0] = lo;
    out[1] = hi;
    *flag = true;
  };
  ParseRangeAndSetFlag(kForkGeoLatitudeRange, data_.geo_latitude_range,
                       &data_.geo_has_latitude_range);
  ParseRangeAndSetFlag(kForkGeoLongitudeRange, data_.geo_longitude_range,
                       &data_.geo_has_longitude_range);
  ParseRangeAndSetFlag(kForkGeoAccuracyRange, data_.geo_accuracy_range,
                       &data_.geo_has_accuracy_range);
  // Single-point switches (legacy).
  if (cmdline.HasSwitch(kForkGeoLatitude)) {
    double v = 0.0;
    if (base::StringToDouble(
            cmdline.GetSwitchValueASCII(kForkGeoLatitude), &v)) {
      data_.geo_latitude = v;
    }
  }
  if (cmdline.HasSwitch(kForkGeoLongitude)) {
    double v = 0.0;
    if (base::StringToDouble(
            cmdline.GetSwitchValueASCII(kForkGeoLongitude), &v)) {
      data_.geo_longitude = v;
    }
  }
  if (cmdline.HasSwitch(kForkGeoAccuracy)) {
    double v = 0.0;
    if (base::StringToDouble(
            cmdline.GetSwitchValueASCII(kForkGeoAccuracy), &v)) {
      data_.geo_accuracy = v;
    }
  }
  if (cmdline.HasSwitch(kForkGeoAltitude)) {
    double v = 0.0;
    if (base::StringToDouble(
            cmdline.GetSwitchValueASCII(kForkGeoAltitude), &v)) {
      data_.geo_altitude = v;
    }
  }
  if (cmdline.HasSwitch(kForkGeoAltitudeAccuracy)) {
    double v = 0.0;
    if (base::StringToDouble(
            cmdline.GetSwitchValueASCII(kForkGeoAltitudeAccuracy), &v)) {
      data_.geo_altitude_accuracy = v;
    }
  }
  if (cmdline.HasSwitch(kForkGeoHeading)) {
    double v = 0.0;
    if (base::StringToDouble(
            cmdline.GetSwitchValueASCII(kForkGeoHeading), &v)) {
      data_.geo_heading = v;
    }
  }
  if (cmdline.HasSwitch(kForkGeoSpeed)) {
    double v = 0.0;
    if (base::StringToDouble(
            cmdline.GetSwitchValueASCII(kForkGeoSpeed), &v)) {
      data_.geo_speed = v;
    }
  }
  // Phase 5.c (2026-07-25): ISO 3166-1 alpha-2 country code.
  // Defence-in-depth: in practice the launcher (Go) is the canonical source
  // and only emits validated values from MaxMind. But a stray CLI override
  // or a hand-written profile.runtime.json could otherwise leak an empty
  // string, garbage, or a 3-letter code into VEM. Reject anything that is
  // not exactly two ASCII letters: discard silently (leave data_ untouched).
  std::string raw_country_code;
  if (GetForkSwitch(cmdline, kForkGeoCountryCode, &raw_country_code) &&
      IsValidIso3166Alpha2(raw_country_code)) {
    data_.geo_country_code = raw_country_code;
  }
  // Phase 5.b: range -> point sampling.
  if (data_.geo_has_latitude_range && data_.geo_has_longitude_range &&
      data_.geo_latitude_range[1] > data_.geo_latitude_range[0] &&
      data_.geo_longitude_range[1] > data_.geo_longitude_range[0]) {
    double lat_lo = data_.geo_latitude_range[0];
    double lat_hi = data_.geo_latitude_range[1];
    double lon_lo = data_.geo_longitude_range[0];
    double lon_hi = data_.geo_longitude_range[1];
    data_.geo_latitude = lat_lo + base::RandDouble() * (lat_hi - lat_lo);
    data_.geo_longitude = lon_lo + base::RandDouble() * (lon_hi - lon_lo);
    fprintf(stderr,
            "[FORK-DBG-GEO-RNG] sampled lat=%.6f lon=%.6f from "
            "[%.4f,%.4f] x [%.4f,%.4f] proc=%lu\n",
            data_.geo_latitude, data_.geo_longitude,
            lat_lo, lat_hi, lon_lo, lon_hi,
            static_cast<unsigned long>(base::GetCurrentProcId()));
    fflush(stderr);
  }
  // Phase 5.b: accuracy range -> accuracy sampling.
  if (data_.geo_has_accuracy_range &&
      data_.geo_accuracy_range[1] > data_.geo_accuracy_range[0]) {
    double acc_lo = data_.geo_accuracy_range[0];
    double acc_hi = data_.geo_accuracy_range[1];
    data_.geo_accuracy = acc_lo + base::RandDouble() * (acc_hi - acc_lo);
    fprintf(stderr,
            "[FORK-DBG-GEO-RNG] sampled accuracy=%.1f from [%.1f,%.1f] proc=%lu\n",
            data_.geo_accuracy, acc_lo, acc_hi,
            static_cast<unsigned long>(base::GetCurrentProcId()));
    fflush(stderr);
  }

  GetForkSwitch(cmdline, kForkPermissionGeolocation,
                &data_.geo_permission_override);

  std::string hide_webdriver_str;
  if (GetForkSwitch(cmdline, kForkHideWebdriver, &hide_webdriver_str)) {
    data_.hide_webdriver = (hide_webdriver_str == "1");
  }

  std::string reduced_motion_str;
  if (GetForkSwitch(cmdline, kForkPrefersReducedMotion,
                    &reduced_motion_str)) {
    data_.prefers_reduced_motion = (reduced_motion_str == "1");
  }

  // Media voices (Phase 3). Two switches: platform_version (the OS build
  // key used to look up the platform-specific voice list in profile.json)
  // and voices_list (the comma-separated voice names).
  GetForkSwitch(cmdline, kForkVoicesPlatformVersion,
                &data_.voices_platform_version);
  std::string voices_list_str;
  if (GetForkSwitch(cmdline, kForkVoicesList, &voices_list_str)) {
    std::vector<std::string_view> voice_parts = base::SplitStringPiece(
        voices_list_str, ",", base::TRIM_WHITESPACE,
        base::SPLIT_WANT_NONEMPTY);
    data_.voices.clear();
    data_.voices.reserve(voice_parts.size());
    for (const auto& vp : voice_parts) {
      data_.voices.emplace_back(vp);
    }
  }
  if (ForkVoicesDebugEnabled()) {
    ForkVoicesDbg(base::StringPrintf(
        "InitFromCommandLine: voices_platform_version=%s voices_count=%zu",
        data_.voices_platform_version.c_str(), data_.voices.size()));
    for (size_t i = 0; i < data_.voices.size() && i < 16; ++i) {
      ForkVoicesDbg(base::StringPrintf("  voice[%zu]=%s",
                                       i, data_.voices[i].c_str()));
    }
  }

  // Test-only Canvas 2D RGBA8 readback perturbation. No defaults are injected:
  // every value must be explicitly supplied, and the renderer-side helper also
  // requires an exact origin allowlist match before doing anything.
  std::string canvas_noise_str;
  if (GetForkSwitch(cmdline, kForkCanvasTestNoiseEnabled,
                    &canvas_noise_str)) {
    data_.canvas_test_noise_enabled = (canvas_noise_str == "1");
  }
  if (GetForkSwitch(cmdline, kForkCanvasTestNoiseSeed, &canvas_noise_str)) {
    base::StringToUint64(canvas_noise_str, &data_.canvas_test_noise_seed);
  }
  if (GetForkSwitch(cmdline, kForkCanvasTestNoiseCaseId, &canvas_noise_str)) {
    base::StringToUint(canvas_noise_str, &data_.canvas_test_noise_case_id);
  }
  if (GetForkSwitch(cmdline, kForkCanvasTestNoiseSampleRate,
                    &canvas_noise_str)) {
    base::StringToDouble(canvas_noise_str,
                         &data_.canvas_test_noise_sample_rate);
  }
  if (GetForkSwitch(cmdline, kForkCanvasTestNoiseMaxPixels,
                    &canvas_noise_str)) {
    base::StringToUint(canvas_noise_str, &data_.canvas_test_noise_max_pixels);
  }
  if (GetForkSwitch(cmdline, kForkCanvasTestNoiseDeltaMin,
                    &canvas_noise_str)) {
    base::StringToInt(canvas_noise_str, &data_.canvas_test_noise_delta_min);
  }
  if (GetForkSwitch(cmdline, kForkCanvasTestNoiseDeltaMax,
                    &canvas_noise_str)) {
    base::StringToInt(canvas_noise_str, &data_.canvas_test_noise_delta_max);
  }
  if (GetForkSwitch(cmdline, kForkCanvasTestNoiseMaxTotalDelta,
                    &canvas_noise_str)) {
    base::StringToInt(canvas_noise_str,
                      &data_.canvas_test_noise_max_total_delta);
  }
  data_.canvas_test_noise_allowed_origins.clear();
  if (GetForkSwitch(cmdline, kForkCanvasTestNoiseAllowedOrigins,
                    &canvas_noise_str)) {
    const auto origins = base::SplitStringPiece(
        canvas_noise_str, ",", base::TRIM_WHITESPACE,
        base::SPLIT_WANT_NONEMPTY);
    for (const auto origin : origins) {
      data_.canvas_test_noise_allowed_origins.emplace_back(origin);
    }
  }

  // The VEM render-seed/WebGL-variant integration is intentionally retired.
  // Canvas test noise uses CanvasTestSessionSeedManager instead. Keeping this
  // code disabled avoids a second seed source and prevents an unused VEM
  // seed from being mistaken for a production rendering control.
#if 0
  // VEM x RenderFingerprint (2026-07-26).
  //
  // Resolve render_seed with the following precedence:
  //   1. --dchromium-fork-render-seed=<n> on cmdline.
  //        - present && parses && n>0 : CLI override (use n)
  //        - present && parses && n=0 : explicit disable (force 0)
  //        - present && parse fails    : LOG_ERROR, fall through to (2)
  //                                       (do NOT silently treat as 0)
  //        - absent                    : fall through to (2)
  //   2. user_data_dir/render_seed.json (persisted across restarts; one
  //      file per VEM profile directory).
  //   3. base::RandUint64() -> write to disk atomically; on failure,
  //      keep the in-memory seed (best-effort, no crash).
  //
  // Renderer / GPU / utility child processes receive the resolved value
  // through Chromium's normal command-line inheritance: the browser main
  // process (which is the only one that calls Resolve() on a fresh
  // user_data_dir) AppendSwitchASCII(kForkRenderSeed, ...) right after
  // resolving, so subsequent forks in subprocesses re-enter this
  // function with kForkRenderSeed already present and resolve to the
  // same value (branch 1). They do NOT touch render_seed.json directly.
  //
  // 2026-07-26 audit fix: Browser main process must NOT distinguish
  // "seed=0 because user disabled it" from "seed=0 because we haven't
  // resolved yet". The AppendSwitchASCII at the end of this block
  // therefore writes the value even when it is 0 -- subprocesses see
  // HasSwitch=true and short-circuit to branch 1 instead of (re)reading
  // JSON or generating a fresh random seed.
  base::FilePath user_data_dir;
  if (cmdline.HasSwitch(switches::kForkUserDataDir)) {
    user_data_dir =
        cmdline.GetSwitchValuePath(switches::kForkUserDataDir);
  }
  uint64_t cli_seed = 0;
  bool cli_valid = false;
  const bool cli_present = cmdline.HasSwitch(kForkRenderSeed);
  if (cli_present) {
    const std::string cli_seed_str =
        cmdline.GetSwitchValueASCII(kForkRenderSeed);
    if (!base::StringToUint64(cli_seed_str, &cli_seed)) {
      // Parse failure: log loud and unambiguous, then fall through to
      // persistence / Rand. Critical: do NOT collapse to 0, because 0
      // is the explicit-disable sentinel and conflating the two makes
      // misconfigurations indistinguishable from intentional disable.
      LOG(ERROR) << "[render-seed] invalid --dchromium-fork-render-seed='"
                 << cli_seed_str << "', falling back to persistence/Rand";
      cli_valid = false;
    } else {
      cli_valid = true;
    }
  }
  const RenderSeedResolution resolved = RenderSeedStore::Resolve(
      user_data_dir, cli_present, cli_valid, cli_seed);
  data_.render_seed = resolved.seed;

  // Diagnostics: help trace which resolution branch fired when investigating
  // "fingerprint drifted across restart" / "two profiles share a fingerprint"
  // issues. Use DVLOG (debug-only) to avoid noise in release builds.
  DVLOG(1) << "[render-seed] resolve: cli=" << resolved.from_cli
           << " persisted=" << resolved.from_persistence
           << " generated=" << resolved.newly_generated
           << " write_failed=" << resolved.write_back_failed
           << " seed=0x" << std::hex << resolved.seed;

  // Subprocess propagation: write the resolved value into the current
  // process command line so that downstream forks inherit it. We always
  // write, including the explicit-0 (disable) case, so that subprocesses
  // can short-circuit Resolve() and avoid regenerating a different seed.
  if (cli_present) {
    // Caller-set CLI still wins semantically; in InitFromCommandLine this
    // branch applies to subprocesses that just inherited the switch.
  }
  // (No AppendSwitchASCII here: InitFromCommandLine runs in subprocesses
  // after the browser main process has already done the propagation in
  // InitFromProfileJSON. Re-writing would be a no-op for an already-set
  // switch but adds noise to logs.)

#endif  // Retired VEM render-seed integration; see comment above.

  // Diagnostic log: emit [FORK-DBG-GEO-CLI] for every geolocation switch found on
  // the command line. This lets operators verify that the GeoIP-driven launcher's
  // --dchromium-fork-geo-latitude/longitude/accuracy switches are being parsed
  // correctly by VEM before InitFromProfileJSON runs.
  std::string cli_lat = cmdline.GetSwitchValueASCII(kForkGeoLatitude);
  std::string cli_lon = cmdline.GetSwitchValueASCII(kForkGeoLongitude);
  std::string cli_acc = cmdline.GetSwitchValueASCII(kForkGeoAccuracy);
  if (!cli_lat.empty() || !cli_lon.empty() || !cli_acc.empty()) {
    fprintf(stderr,
            "[FORK-DBG-GEO-CLI] CLI switches parsed: lat=%s lon=%s acc=%s\n",
            cli_lat.c_str(), cli_lon.c_str(), cli_acc.c_str());
    fflush(stderr);
  }
  std::string cli_country = cmdline.GetSwitchValueASCII(kForkGeoCountryCode);
  if (!cli_country.empty()) {
    fprintf(stderr, "[FORK-DBG-GEO-CLI] country_code=%s\n",
            cli_country.c_str());
    fflush(stderr);
  }

  data_.initialized = true;

  DVLOG(1) << "VirtualEnvironmentManager initialized: platform="
            << data_.platform << " version=" << data_.platform_version
            << " tz=" << data_.timezone
            << " cpu_cores=" << data_.cpu_cores
            << " memory_mb=" << data_.memory_mb
            << " avail_height=" << data_.screen_avail_height
            << " webgl_vendor=" << data_.webgl_vendor
            << " webgl_renderer=" << data_.webgl_renderer
            << " geo_lat=" << data_.geo_latitude
            << " geo_lon=" << data_.geo_longitude
            << " geo_acc=" << data_.geo_accuracy;
}

// =====================================================================
// InitFromProfileJSON
// =====================================================================
void VirtualEnvironmentManager::InitFromProfileJSON(
    const base::FilePath& profile_path) {
  std::string json_text;
  if (!base::ReadFileToString(profile_path, &json_text)) {
    LOG(ERROR) << "VirtualEnvironmentManager: cannot read profile "
               << profile_path;
    return;
  }

  auto profile_opt = base::JSONReader::Read(
      json_text, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!profile_opt.has_value()) {
    LOG(ERROR) << "VirtualEnvironmentManager: invalid profile JSON at "
               << profile_path;
    return;
  }
  const base::Value* profile = profile_opt ? &(*profile_opt) : nullptr;

  // Primary locale language: --lang switch (for GetApplicationLocale).
  // Fallback: regional.locale (current schema used by all regional/*.json
  // templates including hk_zh.json). Without this, --lang is empty for HK/TW/CN
  // profiles and GetApplicationLocale() falls back to the OS default (zh-CN).
  //
  // v2 schema (ui_locale / web_locale split):
  //   ui_locale  → --lang (Chrome UI PAK resources)
  //   web_locale → first entry of accept_languages (navigator.language)
  // For HK: ui_locale=zh-TW, web_locale=zh-HK (no zh-HK.pak on Windows)
  std::string locale_language =
      PathString(profile, {"environment", "regional", "ui_locale"});
  if (locale_language.empty()) {
    locale_language = PathString(profile, {"environment", "regional", "locale"});
  }
  if (locale_language.empty()) {
    locale_language = PathString(profile, {"environment", "locale", "language"});
  }
  if (locale_language.empty()) {
    locale_language = PathString(profile, {"regional", "locale"});
  }

  std::vector<std::string> languages_list =
      PathStringList(profile, {"environment", "regional", "accept_languages"});
  if (languages_list.empty()) {
    languages_list =
        PathStringList(profile, {"environment", "regional", "languages"});
  }
  if (languages_list.empty()) {
    languages_list =
        PathStringList(profile, {"environment", "locale", "languages"});
  }
  if (languages_list.empty()) {
    languages_list = PathStringList(profile, {"regional", "languages"});
  }
  std::string accept_languages;
  for (size_t i = 0; i < languages_list.size(); ++i) {
    if (i > 0) accept_languages += ",";
    accept_languages += languages_list[i];
  }

  std::string version =
      PathString(profile, {"environment", "system", "version"});
  std::string cpu_arch =
      PathString(profile, {"environment", "hardware", "cpu", "architecture"});
  std::string model =
      PathString(profile, {"environment", "hardware", "cpu", "model"});
  // Timezone for renderer adoptDefault + ICU override.
  // Fallback: regional.timezone (current schema for HK/TW/CN profiles).
  std::string timezone =
      PathString(profile, {"environment", "locale", "timezone"});
  if (timezone.empty()) {
    timezone = PathString(profile, {"environment", "regional", "timezone"});
  }
  if (timezone.empty()) {
    timezone = PathString(profile, {"regional", "timezone"});
  }
  int logical_threads = PathInt(
      profile, {"environment", "hardware", "cpu", "logical_processors"});
  if (logical_threads <= 0) {
    logical_threads =
        PathInt(profile, {"environment", "hardware", "cpu", "cores"});
  }
  int memory_mb =
      PathInt(profile, {"environment", "hardware", "memory_mb"});
  std::string cpu_cores_str = (logical_threads > 0)
                                  ? base::NumberToString(logical_threads)
                                  : "";
  std::string memory_mb_str =
      (memory_mb > 0) ? base::NumberToString(memory_mb) : "";
  std::string bitness = (cpu_arch == "x86_64") ? "64" : "32";
  std::string form_factors = "Desktop";

  std::string gpu_vendor_id =
      PathString(profile, {"environment", "hardware", "gpu", "vendor_id"});
  std::string gpu_device_id =
      PathString(profile, {"environment", "hardware", "gpu", "device_id"});
  std::string gpu_driver_ver =
      PathString(profile, {"environment", "hardware", "gpu", "driver_version"});
  std::string gpu_vendor =
      PathString(profile, {"environment", "hardware", "gpu", "vendor"});
  std::string gpu_model =
      PathString(profile, {"environment", "hardware", "gpu", "model"});
  if (gpu_model.empty()) {
    gpu_model =
        PathString(profile, {"environment", "hardware", "gpu", "description"});
  }
  gpu_vendor_id = NormalizeGpuIdForSwitch(gpu_vendor_id);
  gpu_device_id = NormalizeGpuIdForSwitch(gpu_device_id);

  std::string webgl_vendor =
      PathString(profile, {"environment", "hardware", "webgl", "vendor"});
  std::string webgl_renderer =
      PathString(profile, {"environment", "hardware", "webgl", "renderer"});
  std::string webgl_version =
      PathString(profile, {"environment", "hardware", "webgl", "version"});
  if (webgl_vendor.empty()) {
    webgl_vendor = PathString(profile, {"environment", "webgl", "vendor"});
  }
  if (webgl_renderer.empty()) {
    webgl_renderer = PathString(profile, {"environment", "webgl", "renderer"});
  }
  if (webgl_version.empty()) {
    webgl_version = PathString(profile, {"environment", "webgl", "version"});
  }
  if (webgl_vendor.empty()) {
    webgl_vendor = PathString(
        profile, {"environment", "graphics", "webgl", "unmasked_vendor"});
  }
  if (webgl_renderer.empty()) {
    webgl_renderer = PathString(
        profile, {"environment", "graphics", "webgl", "unmasked_renderer"});
  }
  if (webgl_renderer.empty()) {
    webgl_renderer = PathString(
        profile, {"environment", "graphics", "adapter", "description"});
  }
  if (webgl_renderer.empty()) webgl_renderer = gpu_model;
  if (webgl_vendor.empty()) webgl_vendor = gpu_vendor;

  // Write switches to the current process command line.
  base::CommandLine* cmdline_for_write = base::CommandLine::ForCurrentProcess();
  auto do_write = [&](const char* key, const std::string& val) {
    if (!val.empty()) {
      cmdline_for_write->AppendSwitchASCII(key, val);
    }
  };

  // Phase 5 (2026-07-21) + Phase 5.b (2026-07-24): Geolocation override.
  // Priority: range (1) > single-point (2). Profile JSON never emits both.
  const base::Value* geo_dict =
      PathValue(profile, {"environment", "session", "geolocation"});
  if (!geo_dict || !geo_dict->is_dict()) {
    geo_dict = PathValue(profile, {"environment", "geolocation"});
  }

  bool has_lat_range = false;
  bool has_lon_range = false;
  bool has_acc_range = false;
  double lat_range[2] = {0.0, 0.0};
  double lon_range[2] = {0.0, 0.0};
  double acc_range[2] = {0.0, 0.0};

  if (geo_dict && geo_dict->is_dict()) {
    auto ReadRange = [&](const char* key, double out[2],
                         bool is_lat) -> bool {
      const base::Value* arr = PathValue(geo_dict, {key});
      if (!arr || !arr->is_list() || arr->GetList().size() != 2) return false;
      const auto& list = arr->GetList();
      if (!list[0].is_double() && !list[0].is_int()) return false;
      if (!list[1].is_double() && !list[1].is_int()) return false;
      double lo = list[0].is_double() ? list[0].GetDouble()
                                      : static_cast<double>(list[0].GetInt());
      double hi = list[1].is_double() ? list[1].GetDouble()
                                      : static_cast<double>(list[1].GetInt());
      if (hi < lo) std::swap(lo, hi);
      double bound_lo = is_lat ? -90.0 : -180.0;
      double bound_hi = is_lat ? 90.0 : 180.0;
      if (lo < bound_lo || hi > bound_hi || hi <= lo) return false;
      out[0] = lo;
      out[1] = hi;
      return true;
    };

    has_lat_range = ReadRange("latitude_range", lat_range, true);
    has_lon_range = ReadRange("longitude_range", lon_range, false);
    has_acc_range = ReadRange("accuracy_range", acc_range, false);

    std::string lat_str = ValueAsSwitchString(PathValue(geo_dict, {"latitude"}));
    std::string lon_str = ValueAsSwitchString(PathValue(geo_dict, {"longitude"}));
    std::string acc_str = ValueAsSwitchString(PathValue(geo_dict, {"accuracy"}));
    std::string alt_str = ValueAsSwitchString(PathValue(geo_dict, {"altitude"}));
    std::string alt_acc_str =
        ValueAsSwitchString(PathValue(geo_dict, {"altitude_accuracy"}));
    std::string heading_str =
        ValueAsSwitchString(PathValue(geo_dict, {"heading"}));
    std::string speed_str = ValueAsSwitchString(PathValue(geo_dict, {"speed"}));
    double lat = 0.0;
    double lon = 0.0;
    double acc = 0.0;
    bool lat_set = !lat_str.empty() && base::StringToDouble(lat_str, &lat);
    bool lon_set = !lon_str.empty() && base::StringToDouble(lon_str, &lon);
    bool acc_set = !acc_str.empty() && base::StringToDouble(acc_str, &acc);

    if (has_lat_range && has_lon_range) {
      do_write(kForkGeoLatitudeRange,
               base::NumberToString(lat_range[0]) + "," +
                   base::NumberToString(lat_range[1]));
      do_write(kForkGeoLongitudeRange,
               base::NumberToString(lon_range[0]) + "," +
                   base::NumberToString(lon_range[1]));
      if (has_acc_range) {
        do_write(kForkGeoAccuracyRange,
                 base::NumberToString(acc_range[0]) + "," +
                     base::NumberToString(acc_range[1]));
      } else if (acc_set && acc > 0.0) {
        do_write(kForkGeoAccuracy, acc_str);
      }
      if (acc_set && acc > 0.0) {
        if (!alt_str.empty()) do_write(kForkGeoAltitude, alt_str);
        if (!alt_acc_str.empty()) do_write(kForkGeoAltitudeAccuracy, alt_acc_str);
        if (!heading_str.empty()) do_write(kForkGeoHeading, heading_str);
        if (!speed_str.empty()) do_write(kForkGeoSpeed, speed_str);
      }
    } else if (lat_set && lon_set && acc_set && acc > 0.0 &&
               lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0) {
      do_write(kForkGeoLatitude, lat_str);
      do_write(kForkGeoLongitude, lon_str);
      do_write(kForkGeoAccuracy, acc_str);
      if (!alt_str.empty()) do_write(kForkGeoAltitude, alt_str);
      if (!alt_acc_str.empty()) do_write(kForkGeoAltitudeAccuracy, alt_acc_str);
      if (!heading_str.empty()) do_write(kForkGeoHeading, heading_str);
      if (!speed_str.empty()) do_write(kForkGeoSpeed, speed_str);
    }
  }

  // The current profile schema keeps permission defaults below environment.
  // Retain the old path as a compatibility read for existing profiles.
  std::string permission_geo = PathString(
      profile, {"environment", "permissions", "defaults", "geolocation"});
  if (permission_geo.empty()) {
    permission_geo = PathString(profile, {"permissions", "geolocation"});
  }

  do_write(kForkPlatform, "Windows");
  do_write(kForkPlatformVersion, version);
  do_write(kForkArchitecture, cpu_arch);
  do_write(kForkModel, model);
  do_write(kForkBitness, bitness);
  do_write(kForkWow64, "0");
  do_write(kForkFormFactors, form_factors);
  do_write(kForkTimezone, timezone);
  if (!locale_language.empty()) {
    cmdline_for_write->AppendSwitchASCII("lang", locale_language);
  }
  do_write(kForkAcceptLanguages, accept_languages);
  do_write(kForkCpuCores, cpu_cores_str);
  do_write(kForkMemoryMB, memory_mb_str);
  do_write(kForkScreenAvailHeight,
           PathString(profile, {"environment", "session", "screen", "avail_height"}));
  do_write(kForkMaxTouchPoints,
           PathString(profile, {"environment", "session", "touch", "max_touch_points"}));
  do_write(kForkTouchCapable,
           PathString(profile, {"environment", "session", "touch", "touch_capable"}));
  // Audio is intentionally not a VEM-controlled surface.  Keep Web Audio,
  // output devices, and latency values on the host-native Chromium path.
  do_write(kForkBatteryIsLaptop,
           PathString(profile, {"environment", "session", "battery", "is_laptop"}));
  do_write(kForkBatteryIsMobile,
           PathString(profile, {"environment", "session", "battery", "is_mobile"}));
  do_write(kForkBatteryCharging,
           PathString(profile, {"environment", "session", "battery", "charging"}));
  do_write(kForkBatteryLevelMin,
           PathString(profile, {"environment", "session", "battery", "level_min"}));
  do_write(kForkBatteryLevelMax,
           PathString(profile, {"environment", "session", "battery", "level_max"}));
  do_write(kForkWebGLVendor, webgl_vendor);
  do_write(kForkWebGLRenderer, webgl_renderer);
  do_write(kForkWebGLVersion, webgl_version);

  // WebGL Variant (2026-07-27). See Docs/4.WebGL噪色方案落地方案.md.
  // Priority: --disable-dchromium-fork-webgl-variant (emergency rollback) >
  //            profile.webgl_variant.enabled > default (disabled).
  // Only writes variant switches if enabled; otherwise leaves them absent
  // so InitFromCommandLine sees empty -> disabled (seed=0 behavior).
  // Retired VEM WebGL source-variant generation. The active VEM WebGL
  // configuration is limited to explicit vendor/renderer/version values.
#if 0
  if (!cmdline_for_write->HasSwitch(kDisableForkWebGLVariant)) {
    std::string variant_enabled_str = PathString(
        profile, {"environment", "webgl_variant", "enabled"});
    if (variant_enabled_str == "true" || variant_enabled_str == "1") {
      std::string capability_profile = PathString(
          profile, {"environment", "webgl_variant", "capability_profile"});
      // Validate: capability_profile must be non-empty if enabled.
      if (!capability_profile.empty()) {
        do_write(kForkWebGLVariantEnabled, "1");
        do_write(kForkWebGLCapabilityProfile, capability_profile);
        // variant_id is computed by DeriveWebGLVariantId() from seed + profile.
        // Read seed from render_seed (set above by RenderSeedStore).
        // If seed == 0, variant stays disabled regardless of profile settings.
        if (data_.render_seed != 0) {
          uint32_t variant_id = DeriveWebGLVariantId(
              data_.render_seed, capability_profile);
          do_write(kForkWebGLVariantId, base::NumberToString(variant_id));
          // Mirror to data_ for accessors.
          data_.webgl_variant_enabled = true;
          data_.webgl_capability_profile = capability_profile;
          data_.webgl_variant_id = variant_id;
        }
      }
    }
  }
  // If --disable switch is present, force all variant fields to disabled state.
  if (cmdline_for_write->HasSwitch(kDisableForkWebGLVariant)) {
    data_.webgl_variant_enabled = false;
    data_.webgl_capability_profile.clear();
    data_.webgl_variant_id = 0;
  }

#endif  // Retired VEM WebGL source-variant generation.

  do_write(kForkGeoLatitude,
           PathString(profile, {"environment", "geolocation", "latitude"}));
  do_write(kForkGeoLongitude,
           PathString(profile, {"environment", "geolocation", "longitude"}));
  do_write(kForkGeoAccuracy,
           PathString(profile, {"environment", "geolocation", "accuracy"}));
  do_write(kForkGeoAltitude,
           PathString(profile, {"environment", "geolocation", "altitude"}));
  do_write(kForkGeoAltitudeAccuracy,
           PathString(profile, {"environment", "geolocation", "altitude_accuracy"}));
  do_write(kForkGeoHeading,
           PathString(profile, {"environment", "geolocation", "heading"}));
  do_write(kForkGeoSpeed,
           PathString(profile, {"environment", "geolocation", "speed"}));
  // Phase 5.c (2026-07-25): ISO 3166-1 alpha-2 country code.
  // Priority (newest path first, falls back to legacy):
  //   1. "environment" / "regional" / "country_code" - schema produced by
  //      go_manager profile_builder.Pipeline → profile.runtime.json
  //      (the runtime path consumed by VEM InitFromProfileJSON in the current
  //      interactive_launch flow).
  //   2. "regional" / "country_code" - schema used by assets/profiles_base/regional/*.json
  //      (legacy flat regional templates; hk_zh, cn_zh, jp_ja, us_en, gb_en).
  //   3. (fallback) top-level "country_code" - reserved for future flat profiles.
  std::string geo_country_code =
      PathString(profile, {"environment", "regional", "country_code"});
  if (geo_country_code.empty()) {
    geo_country_code = PathString(profile, {"regional", "country_code"});
  }
  if (geo_country_code.empty()) {
    geo_country_code = PathString(profile, {"country_code"});
  }
  do_write(kForkGeoCountryCode, geo_country_code);
  // Mirror into data_ so that subsequent VEM accessors in this process
  // (InitFromProfileJSON is the in-process entry point for the browser main
  // process) reflect the JSON value without requiring a second
  // InitFromCommandLine() pass. Subprocesses (renderer/gpu/utility) keep
  // populating data_ via InitFromCommandLine() in their own startup.
  // Priority: JSON > CLI. Once data_ is touched here, any CLI value
  // already loaded by InitFromCommandLine() is overwritten on purpose —
  // the profile JSON is the canonical session intent.
  if (!geo_country_code.empty() &&
      IsValidIso3166Alpha2(geo_country_code)) {
    data_.geo_country_code = geo_country_code;
  } else if (!geo_country_code.empty()) {
    LOG(WARNING) << "VirtualEnvironmentManager: profile supplied invalid "
                 << "country_code='" << geo_country_code
                 << "', keeping empty (ISO 3166-1 alpha-2 required)";
  }
  do_write(kForkPermissionGeolocation, permission_geo);
  do_write(kForkHideWebdriver,
           PathString(profile, {"environment", "session", "hide_webdriver"}));
  do_write(kForkPrefersReducedMotion,
           PathString(profile, {"environment", "session", "prefers_reduced_motion"}));
  // Media devices and speech-synthesis voices are also intentionally native.
  // Do not promote profile values to command-line switches.

  do_write("gpu-vendor-id", gpu_vendor_id);
  do_write("gpu-device-id", gpu_device_id);
  do_write("gpu-driver-version", gpu_driver_ver);

  // Set ANGLE environment variables for WebGL spoofing.
#if defined(OS_WIN)
  if (!webgl_vendor.empty()) {
    _putenv_s("ANGLE_GL_VENDOR", webgl_vendor.c_str());
  }
  if (!webgl_renderer.empty()) {
    _putenv_s("ANGLE_GL_RENDERER", webgl_renderer.c_str());
  }
  if (!webgl_version.empty()) {
    _putenv_s("ANGLE_GL_VERSION", webgl_version.c_str());
  }
#else
  if (!webgl_vendor.empty()) {
    setenv("ANGLE_GL_VENDOR", webgl_vendor.c_str(), 1);
  }
  if (!webgl_renderer.empty()) {
    setenv("ANGLE_GL_RENDERER", webgl_renderer.c_str(), 1);
  }
  if (!webgl_version.empty()) {
    setenv("ANGLE_GL_VERSION", webgl_version.c_str(), 1);
  }
#endif

  // RenderSeedStore/VEM propagation is retired. The active seed source for
  // Canvas tests is CanvasTestSessionSeedManager; production VEM no longer
  // resolves or forwards a separate render seed.
  //
  // Note (2026-07-27 W2 audit): base::FilePath user_data_dir is declared
  // here (above the retired #if 0 ... #endif block) so that the Canvas
  // anti-fraud seed resolver below (post-#endif) can read it. Earlier
  // reformatting had moved the declaration inside the #if 0 dead-strip
  // region, which produced an "use of undeclared identifier" compile
  // error in the active Canvas anti-fraud seed block.
  base::FilePath user_data_dir;
  if (cmdline_for_write->HasSwitch(switches::kForkUserDataDir)) {
    user_data_dir =
        cmdline_for_write->GetSwitchValuePath(switches::kForkUserDataDir);
  }
#if 0
  // VEM x RenderFingerprint (2026-07-26):
  // After parsing the profile JSON, resolve the render_seed via the same
  // precedence used by InitFromCommandLine() and propagate it to the
  // command line so that renderer / GPU / utility subprocesses inherit it.
  //
  // We do NOT regenerate on every call: Resolve() honours the CLI override
  // (--dchromium-fork-render-seed) first, then user_data_dir persistence,
  // then generates only when neither is present. So calling it once here
  // (Browser main process) is sufficient; subprocesses will hit branch 1
  // and skip persistence entirely.
  //
  // 2026-07-26 audit fix: explicit-0 (CLI user disabled) MUST propagate
  // to subprocesses. Previously this branch only wrote when seed != 0,
  // letting subprocesses re-resolve and potentially flip to a different
  // value. AppendSwitchASCII is idempotent for already-present switches
  // in Chromium, so writing the same value twice is a no-op.
  uint64_t cli_seed = 0;
  bool cli_valid = false;
  const bool cli_present = cmdline_for_write->HasSwitch(kForkRenderSeed);
  if (cli_present) {
    const std::string cli_seed_str =
        cmdline_for_write->GetSwitchValueASCII(kForkRenderSeed);
    if (!base::StringToUint64(cli_seed_str, &cli_seed)) {
      LOG(ERROR) << "[render-seed] profile-init: invalid "
                    "--dchromium-fork-render-seed='"
                 << cli_seed_str << "', falling back to persistence/Rand";
      cli_valid = false;
    } else {
      cli_valid = true;
    }
  }
  const RenderSeedResolution resolved = RenderSeedStore::Resolve(
      user_data_dir, cli_present, cli_valid, cli_seed);

  // Always write the resolved seed back into the command line so that
  // subprocesses (renderer/gpu/utility) inherit the canonical value
  // WITHOUT touching render_seed.json themselves. This includes the
  // explicit-0 (disable) case: subprocesses must see HasSwitch=true
  // and short-circuit to branch 1 of Resolve().
  cmdline_for_write->AppendSwitchASCII(
      kForkRenderSeed, base::NumberToString(resolved.seed));
  data_.render_seed = resolved.seed;

  DVLOG(1) << "[render-seed] profile-init resolve: cli=" << resolved.from_cli
           << " persisted=" << resolved.from_persistence
           << " generated=" << resolved.newly_generated
           << " seed=0x" << std::hex << resolved.seed;

#endif  // Retired VEM render-seed propagation.

  // Canvas anti-fraud seed (2026-07-26).
  //
  // Resolve the effective canvas anti-fraud seed with the same precedence
  // used by CanvasTestSessionSeedManager::DetermineSessionSeedLocked()
  // (CLI > JSON > Rand) and propagate the canonical value to subprocesses
  // via AppendSwitchASCII. This ensures that:
  //   - Renderer / GPU / utility child processes share the same seed as
  //     the Browser main process (no regeneration drift).
  //   - Cross-process stability is preserved across renderer restarts.
  //   - Cross-process variation only happens when the user opts in via
  //     --canvas-anti-fraud-seed=<n> or clears the persisted JSON.
  //
  // The CanvasAntiFraudSeedStore honours CLI=0 as "fall through", so we
  // also handle the explicit-disable semantics the same way: if the user
  // passed --canvas-anti-fraud-seed=0 the resolver falls through to JSON
  // or Rand, which is the intended behaviour (we never write a literal 0
  // because that would short-circuit the xorshift64 PRNG and produce a
  // constant fingerprint signature).
  uint64_t caf_cli_seed = 0;
  bool caf_cli_valid = false;
  const bool caf_cli_present =
      cmdline_for_write->HasSwitch(switches::kForkCanvasAntiFraudSeed);
  if (caf_cli_present) {
    const std::string caf_cli_str =
        cmdline_for_write->GetSwitchValueASCII(
            switches::kForkCanvasAntiFraudSeed);
    caf_cli_valid = base::StringToUint64(caf_cli_str, &caf_cli_seed);
    if (!caf_cli_valid) {
      LOG(ERROR) << "[canvas-anti-fraud-seed] profile-init: invalid "
                    "--canvas-anti-fraud-seed='"
                 << caf_cli_str << "', falling back to persistence/Rand";
    }
  }
  const CanvasAntiFraudSeedResolution caf_resolved =
      CanvasAntiFraudSeedStore::Resolve(user_data_dir, caf_cli_present,
                                        caf_cli_valid, caf_cli_seed);

  // Always write the resolved value (including the 0-fall-through case
  // where caf_cli_present && caf_cli_valid && caf_cli_seed == 0 has
  // been resolved to a JSON or Rand value). Subprocesses see
  // HasSwitch=true and short-circuit to branch 1 of Resolve() in their
  // own session_seed_manager.cc::DetermineSessionSeedLocked.
  cmdline_for_write->AppendSwitchASCII(
      switches::kForkCanvasAntiFraudSeed,
      base::NumberToString(caf_resolved.seed));

  DVLOG(1) << "[canvas-anti-fraud-seed] profile-init resolve: cli="
           << caf_resolved.from_cli
           << " persisted=" << caf_resolved.from_persistence
           << " generated=" << caf_resolved.newly_generated
           << " seed=0x" << std::hex << caf_resolved.seed;

  LOG(INFO) << "VirtualEnvironmentManager: loaded from " << profile_path
            << " platform=" << version << " arch=" << cpu_arch
            << " tz=" << timezone
            << " cpu_cores=" << cpu_cores_str
            << " memory_mb=" << memory_mb_str
            << " gpu=" << gpu_vendor << " " << gpu_model
            << " (vendor_id=" << gpu_vendor_id << " device_id=" << gpu_device_id
            << " driver=" << gpu_driver_ver << ")"
            << " webgl_vendor=" << webgl_vendor
            << " webgl_renderer=" << webgl_renderer
            << " webgl_version=" << webgl_version
            << " geo_lat_range=" << (has_lat_range ? "yes" : "no")
            << " geo_lon_range=" << (has_lon_range ? "yes" : "no")
            << " geo_acc_range=" << (has_acc_range ? "yes" : "no")
            << " geo_country_code=" << geo_country_code;
}

// Phase 5.c (2026-07-25): ISO 3166-1 alpha-2 country code validator.
// Accepts exactly two ASCII letters (case-insensitive). Rejects empty
// strings, 3-letter codes, digits, and any non-ASCII input. Used by
// both InitFromCommandLine() and InitFromProfileJSON() to drop invalid
// values before they leak into data_.geo_country_code.
bool IsValidIso3166Alpha2(const std::string& code) {
  if (code.size() != 2) return false;
  for (char c : code) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
      return false;
    }
  }
  return true;
}

// Voices debug logging (2026-07-27). Toggle via env var
// DCHROMIUM_FORK_DEBUG_VOICES. Any non-empty / non-"0" value enables.
// Cached at first call (cheap; deterministic for the process lifetime).
bool ForkVoicesDebugEnabled() {
  static const bool enabled = []() -> bool {
#if defined(OS_WIN)
    char buf[8] = {0};
    size_t len = 0;
    getenv_s(&len, buf, sizeof(buf) - 1, "DCHROMIUM_FORK_DEBUG_VOICES");
    if (len == 0) return false;
    return !(buf[0] == '0' && buf[1] == '\0');
#else
    const char* v = getenv("DCHROMIUM_FORK_DEBUG_VOICES");
    if (!v || !*v) return false;
    return !(v[0] == '0' && v[1] == '\0');
#endif
  }();
  return enabled;
}

// Helper: write a single [FORK-DBG-VOICES] line to stderr (unconditional;
// caller must guard with ForkVoicesDebugEnabled() to keep noise out of
// release builds).
void ForkVoicesDbg(const std::string& msg) {
  fprintf(stderr, "[FORK-DBG-VOICES] %s\n", msg.c_str());
  fflush(stderr);
}

}  // namespace chromium_fork
