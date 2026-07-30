// Copyright 2026 Dchromium_fork

#include "chromium_fork/canvas_anti_fraud_seed_store.h"
#include "chromium_fork/canvas_session_seed_manager.h"
#include "chromium_fork/switches.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/command_line.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/process/process_handle.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/values.h"
#include "chromium_fork/switches.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace chromium_fork {

CanvasTestConfig::CanvasTestConfig() = default;
CanvasTestConfig::~CanvasTestConfig() = default;
CanvasTestConfig::CanvasTestConfig(const CanvasTestConfig&) = default;
CanvasTestConfig& CanvasTestConfig::operator=(const CanvasTestConfig&) = default;
CanvasTestConfig::CanvasTestConfig(CanvasTestConfig&&) noexcept = default;
CanvasTestConfig& CanvasTestConfig::operator=(CanvasTestConfig&&) noexcept = default;

namespace {

// New unified switch names (Phase P1).
constexpr const char kSwEnable[] = "canvas-test-noise-enabled";
constexpr const char kSwFixedSeed[] = "canvas-fixed-seed";
constexpr const char kSwOrigins[] = "canvas-test-noise-origins";
constexpr const char kSwVersion[] = "canvas-test-noise-version";

// Legacy switch names kept for backward compatibility.
constexpr const char kLegacyEnable[] = "dchromium-fork-canvas-test-noise-enabled";
constexpr const char kLegacySeed[] = "dchromium-fork-canvas-test-noise-seed";
constexpr const char kLegacyOrigins[] =
    "dchromium-fork-canvas-test-noise-allowed-origins";

// Phase P1-2 (2026-07-25): strict flag-on detection. Returns:
//   -1 if switch is present and explicitly set to "0" or "false" (force off)
//   +1 if switch is present and explicitly set to "1" or "true"  (force on)
//    0 if switch is absent.
// We treat any other value as "absent" (operator typo -> no-op, not
// silent failure).
int SwitchFlagTrioState(const base::CommandLine& cmdline, const char* name) {
  if (!cmdline.HasSwitch(name)) {
    return 0;
  }
  const std::string v = cmdline.GetSwitchValueASCII(name);
  if (v.empty() || v == "1" || v == "true") {
    return 1;
  }
  if (v == "0" || v == "false") {
    return -1;
  }
  LOG(WARNING) << "[CANVAS-TEST-INFO] switch '" << name
               << "' has non-boolean value '" << v << "', treating as absent";
  return 0;
}

// Resolve the new-switch enable state using the strict priority rule:
//   1. New switch explicitly on  -> enabled
//   2. New switch explicitly off -> disabled (overrides legacy)
//   3. New switch absent + legacy on  -> enabled
//   4. Otherwise -> disabled
int ResolveEnabledState(const base::CommandLine& cmdline) {
  const int new_state = SwitchFlagTrioState(cmdline, kSwEnable);
  const int legacy_state = SwitchFlagTrioState(cmdline, kLegacyEnable);
  if (new_state == 1) return 1;
  if (new_state == -1) return -1;
  if (legacy_state == 1) return 1;
  if (legacy_state == -1) return -1;
  return 0;
}

// Phase P1-1 (2026-07-25): parse one origin entry through url::Origin so that
// scheme/host/port/opaque are validated. Returns empty optional and logs an
// error when the entry is invalid.
//
// Specifically rejects:
//   - empty entries
//   - entries with control characters (already done above, but kept here for
//     defence-in-depth)
//   - entries that do not parse to a non-opaque url::Origin
//   - entries whose scheme is not http/https/ws/wss/file/ftp (the set Blink
//     treats as security origins for Canvas)
//
// Returns the canonical url::Origin->Serialize() string, NOT the raw entry.
// Special case: "any" / "*" -> sentinel value "" (empty string) to signal
// wildcard-all origins. IsOriginAllowed() must handle this sentinel.
std::optional<std::string> ParseOriginEntry(const std::string& entry) {
  // Wildcard sentinel: "any" or "*" means match all origins.
  if (entry == "any" || entry == "*") {
    return std::make_optional(std::string{});  // empty string = wildcard sentinel
  }

  // url::Origin::Create() with an empty URL yields an opaque origin, which
  // is invalid for our purposes.
  const url::Origin origin = url::Origin::Create(GURL(entry));
  if (origin.opaque()) {
    LOG(ERROR) << "[CANVAS-TEST-ERROR] origin does not parse to a non-opaque "
                  "url::Origin: '"
               << entry << "'; ignoring";
    return std::nullopt;
  }

  const std::string scheme = origin.scheme();
  static const char* const kAllowedSchemes[] = {
      "http", "https", "ws", "wss", "file", "ftp"};
  bool scheme_ok = false;
  for (const char* allowed : kAllowedSchemes) {
    if (scheme == allowed) {
      scheme_ok = true;
      break;
    }
  }
  if (!scheme_ok) {
    LOG(ERROR) << "[CANVAS-TEST-ERROR] origin scheme not allowed: '" << scheme
               << "' (in entry '" << entry << "'); ignoring";
    return std::nullopt;
  }

  // url::Origin->Serialize() produces the canonical "scheme://host[:port]"
  // string with default ports stripped. We compare on this so that
  // https://example.com and https://example.com:443 collapse to the same key.
  std::string canonical = origin.Serialize();
  // url::Origin->Serialize() for opaque returns "null"; double-check.
  if (canonical.empty() || canonical == "null") {
    LOG(ERROR) << "[CANVAS-TEST-ERROR] origin serialized to empty/null: '"
               << entry << "'; ignoring";
    return std::nullopt;
  }
  return canonical;
}

}  // namespace

