// Copyright 2026 Dchromium_fork
//
// CanvasAntiFraudSeedStore: persistence layer for the canvas anti-fraud seed.
//
// Rationale:
//   The canvas anti-fraud algorithm in canvas_anti_fraud.cc derives its
//   per-pixel LSB distribution from a single uint64_t "session seed". To
//   meet the cross-process stability requirement:
//     * within a single process   -> seed MUST be stable (F5 reload etc.)
//     * across processes          -> seed MAY vary when the user wants
//                                    variation, and SHOULD persist so that
//                                    the same browser profile retains the
//                                    same fingerprint across restarts by
//                                    default.
//
// We persist the seed in <user_data_dir>/canvas_anti_fraud_seed.json,
// separate from the retired VEM render seed. This store belongs only to the
// Canvas test-noise path and must not control production rendering.
//
// File format (manual JSON, base/json_value compatible):
//
//   {
//     "schema_version": 1,
//     "render_seed": "<decimal-as-string>",
//     "created_ms": 1721904000000
//   }
//
// Resolution precedence (from high to low):
//   1. CLI present && parses && n != 0 : CLI override (use n).
//                                          n == 0 is treated as "explicit
//                                          disable of CLI override" and
//                                          falls through to JSON / Rand.
//                                          This matches the existing
//                                          --canvas-fixed-seed=0 semantics
//                                          in canvas_session_seed_manager.cc.
//   2. <user_data_dir>/canvas_anti_fraud_seed.json : on-disk value.
//   3. base::RandUint64() + write JSON atomically.
//
// Boundary compliance (Chromium开发规则.md §⚠️ 关键约束):
//   - Does NOT touch Blink.
//   - Does NOT modify any readback path.
//   - Does NOT depend on global mutable state outside |user_data_dir|.

#ifndef SRC_CHROMIUM_FORK_CANVAS_ANTI_FRAUD_SEED_STORE_H_
#define SRC_CHROMIUM_FORK_CANVAS_ANTI_FRAUD_SEED_STORE_H_

#include <cstdint>

#include "base/files/file_path.h"

namespace chromium_fork {

struct CanvasAntiFraudSeedStoreConfig {
  // Filename relative to |user_data_dir|. Sibling of render_seed.json.
  static constexpr char kFileName[] = "canvas_anti_fraud_seed.json";

  // Bump when on-disk format changes incompatibly.
  static constexpr uint32_t kSchemaVersion = 1;
};

// Outcome of Resolve().
struct CanvasAntiFraudSeedResolution {
  uint64_t seed = 0;             // Final seed value.
  bool from_cli = false;         // true: --canvas-anti-fraud-seed CLI.
  bool from_persistence = false; // true: hit canvas_anti_fraud_seed.json.
  bool newly_generated = false;  // true: base::RandUint64() + wrote JSON.
  bool write_back_failed = false;// true: tried to persist, failed; in-memory
                                 //       value is still valid.
};

// Resolves the effective canvas anti-fraud seed for this process.
//
// Precedence (highest first):
//   1. cli_present && cli_valid && cli_seed != 0
//        -> use cli_seed verbatim.
//        cli_present && cli_valid && cli_seed == 0 is treated as "fall
//        through" so that the JSON / Rand path governs; this matches
//        the existing 0-sentinel semantics in --canvas-fixed-seed.
//   2. !case 1 -> user_data_dir/canvas_anti_fraud_seed.json parses
//                 -> its value.
//   3. else    -> base::RandUint64() and write JSON.
//
// The split between cli_present / cli_valid / cli_seed == 0 is critical
// to keep "explicit disable" distinguishable from "garbage input" and
// "intentional CLI override of 0".
//
// Thread safety: caller must invoke from the Browser main process only.
// Renderer / GPU children inherit the value via the standard VEM
// AppendSwitchASCII mechanism (NOT through this file directly), avoiding
// multi-process races on a single JSON file without flock.
class CanvasAntiFraudSeedStore {
 public:
  // Public for unit testing; production callers use Resolve().
  static bool ReadFromFile(const base::FilePath& user_data_dir,
                           uint64_t* out_seed);

  // Public for unit testing; production callers use Resolve().
  // Writes a new JSON file atomically (write to *.tmp.<pid>, then rename).
  // Returns true on success.
  static bool WriteToFile(const base::FilePath& user_data_dir,
                          uint64_t seed);

  // Main entry point.
  //   user_data_dir  : Chrome profile directory (Chromium-native).
  //   cli_present    : whether --canvas-anti-fraud-seed was present
  //                    on cmdline.
  //   cli_valid      : whether the parsed string is a valid uint64.
  //                    cli_present && !cli_valid means "user passed this
  //                    switch but the value is malformed"; the caller
  //                    MUST log and fall through.
  //   cli_seed       : parsed uint64 value of the CLI switch; only
  //                    inspected when cli_present && cli_valid.
  static CanvasAntiFraudSeedResolution Resolve(
      const base::FilePath& user_data_dir,
      bool cli_present,
      bool cli_valid,
      uint64_t cli_seed);
};

}  // namespace chromium_fork

#endif  // SRC_CHROMIUM_FORK_CANVAS_ANTI_FRAUD_SEED_STORE_H_
