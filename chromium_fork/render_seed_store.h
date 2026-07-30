// Copyright 2026 Dchromium_fork
//
// RenderSeedStore: historical test-only persistence fixture.
//
// Rationale:
//   The render-fingerprint sub-parameters (extension order, viewport bias,
//   max-texture offset, etc.) are derived from a single uint64_t "render
//   seed". To keep the per-profile fingerprint stable across browser
//   restarts AND different across profiles, the seed must be:
//
//   1. Persisted to disk (otherwise restart changes the fingerprint).
//   2. Tied to the profile directory (otherwise switching profiles loses
//      differentiation).
//
// We persist the seed in <user_data_dir>/render_seed.json, which is the
// same directory Chromium uses for cookies / cache / IndexedDB / history
// for that profile. Each VEM profile -> its own user_data_dir -> its own
// render_seed.json -> its own seed.
//
// File format (manual JSON, no dependency on base/json_value):
//
//   {
//     "schema_version": 1,
//     "render_seed": "12345678901234567890",
//     "created_ms": 1721904000000
//   }
//
// Rules:
//   - schema_version mismatches cause the file to be re-generated (not
//     a hard error; the old seed is discarded).
//   - On corruption, the file is renamed to *.bak and a fresh one is
//     generated.
//   - Write is best-effort: failure is logged but never crashes; the
//     in-memory seed is still usable.
//   - Tests may read and write the fixture in an isolated temporary directory.
//     VEM and renderer/GPU processes do not use this file.
//
// Boundary compliance (Chromium开发规则.md §⚠️ 关键约束):
//   - Does NOT touch Blink.
//   - Does NOT modify any readback path.
//   - Does NOT depend on global mutable state outside |user_data_dir|.
//
// This file is independent of canvas_noise_engine / SessionSeedManager,
// which serve different concerns (readback-time pixel perturbation, used
// only when ENABLE_CANVAS_TEST_NOISE=1 and an allowlist is configured).

#ifndef SRC_CHROMIUM_FORK_RENDER_SEED_STORE_H_
#define SRC_CHROMIUM_FORK_RENDER_SEED_STORE_H_

#include <cstdint>

#include "base/files/file_path.h"

namespace chromium_fork {

// Persistence location and version policy.
struct RenderSeedStoreConfig {
  // Filename relative to |user_data_dir|.
  static constexpr char kFileName[] = "render_seed.json";

  // Bump when on-disk format changes incompatibly.
  static constexpr uint32_t kSchemaVersion = 1;
};

// Outcome of Resolve().
struct RenderSeedResolution {
  uint64_t seed = 0;             // Final seed value (0 = explicitly disabled).
  bool from_cli = false;         // true: CLI --dchromium-fork-render-seed.
  bool from_persistence = false; // true: hit render_seed.json.
  bool newly_generated = false;  // true: base::RandUint64() + wrote JSON.
  bool write_back_failed = false;// true: tried to persist, failed; in-memory
                                 //       value is still valid.
};

// Resolves the effective render seed for this process.
//
// Precedence (highest first):
//   1. cli_present && cli_valid   -> use cli_seed verbatim.
//        - cli_seed == 0           : explicit disable (force 0).
//        - cli_seed != 0           : CLI override (use cli_seed).
//   2. !cli_present || !cli_valid -> user_data_dir/render_seed.json
//                                    exists and parses -> its value.
//   3. else                       -> base::RandUint64() and write JSON.
//
// The split between cli_present and cli_valid is critical: it lets the
// caller distinguish "user passed --dchromium-fork-render-seed=garbage"
// (cli_present=true, cli_valid=false -> fall through to persistence) from
// "user passed --dchromium-fork-render-seed=0" (cli_present=true,
// cli_valid=true -> force 0). Collapsing either into 0 means
// misconfigurations become indistinguishable from intentional disable.
//
// Thread safety: caller must invoke from the Browser main process only;
// renderer / GPU children inherit the value via VEM command-line wiring
// (AppendSwitchASCII in InitFromProfileJSON), NOT through this file.
class RenderSeedStore {
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
  //   cli_present    : whether --dchromium-fork-render-seed was present
  //                    on cmdline (Chromium distinguishes absent from
  //                    empty-string-valued).
  //   cli_valid      : whether the parsed string is a valid uint64.
  //                    cli_present=true && cli_valid=false means
  //                    "user passed this switch but the value is
  //                    malformed"; the caller MUST log and fall through.
  //   cli_seed       : parsed uint64 value of the CLI switch; only
  //                    inspected when cli_present && cli_valid.
  static RenderSeedResolution Resolve(const base::FilePath& user_data_dir,
                                      bool cli_present,
                                      bool cli_valid,
                                      uint64_t cli_seed);
};

}  // namespace chromium_fork

#endif  // SRC_CHROMIUM_FORK_RENDER_SEED_STORE_H_