CanvasTestSessionSeedManager* CanvasTestSessionSeedManager::GetInstance() {
  // P2-2 2026-07-25: wrap in base::NoDestructor to avoid the implicit
  // exit-time destructor (-Wexit-time-destructors). The friend declaration
  // in the header is required because the constructor is private.
  static base::NoDestructor<CanvasTestSessionSeedManager> instance;
  return instance.get();
}

void CanvasTestSessionSeedManager::InitializeFromCommandLine() {
  base::AutoLock lock(lock_);
  EnsureInitializedLocked();
}

void CanvasTestSessionSeedManager::EnsureInitializedLocked() {
  if (initialized_) {
    fprintf(stderr, "[DBG-INIT pid=%lu] already initialized (lazy)\n", static_cast<unsigned long>(base::GetCurrentProcId())); fflush(stderr);
    return;
  }

  const base::CommandLine& cmdline = *base::CommandLine::ForCurrentProcess();
  fprintf(stderr, "[DBG-INIT pid=%lu] ENTER process_type='%s' has_noise_enabled=%d has_noise_legacy=%d has_origins_new=%d has_origins_legacy=%d\n",
          static_cast<unsigned long>(base::GetCurrentProcId()),
          base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII("type").c_str(),
          cmdline.HasSwitch("canvas-test-noise-enabled") ? 1 : 0,
          cmdline.HasSwitch("dchromium-fork-canvas-test-noise-enabled") ? 1 : 0,
          cmdline.HasSwitch("canvas-test-noise-origins") ? 1 : 0,
          cmdline.HasSwitch("dchromium-fork-canvas-test-noise-allowed-origins") ? 1 : 0); fflush(stderr);

  // 1. enabled flag (P1-2 strict priority).
  const int enable_state = ResolveEnabledState(cmdline);
  config_.enabled = (enable_state == 1);
  fprintf(stderr, "[DBG-INIT] enable_state=%d config_.enabled=%d\n", enable_state, config_.enabled ? 1 : 0); fflush(stderr);

  if (!config_.enabled) {
    initialized_ = true;
    return;
  }

  // 2. origin allowlist (required).
  std::string origins_csv;
  if (cmdline.HasSwitch(kSwOrigins)) {
    origins_csv = cmdline.GetSwitchValueASCII(kSwOrigins);
  } else if (cmdline.HasSwitch(kLegacyOrigins)) {
    origins_csv = cmdline.GetSwitchValueASCII(kLegacyOrigins);
    LOG(WARNING) << "[CANVAS-TEST-INFO] using legacy switch '"
                 << kLegacyOrigins
                 << "', prefer --canvas-test-noise-origins";
  }

  if (origins_csv.empty()) {
    config_.enabled = false;
    fprintf(stderr, "[DBG-INIT] origins_csv empty, disabling\n"); fflush(stderr);
    LOG(ERROR) << "[CANVAS-TEST-ERROR] canvas-test-noise is enabled but no "
                  "allowed origins were specified (--canvas-test-noise-origins); "
                  "disabling noise path";
    initialized_ = true;
    return;
  }

  fprintf(stderr, "[DBG-INIT] origins_csv='%s'\n", origins_csv.c_str()); fflush(stderr);
  ParseAllowedOriginsLocked(origins_csv);
  if (config_.allowed_origins.empty()) {
    config_.enabled = false;
    fprintf(stderr, "[DBG-INIT] allowed_origins parsed empty, disabling\n"); fflush(stderr);
    LOG(ERROR) << "[CANVAS-TEST-ERROR] canvas-test-noise origin allowlist "
                  "parsed to empty list; disabling noise path";
    initialized_ = true;
    return;
  }
  fprintf(stderr, "[DBG-INIT] allowed_origins count=%zu\n", config_.allowed_origins.size()); fflush(stderr);

  // 3. session seed (priority: fixed override > legacy override > random).
  bool overridden = false;
  config_.session_seed = DetermineSessionSeedLocked(&overridden);
  config_.seed_overridden_by_cli = overridden;

  if (config_.session_seed == 0 && overridden) {
    // 0 is a sentinel for "explicit zero from CLI" - we accept it but
    // log a warning so operators notice the suspicious choice.
    LOG(WARNING) << "[CANVAS-TEST-INFO] session_seed=0 was supplied "
                    "explicitly via CLI; entropy is minimal";
  }

  // 4. algorithm version (defaults to 1 when missing/invalid).
  std::string version_str;
  if (cmdline.HasSwitch(kSwVersion)) {
    version_str = cmdline.GetSwitchValueASCII(kSwVersion);
  }
  if (!version_str.empty()) {
    uint32_t version = 0;
    if (base::StringToUint(version_str, &version) && version > 0 &&
        version <= 32) {
      config_.algorithm_version = version;
    } else {
      LOG(WARNING) << "[CANVAS-TEST-INFO] invalid --canvas-test-noise-version='"
                   << version_str << "', defaulting to 1";
      config_.algorithm_version = 1;
    }
  }

  // 5. metadata.
  config_.browser_pid = static_cast<int64_t>(base::GetCurrentProcId());
  config_.create_time_ms =
      base::Time::Now().InMillisecondsSinceUnixEpoch();

  initialized_ = true;
  fprintf(stderr, "[DBG-INIT pid=%lu] DONE initialized=true, enabled=%d, allowed_origins=%zu, session_seed=0x%llx\n",
          static_cast<unsigned long>(base::GetCurrentProcId()),
          config_.enabled ? 1 : 0, config_.allowed_origins.size(), (unsigned long long)config_.session_seed); fflush(stderr);
  WriteStructuredLogLocked();
}

