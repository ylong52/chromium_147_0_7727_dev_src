// Copyright 2026 Dchromium_fork

#include "chromium_fork/canvas_anti_fraud_seed_store.h"

#include <cstdint>
#include <string>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/process/process_handle.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "chromium_fork/switches.h"

namespace chromium_fork {

namespace {

// Parse a JSON object with at least {"render_seed": "<decimal-as-string>"}
// and an optional "schema_version" + "created_ms". Returns nullopt on any
// parse error.
std::optional<uint64_t> ParseJsonObject(const std::string& text) {
  auto result = base::JSONReader::ReadAndReturnValueWithError(
      text, base::JSON_PARSE_RFC);
  if (!result.has_value() || !result->is_dict()) {
    return std::nullopt;
  }
  const auto& dict = result->GetDict();

  // schema_version check (soft: only reject on forward-incompatible).
  const auto* schema_v = dict.Find("schema_version");
  if (schema_v && schema_v->is_int()) {
    int v = schema_v->GetInt();
    if (v > static_cast<int>(CanvasAntiFraudSeedStoreConfig::kSchemaVersion)) {
      // Forward-incompatible; treat as missing.
      return std::nullopt;
    }
  }

  const auto* seed_val = dict.Find("render_seed");
  if (!seed_val || !seed_val->is_string()) {
    return std::nullopt;
  }
  uint64_t out = 0;
  if (!base::StringToUint64(seed_val->GetString(), &out)) {
    return std::nullopt;
  }
  return out;
}

base::FilePath PathFor(const base::FilePath& user_data_dir) {
  return user_data_dir.AppendASCII(
      CanvasAntiFraudSeedStoreConfig::kFileName);
}

}  // namespace

// static
bool CanvasAntiFraudSeedStore::ReadFromFile(
    const base::FilePath& user_data_dir, uint64_t* out_seed) {
  if (user_data_dir.empty()) {
    return false;
  }
  const base::FilePath path = PathFor(user_data_dir);
  std::string text;
  if (!base::ReadFileToString(path, &text)) {
    return false;
  }
  auto parsed = ParseJsonObject(text);
  if (parsed.has_value()) {
    *out_seed = *parsed;
    return true;
  }
  // Parse failed - preserve the corrupted file for diagnostics. Renaming
  // to <name>.bak keeps the original bytes on disk so engineers can
  // triage "which field was malformed" without rerunning the trigger.
  // Best-effort: if Move fails (e.g. permission denied) we still return
  // false; the next Resolve() will regenerate the seed in JSON shape.
  const base::FilePath bak_path = path.AddExtensionASCII(".bak");
  // Don't overwrite an existing .bak (every previous failure would
  // otherwise be lost). Resolve() will regenerate the new JSON file in
  // place on its own success path.
  if (!base::PathExists(bak_path)) {
    base::Move(path, bak_path);
  } else {
    base::DeleteFile(path);
  }
  return false;
}

// static
bool CanvasAntiFraudSeedStore::WriteToFile(
    const base::FilePath& user_data_dir, uint64_t seed) {
  if (user_data_dir.empty()) {
    return false;
  }
  if (!base::DirectoryExists(user_data_dir)) {
    if (!base::CreateDirectory(user_data_dir)) {
      return false;
    }
  }

  // Chromium 147: base::DictValue (top-level class). JSONWriter::Write
  // accepts base::DictValue& via implicit conversion to base::ValueView.
  base::DictValue dict;
  dict.Set("schema_version",
           static_cast<int>(CanvasAntiFraudSeedStoreConfig::kSchemaVersion));
  dict.Set("render_seed", base::NumberToString(seed));
  dict.Set("created_ms", static_cast<double>(
                             base::Time::Now().InMillisecondsSinceUnixEpoch()));

  std::string text;
  base::JSONWriter::Write(dict, &text);

  const base::FilePath final_path = PathFor(user_data_dir);
  // Include PID in the temp filename to avoid clobbering between two
  // browser processes that race to first-touch the same user_data_dir.
  const std::string suffix =
      ".tmp." + base::NumberToString(base::GetCurrentProcId());
  const base::FilePath tmp_path = final_path.AddExtensionASCII(suffix);

  // Atomic-ish: write tmp then rename. base::WriteFile uses platform
  // best-effort atomic semantics for small files; acceptable here.
  if (!base::WriteFile(tmp_path, text)) {
    return false;
  }
  if (!base::Move(tmp_path, final_path)) {
    base::DeleteFile(tmp_path);
    return false;
  }
  return true;
}

// static
CanvasAntiFraudSeedResolution CanvasAntiFraudSeedStore::Resolve(
    const base::FilePath& user_data_dir,
    bool cli_present,
    bool cli_valid,
    uint64_t cli_seed) {
  CanvasAntiFraudSeedResolution out;

  // Priority 0 (2026-07-27): --canvas-anti-fraud-new-session-seed.
  // When present, SKIP the persistent JSON read entirely and always
  // generate a fresh random seed. The generated seed is NOT written back
  // to disk (WriteToFile is suppressed), so every Chrome launch gets a
  // new random seed — breaking cross-restart fingerprint correlation.
  //
  // Implementation note: Resolve() is called from the Browser main process
  // via VirtualEnvironmentManager::InitFromCommandLine, which already
  // parsed the switch. We check it HERE (not in the caller) to keep the
  // API surface minimal — callers just pass cli_present/cli_valid/cli_seed
  // as before. The new_session flag is read from the CommandLine directly
  // to avoid changing the Resolve() signature.
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kForkCanvasAntiFraudNewSessionSeed)) {
    out.seed = base::RandUint64();
    out.newly_generated = true;
    out.write_back_failed = false;  // Suppressed intentionally.
    DVLOG(1) << "[canvas-anti-fraud-seed] new-session-seed mode; "
                "forced random, no disk write";
    return out;
  }

  // Priority 1: explicit CLI override. cli_seed == 0 is the sentinel
  // for "explicit disable of CLI override" and falls through to JSON /
  // Rand so that 0 does not silently become a real seed (which would
  // mean xorshift64 never advances and produces deterministic noise).
  if (cli_present && cli_valid && cli_seed != 0) {
    out.seed = cli_seed;
    out.from_cli = true;
    return out;
  }
  if (cli_present && cli_valid && cli_seed == 0) {
    LOG(WARNING) << "[canvas-anti-fraud-seed] CLI value is 0; "
                    "falling back to JSON / Rand";
  }

  // Priority 2: persistent JSON. If user_data_dir is empty we cannot
  // even attempt; fall through to Priority 3 (in-memory Rand) so a
  // Browser invocation without --user-data-dir still has a deterministic
  // seed for the duration of the session.
  if (!user_data_dir.empty()) {
    uint64_t persisted = 0;
    if (ReadFromFile(user_data_dir, &persisted)) {
      out.seed = persisted;
      out.from_persistence = true;
      return out;
    }
  }

  // Priority 3: generate in-memory. NEVER write to disk when there
  // is no user_data_dir; a write attempt would just fail and produce
  // an unnecessary write_back_failed diagnostic.
  out.seed = base::RandUint64();
  out.newly_generated = true;
  if (!user_data_dir.empty()) {
    if (!WriteToFile(user_data_dir, out.seed)) {
      out.write_back_failed = true;
      // In-memory seed is still valid; the next Resolve() will retry
      // the write.
      DVLOG(1) << "[canvas-anti-fraud-seed] failed to write "
               << CanvasAntiFraudSeedStoreConfig::kFileName
               << " to user_data_dir; using in-memory seed only";
    }
  } else {
    DVLOG(1) << "[canvas-anti-fraud-seed] no user_data_dir; "
                "in-memory seed only";
  }
  return out;
}

}  // namespace chromium_fork