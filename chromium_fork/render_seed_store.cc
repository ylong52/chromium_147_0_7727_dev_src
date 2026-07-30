// Copyright 2026 Dchromium_fork
//
// RenderSeedStore: historical test-only persistence fixture.
// It is intentionally not used by VEM. Canvas test noise owns its seed through
// CanvasTestSessionSeedManager, while production rendering remains unchanged.
//
// See:
//   - Docs/fingerprint/render_fingerprint_integration_plan.md (test-only)
//   - chromium_fork/BUILD.gn (visibility = test targets only)

#include "chromium_fork/render_seed_store.h"

#include <cstdint>
#include <string>

#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/process/process_handle.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"

namespace chromium_fork {

namespace {

// Parse a JSON object with at least {"render_seed": "<decimal-as-string>"}
// and an optional "schema_version" + "created_ms".
// On any parse error returns false.
bool ParseJsonObject(const std::string& text, uint64_t* out_seed) {
  auto result = base::JSONReader::ReadAndReturnValueWithError(
      text, base::JSON_PARSE_RFC);
  if (!result.has_value() || !result->is_dict()) {
    return false;
  }
  // Chromium 147: Value::GetDict() returns base::DictValue& (top-level
  // class in base/values.h). The nested alias base::Value::Dict does NOT
  // exist in this version - it's the newer-upstream name. See
  // canvas_session_seed_manager.cc::WriteStructuredLogLocked() for a
  // parallel pair that confirms the same constraint.
  const auto& dict = result->GetDict();

  // schema_version check (soft: only reject on forward-incompatible).
  const auto* schema_v = dict.Find("schema_version");
  if (schema_v && schema_v->is_int()) {
    int v = schema_v->GetInt();
    if (v > static_cast<int>(RenderSeedStoreConfig::kSchemaVersion)) {
      // Forward-incompatible; treat as missing.
      return false;
    }
  }

  const auto* seed_val = dict.Find("render_seed");
  if (!seed_val || !seed_val->is_string()) {
    return false;
  }
  return base::StringToUint64(seed_val->GetString(), out_seed);
}

base::FilePath PathFor(const base::FilePath& user_data_dir) {
  return user_data_dir.AppendASCII(RenderSeedStoreConfig::kFileName);
}

}  // namespace

// static
bool RenderSeedStore::ReadFromFile(const base::FilePath& user_data_dir,
                                   uint64_t* out_seed) {
  if (user_data_dir.empty()) {
    return false;
  }
  const base::FilePath path = PathFor(user_data_dir);
  std::string text;
  if (!base::ReadFileToString(path, &text)) {
    return false;
  }
  if (ParseJsonObject(text, out_seed)) {
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
bool RenderSeedStore::WriteToFile(const base::FilePath& user_data_dir,
                                  uint64_t seed) {
  if (user_data_dir.empty()) {
    return false;
  }
  if (!base::DirectoryExists(user_data_dir)) {
    if (!base::CreateDirectory(user_data_dir)) {
      return false;
    }
  }

  // Chromium 147: base::DictValue (top-level class). The nested alias
  // base::Value::Dict does NOT exist in this version. JSONWriter::Write
  // accepts base::DictValue& via implicit conversion to base::ValueView.
  base::DictValue dict;
  dict.Set("schema_version",
           static_cast<int>(RenderSeedStoreConfig::kSchemaVersion));
  dict.Set("render_seed", base::NumberToString(seed));
  dict.Set("created_ms", static_cast<double>(
                             base::Time::Now().InMillisecondsSinceUnixEpoch()));

  std::string text;
  base::JSONWriter::Write(dict, &text);

  const base::FilePath final_path = PathFor(user_data_dir);
  // Include PID in the temp filename to avoid clobbering between two
  // browser processes that race to first-touch the same user_data_dir.
  // base::GetCurrentProcId() is available via base/process/process_handle.h
  // which we get transitively through virtual_environment_manager.cc's
  // include chain. Stand-alone callers (unit tests) include this .cc
  // directly so we add the explicit include below.
  const std::string suffix =
      ".tmp." + base::NumberToString(base::GetCurrentProcId());
  const base::FilePath tmp_path = final_path.AddExtensionASCII(suffix);

  // Atomic-ish: write tmp then rename. base::WriteFile uses platform best-
  // effort atomic semantics for small files; acceptable for this use case.
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
RenderSeedResolution RenderSeedStore::Resolve(
    const base::FilePath& user_data_dir,
    bool cli_present,
    bool cli_valid,
    uint64_t cli_seed) {
  RenderSeedResolution out;

  // Priority 1a: explicit CLI override (valid parse, non-zero value).
  // Priority 1b: explicit CLI disable (valid parse, zero value).
  //
  // Both branches short-circuit persistence / generation. The caller
  // MUST have already logged when cli_present && !cli_valid so we don't
  // duplicate the error here.
  if (cli_present && cli_valid) {
    out.seed = cli_seed;
    out.from_cli = true;
    return out;
  }

  // Priority 2: persistent JSON. If user_data_dir is empty we cannot
  // even attempt; fall through to Priority 3 (in-memory Rand) so a
  // Browser invocation without --user-data-dir is still deterministic
  // for the duration of the session.
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
      // In-memory seed is still valid; the next Resolve() will retry write.
      DVLOG(1) << "[render-seed] failed to write "
               << RenderSeedStoreConfig::kFileName
               << " to user_data_dir; using in-memory seed only";
    }
  } else {
    DVLOG(1) << "[render-seed] no user_data_dir; in-memory seed only";
  }
  return out;
}

}  // namespace chromium_fork