void CanvasTestSessionSeedManager::ParseAllowedOriginsLocked(
    const std::string& origins_csv) {
  config_.allowed_origins.clear();
  const auto parts = base::SplitStringPiece(
      origins_csv, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  config_.allowed_origins.reserve(parts.size());
  for (const auto part : parts) {
    const std::string entry(part);
    // P2-2 2026-07-25: control-char check rejects the WHOLE entry.
    // The previous implementation used `continue` inside the inner
    // for-loop, which only skipped the offending byte and kept checking
    // the rest of the same entry - so an origin with embedded \x01 would
    // slip through to url::Origin::Create below.
    bool reject_entry = false;
    for (char c : entry) {
      if (c < 0x20 || c == 0x7F) {
        reject_entry = true;
        break;
      }
    }
    if (reject_entry) {
      LOG(ERROR) << "[CANVAS-TEST-ERROR] origin contains control char: '"
                 << entry << "'; ignoring";
      continue;
    }
    std::optional<std::string> canonical = ParseOriginEntry(entry);
    if (!canonical) {
      continue;
    }
    config_.allowed_origins.emplace_back(std::move(*canonical));
  }
}

uint64_t CanvasTestSessionSeedManager::DetermineSessionSeedLocked(
    bool* out_was_overridden) {
  *out_was_overridden = false;
  const base::CommandLine& cmdline = *base::CommandLine::ForCurrentProcess();

  // Phase 1 (legacy, kept for back-compat):
  //   --canvas-fixed-seed=<uint64>            (newer legacy CLI)
  //   --dchromium-fork-canvas-test-noise-seed=<uint64>   (older legacy CLI)
  // If present and parses, use verbatim. Mark overridden=true so the
  // operator sees "explicit" in the structured log. n=0 is allowed here
  // (existing semantics) but emits a warning.
  std::string fixed_seed_str;
  if (cmdline.HasSwitch(kSwFixedSeed)) {
    fixed_seed_str = cmdline.GetSwitchValueASCII(kSwFixedSeed);
  } else if (cmdline.HasSwitch(kLegacySeed)) {
    fixed_seed_str = cmdline.GetSwitchValueASCII(kLegacySeed);
    LOG(WARNING) << "[CANVAS-TEST-INFO] using legacy switch '"
                 << kLegacySeed
                 << "', prefer --canvas-fixed-seed";
  }
  if (!fixed_seed_str.empty()) {
    uint64_t parsed = 0;
    if (base::StringToUint64(fixed_seed_str, &parsed)) {
      *out_was_overridden = true;
      config_.seed_source = "legacy_cli";
      config_.seed_from_cli = true;
      return parsed;
    }
    // Invalid -> fall through to anti-fraud store, but keep enabled=true
    // so the operator notices the random seed in the structured log.
    LOG(ERROR) << "[CANVAS-TEST-ERROR] invalid --canvas-fixed-seed='"
               << fixed_seed_str
               << "', falling back to anti-fraud store";
  }

  // Phase 2 (2026-07-26): canvas anti-fraud seed store.
  //
  // Precedence:
  //   1. --canvas-anti-fraud-seed=<n> CLI override (n != 0).
  //        n == 0 falls through (matches --canvas-fixed-seed=0 semantics).
  //   2. <user_data_dir>/canvas_anti_fraud_seed.json (persisted).
  //   3. base::RandUint64() and write JSON atomically.
  base::FilePath user_data_dir;
  if (cmdline.HasSwitch(switches::kForkUserDataDir)) {
    user_data_dir =
        cmdline.GetSwitchValuePath(switches::kForkUserDataDir);
  }
  fprintf(stderr, "[DBG-RESOLVE] kForkUserDataDir='%s' HasSwitch=%d user_data_dir empty=%d\n",
          switches::kForkUserDataDir,
          cmdline.HasSwitch(switches::kForkUserDataDir) ? 1 : 0,
          user_data_dir.empty() ? 1 : 0);
  fflush(stderr);

  bool caf_present = cmdline.HasSwitch(switches::kForkCanvasAntiFraudSeed);
  bool caf_valid = false;
  uint64_t caf_seed = 0;
  if (caf_present) {
    const std::string caf_seed_str =
        cmdline.GetSwitchValueASCII(switches::kForkCanvasAntiFraudSeed);
    if (!base::StringToUint64(caf_seed_str, &caf_seed)) {
      // Parse failure: log loud and unambiguous, then fall through to
      // persistence / Rand. Critical: do NOT collapse to 0, because 0
      // would short-circuit the xorshift64 PRNG and produce constant
      // noise (which would itself be a fingerprintable signature).
      LOG(ERROR) << "[canvas-anti-fraud-seed] invalid --canvas-anti-fraud-seed='"
                 << caf_seed_str << "', falling back to persistence/Rand";
      caf_valid = false;
    } else {
      caf_valid = true;
    }
  }

  const CanvasAntiFraudSeedResolution caf_res =
      CanvasAntiFraudSeedStore::Resolve(user_data_dir, caf_present,
                                        caf_valid, caf_seed);

  // Diagnostics: help trace which resolution branch fired when
  // investigating "fingerprint drifted across restart" / "two profiles
  // share a fingerprint" issues.
  DVLOG(1) << "[canvas-anti-fraud-seed] resolve: cli="
           << caf_res.from_cli
           << " persisted=" << caf_res.from_persistence
           << " generated=" << caf_res.newly_generated
           << " write_failed=" << caf_res.write_back_failed
           << " seed=0x" << std::hex << caf_res.seed;

  config_.seed_from_cli = caf_res.from_cli;
  config_.seed_from_persistence = caf_res.from_persistence;
  config_.seed_newly_generated = caf_res.newly_generated;
  config_.seed_write_back_failed = caf_res.write_back_failed;
  config_.seed_source = caf_res.from_cli       ? "cli"
                      : caf_res.from_persistence ? "json"
                      :                             "random";

  fprintf(stderr, "[DBG-RESOLVE] seed=0x%llx from_cli=%d from_persistence=%d new=%d write_failed=%d source=%s\n",
          (unsigned long long)caf_res.seed,
          caf_res.from_cli ? 1 : 0,
          caf_res.from_persistence ? 1 : 0,
          caf_res.newly_generated ? 1 : 0,
          caf_res.write_back_failed ? 1 : 0,
          config_.seed_source.c_str());
  fflush(stderr);

  *out_was_overridden = caf_res.from_cli;
  return caf_res.seed;
}

bool CanvasTestSessionSeedManager::IsEnabled() const {
  base::AutoLock lock(lock_);
  // Lazy init (2026-07-27): in component build, content.dll and
  // chrome.dll each have their own per-DLL singleton. The first
  // IsEnabled() call from chrome.dll (e.g. inside Blink's
  // ApplyCanvasReadbackNoise) may hit a fresh instance that has not
  // been initialized by content.dll's content_main_runner_impl.cc.
  // EnsureInitializedLocked() parses the same command-line switches
  // and configures the singleton in-place. Idempotent across both
  // DLLs because both read the same command line.
  const_cast<CanvasTestSessionSeedManager*>(this)->EnsureInitializedLocked();
  const bool enabled = config_.enabled && initialized_ && !config_.allowed_origins.empty();
  fprintf(stderr, "[DBG-ISE pid=%lu init=%p] initialized=%d enabled=%d allowed_origins=%zu -> IsEnabled=%d\n", static_cast<unsigned long>(base::GetCurrentProcId()), (void*)&initialized_, initialized_ ? 1 : 0, config_.enabled ? 1 : 0, config_.allowed_origins.size(), enabled ? 1 : 0); fflush(stderr);
  return enabled;
}

CanvasTestConfig CanvasTestSessionSeedManager::GetConfig() const {
  base::AutoLock lock(lock_);
  return config_;
}

std::vector<std::string> CanvasTestSessionSeedManager::GetAllowedOrigins()
    const {
  base::AutoLock lock(lock_);
  return config_.allowed_origins;
}

bool CanvasTestSessionSeedManager::IsOriginAllowed(
    const std::string& origin) const {
  base::AutoLock lock(lock_);
  const_cast<CanvasTestSessionSeedManager*>(this)->EnsureInitializedLocked();
  if (!config_.enabled) return false;
  if (config_.allowed_origins.empty()) return false;
  // Wildcard sentinel: empty string in allowed_origins means "allow all".
  // This is set when --canvas-test-noise-origins contains "any" or "*".
  for (const auto& allowed : config_.allowed_origins) {
    if (allowed.empty()) return true;  // wildcard sentinel
  }
  // Phase P1-1 (2026-07-25): caller's origin must be the url::Origin
  // serialization produced by Blink (i.e. the same canonical form used
  // when the allowlist was parsed). We still parse it through url::Origin
  // so that https://Foo.test and https://foo.test collapse to the same
  // key. If parsing fails (opaque or invalid scheme), the origin is not
  // allowed.
  const url::Origin parsed = url::Origin::Create(GURL(origin));
  if (parsed.opaque()) {
    return false;
  }
  const std::string canonical = parsed.Serialize();
  for (const auto& allowed : config_.allowed_origins) {
    if (!allowed.empty() && allowed == canonical) return true;
  }
  return false;
}

bool CanvasTestSessionSeedManager::IsSeedOverriddenByCli() const {
  base::AutoLock lock(lock_);
  return config_.seed_overridden_by_cli;
}

void CanvasTestSessionSeedManager::LogSessionSeedStructured() const {
  base::AutoLock lock(lock_);
  WriteStructuredLogLocked();
}

void CanvasTestSessionSeedManager::WriteStructuredLogLocked() const {
  // Single-line JSON for grep-friendliness.
  // P2-2 2026-07-25: Chromium 147 uses base::DictValue (top-level class in
  // base/values.h). The nested alias base::Value::Dict does NOT exist in
  // this version - it's the newer-upstream name. The JSONWriter::Write
  // overload accepts base::DictValue& directly.
  base::DictValue entry;
  entry.Set("event", "canvas_test_seed");
  entry.Set("session_seed", base::NumberToString(config_.session_seed));
  entry.Set("algorithm_version",
            static_cast<int>(config_.algorithm_version));
  entry.Set("browser_pid", static_cast<int>(config_.browser_pid));
  entry.Set("create_time_ms", static_cast<double>(config_.create_time_ms));
  entry.Set("override", config_.seed_overridden_by_cli);
  entry.Set("origins_count",
            static_cast<int>(config_.allowed_origins.size()));

  std::string json;
  base::JSONWriter::Write(entry, &json);
  // INFO so it shows up in normal logs but doesn't trigger alerts.
  LOG(INFO) << "[CANVAS-TEST-SEED] " << json;
}

bool IsCanvasTestNoiseEnabled() {
  return CanvasTestSessionSeedManager::GetInstance()->IsEnabled();
}

}  // namespace chromium_fork
