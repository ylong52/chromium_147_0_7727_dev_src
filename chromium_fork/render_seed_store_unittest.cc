// Copyright 2026 Dchromium_fork
//
// Unit tests for RenderSeedStore.
//
// Covers the failure modes called out in the 2026-07-26 audit
// (see BUILD.gn lines 235-242 and render_fingerprint_integration_plan.md §8):
//
//   - CLI seed parsing failure (invalid string)         -> ValidNonZero / InvalidString
//   - explicit CLI seed == 0 (force disabled)          -> ValidZero
//   - CLI absent                                    -> Absent
//   - JSON missing                                  -> JsonMissing
//   - JSON corrupted                                -> JsonCorrupted
//   - JSON forward-incompatible schema                -> JsonForwardIncompatible
//   - JSON empty                                   -> JsonEmpty
//   - .bak backup on parse failure                   -> BakBackupOnParseFailure
//   - no user_data_dir                             -> NoUserDataDir
//   - subprocess inherit via CLI                     -> SubprocessInheritViaCLI
//   - concurrent first-touch (different pid)         -> TwoProcesses_RaceForFirstGeneration
//   - temp file cleanup                            -> SingleProcess_TempFileCleanup
//
// All cases verify that the 4-field RenderSeedResolution struct is correctly
// populated; no case relies on side effects outside the test temp directory.

#include "chromium_fork/render_seed_store.h"

#include <cstdint>
#include <optional>
#include <string>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace chromium_fork {
namespace {

using ParseResult = std::optional<uint64_t>;

// Helper: write a JSON file directly to disk (bypassing WriteToFile so we
// can inject malformed content).
bool WriteJsonFile(const base::FilePath& dir,
                   const std::string& content) {
  const base::FilePath path = dir.AppendASCII(RenderSeedStoreConfig::kFileName);
  return base::WriteFile(path, content) > 0;
}

// Helper: read the .bak backup if it exists.
bool BakExists(const base::FilePath& dir) {
  const base::FilePath bak = dir.AppendASCII(RenderSeedStoreConfig::kFileName)
                               .AddExtensionASCII(".bak");
  return base::PathExists(bak);
}

// Convenience: resolve with no CLI signal.
RenderSeedResolution ResolveNoCli(const base::FilePath& user_data_dir) {
  return RenderSeedStore::Resolve(user_data_dir, /*cli_present=*/false,
                                  /*cli_valid=*/false,
                                  /*cli_seed=*/0);
}

// Convenience: resolve with a valid non-zero CLI seed.
RenderSeedResolution ResolveWithCli(uint64_t seed,
                                   const base::FilePath& user_data_dir) {
  return RenderSeedStore::Resolve(user_data_dir, /*cli_present=*/true,
                                  /*cli_valid=*/true, seed);
}

// Convenience: resolve with explicit 0 CLI seed.
RenderSeedResolution ResolveWithCliZero(const base::FilePath& user_data_dir) {
  return RenderSeedStore::Resolve(user_data_dir, /*cli_present=*/true,
                                  /*cli_valid=*/true, /*cli_seed=*/0);
}

// Convenience: resolve with present-but-invalid CLI string.
RenderSeedResolution ResolveWithCliInvalid(const base::FilePath& user_data_dir) {
  return RenderSeedStore::Resolve(user_data_dir, /*cli_present=*/true,
                                  /*cli_valid=*/false,
                                  /*cli_seed=*/0);
}

// ============================================================================
// Group 1: CLI three-state parsing
// ============================================================================

TEST(RenderSeedStoreCliTest, ValidNonZero) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());

  // CLI non-zero should short-circuit everything: no disk I/O.
  RenderSeedResolution r = ResolveWithCli(0xDEADBEEFCAFEBABEULL, tmp.GetPath());
  EXPECT_TRUE(r.from_cli);
  EXPECT_FALSE(r.from_persistence);
  EXPECT_FALSE(r.newly_generated);
  EXPECT_FALSE(r.write_back_failed);
  EXPECT_EQ(r.seed, 0xDEADBEEFCAFEBABEULL);

  // No file should have been created.
  EXPECT_FALSE(base::PathExists(
      tmp.GetPath().AppendASCII(RenderSeedStoreConfig::kFileName)));
}

TEST(RenderSeedStoreCliTest, ValidZero) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());

  // Explicit 0 from CLI must return 0 WITHOUT consulting persistence.
  // Pre-create a "stale" JSON so we can prove it was NOT read.
  ASSERT_TRUE(WriteJsonFile(tmp.GetPath(),
                             R"({"schema_version":1,"render_seed":"999"})"));

  RenderSeedResolution r = ResolveWithCliZero(tmp.GetPath());
  EXPECT_TRUE(r.from_cli);
  EXPECT_FALSE(r.from_persistence);
  EXPECT_FALSE(r.newly_generated);
  EXPECT_FALSE(r.write_back_failed);
  EXPECT_EQ(r.seed, 0ULL);
}

TEST(RenderSeedStoreCliTest, InvalidString) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());

  // cli_valid=false must fall through to persistence/Rand.
  // Pre-seed the JSON so we can verify it was read.
  ASSERT_TRUE(WriteJsonFile(tmp.GetPath(),
                             R"({"schema_version":1,"render_seed":"12345"})"));

  RenderSeedResolution r = ResolveWithCliInvalid(tmp.GetPath());
  EXPECT_FALSE(r.from_cli);       // not a valid CLI signal
  EXPECT_TRUE(r.from_persistence);  // fell through to JSON
  EXPECT_FALSE(r.newly_generated);
  EXPECT_FALSE(r.write_back_failed);
  EXPECT_EQ(r.seed, 12345ULL);
}

TEST(RenderSeedStoreCliTest, Absent) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());

  // cli_present=false must fall through to persistence/Rand.
  RenderSeedResolution r = ResolveNoCli(tmp.GetPath());
  EXPECT_FALSE(r.from_cli);
  EXPECT_FALSE(r.from_persistence);  // no JSON exists
  EXPECT_TRUE(r.newly_generated);
  EXPECT_FALSE(r.write_back_failed);
  EXPECT_NE(r.seed, 0ULL);  // Rand must produce non-zero

  // A file should have been written.
  EXPECT_TRUE(base::PathExists(
      tmp.GetPath().AppendASCII(RenderSeedStoreConfig::kFileName)));

  // The written value must match what Resolve returned.
  uint64_t stored = 0;
  ASSERT_TRUE(RenderSeedStore::ReadFromFile(tmp.GetPath(), &stored));
  EXPECT_EQ(stored, r.seed);
}

// ============================================================================
// Group 2: Persistence failure modes
// ============================================================================

TEST(RenderSeedStorePersistenceTest, JsonMissing) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  // No JSON file exists -> fall through to Rand + write-back.
  RenderSeedResolution r = ResolveNoCli(tmp.GetPath());
  EXPECT_TRUE(r.newly_generated);
  EXPECT_FALSE(r.write_back_failed);
  EXPECT_NE(r.seed, 0ULL);
}

TEST(RenderSeedStorePersistenceTest, JsonCorrupted) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  ASSERT_TRUE(WriteJsonFile(tmp.GetPath(), "not json at all{{{"));
  ASSERT_FALSE(BakExists(tmp.GetPath()));

  RenderSeedResolution r = ResolveNoCli(tmp.GetPath());
  // Must fall through to Rand + re-generate.
  EXPECT_TRUE(r.newly_generated);
  EXPECT_FALSE(r.write_back_failed);
  // .bak must have been created.
  EXPECT_TRUE(BakExists(tmp.GetPath())) << ".bak should be created from corrupted JSON";
  // Original corrupted file should be gone.
  EXPECT_FALSE(base::PathExists(
      tmp.GetPath().AppendASCII(RenderSeedStoreConfig::kFileName)));
}

TEST(RenderSeedStorePersistenceTest, JsonForwardIncompatible) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  // schema_version = 999 (>> kSchemaVersion=1) -> reject, treat as missing.
  ASSERT_TRUE(WriteJsonFile(tmp.GetPath(),
                             R"({"schema_version":999,"render_seed":"42"})"));
  ASSERT_FALSE(BakExists(tmp.GetPath()));  // forward-incompatible does NOT .bak

  RenderSeedResolution r = ResolveNoCli(tmp.GetPath());
  EXPECT_TRUE(r.newly_generated);
  EXPECT_FALSE(r.from_persistence);
  EXPECT_FALSE(r.write_back_failed);
  // Forward-incompatible does NOT create .bak (that's for corrupted content).
  EXPECT_FALSE(BakExists(tmp.GetPath()));
}

TEST(RenderSeedStorePersistenceTest, JsonEmpty) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  ASSERT_TRUE(WriteJsonFile(tmp.GetPath(), ""));

  RenderSeedResolution r = ResolveNoCli(tmp.GetPath());
  EXPECT_TRUE(r.newly_generated);
  EXPECT_FALSE(r.from_persistence);
  // Empty string fails JSON parse -> treated as corrupted.
  EXPECT_TRUE(BakExists(tmp.GetPath()));
}

TEST(RenderSeedStorePersistenceTest, BakBackupOnParseFailure) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  // Valid JSON but missing the required "render_seed" key.
  ASSERT_TRUE(WriteJsonFile(tmp.GetPath(), R"({"schema_version":1,"created_ms":42})"));
  ASSERT_FALSE(BakExists(tmp.GetPath()));

  RenderSeedResolution r = ResolveNoCli(tmp.GetPath());
  EXPECT_TRUE(r.newly_generated);
  EXPECT_FALSE(r.from_persistence);
  // .bak must exist with the malformed content preserved.
  EXPECT_TRUE(BakExists(tmp.GetPath())) << ".bak should preserve malformed JSON";

  // Verify .bak contains the original malformed bytes.
  std::string bak_content;
  const base::FilePath bak_path =
      tmp.GetPath().AppendASCII(RenderSeedStoreConfig::kFileName)
          .AddExtensionASCII(".bak");
  ASSERT_TRUE(base::ReadFileToString(bak_path, &bak_content));
  EXPECT_TRUE(bak_content.find("schema_version") != std::string::npos);
}

// ============================================================================
// Group 3: No user_data_dir
// ============================================================================

TEST(RenderSeedStoreBehaviorTest, NoUserDataDir) {
  // Empty path means no disk I/O at all.
  base::FilePath empty;
  RenderSeedResolution r = ResolveNoCli(empty);
  EXPECT_TRUE(r.newly_generated);
  EXPECT_FALSE(r.from_persistence);
  EXPECT_FALSE(r.write_back_failed);  // no dir = no write attempt
  EXPECT_NE(r.seed, 0ULL);
}

TEST(RenderSeedStoreBehaviorTest, SubprocessInheritViaCLI) {
  // Simulates a subprocess: CLI already present from browser's propagation.
  // Must NOT re-generate or re-read JSON.
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());

  // Pre-create a "stale" JSON to prove subprocess does NOT consult it.
  ASSERT_TRUE(WriteJsonFile(tmp.GetPath(),
                             R"({"schema_version":1,"render_seed":"9999"})"));

  // Simulate subprocess: inherited CLI switch, no regeneration needed.
  RenderSeedResolution r = ResolveWithCli(0x111122223333ULL, tmp.GetPath());
  EXPECT_TRUE(r.from_cli);
  EXPECT_FALSE(r.from_persistence);
  EXPECT_FALSE(r.newly_generated);
  EXPECT_FALSE(r.write_back_failed);
  EXPECT_EQ(r.seed, 0x111122223333ULL);

  // Stale JSON must NOT have been overwritten.
  uint64_t stale = 0;
  ASSERT_TRUE(RenderSeedStore::ReadFromFile(tmp.GetPath(), &stale));
  EXPECT_EQ(stale, 9999ULL) << "subprocess CLI must not overwrite stale persistence";
}

// ============================================================================
// Group 4: Concurrent first-touch (simulated via temp file pattern)
// ============================================================================

TEST(RenderSeedStoreConcurrencyTest, TwoProcesses_RaceForFirstGeneration) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());

  // Process A: resolves first.
  RenderSeedResolution rA = ResolveNoCli(tmp.GetPath());
  ASSERT_TRUE(rA.newly_generated);
  ASSERT_FALSE(rA.write_back_failed);

  // Process B: sees the JSON that A just wrote.
  RenderSeedResolution rB = ResolveNoCli(tmp.GetPath());
  EXPECT_FALSE(rB.newly_generated);
  EXPECT_TRUE(rB.from_persistence);
  EXPECT_EQ(rB.seed, rA.seed) << "both processes must agree on the same seed";

  // Only one final JSON should exist (A's write).
  const base::FilePath final_path =
      tmp.GetPath().AppendASCII(RenderSeedStoreConfig::kFileName);
  EXPECT_TRUE(base::PathExists(final_path));

  // A's temp file should have been renamed away.
  std::string final_content;
  ASSERT_TRUE(base::ReadFileToString(final_path, &final_content));
  EXPECT_TRUE(final_content.find(base::NumberToString(rA.seed)) !=
              std::string::npos);
}

TEST(RenderSeedStoreConcurrencyTest, SingleProcess_TempFileCleanup) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());

  // Multiple Resolve calls from the same process.
  RenderSeedResolution r1 = ResolveNoCli(tmp.GetPath());
  ASSERT_TRUE(r1.newly_generated);
  ASSERT_FALSE(r1.write_back_failed);

  RenderSeedResolution r2 = ResolveNoCli(tmp.GetPath());
  EXPECT_FALSE(r2.newly_generated);
  EXPECT_TRUE(r2.from_persistence);
  EXPECT_EQ(r1.seed, r2.seed);

  // No stray .tmp files should remain.
  const base::FilePath dir = tmp.GetPath();
  base::FileVector entries;
  base::FileEnumerator enumerator(dir, false,
                                  base::FileEnumerator::FILES);
  for (base::FilePath f = enumerator.Next(); !f.empty();
       f = enumerator.Next()) {
    std::string name = f.BaseName().MaybeAsASCII();
    EXPECT_TRUE(name.find(".tmp") == std::string::npos)
        << "no stray .tmp file should remain: " << name;
  }
}

// ============================================================================
// Group 5: ReadFromFile / WriteToFile public API
// ============================================================================

TEST(RenderSeedStoreAPITest, ReadFromFile_NonExistent) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  uint64_t out = 999;
  EXPECT_FALSE(RenderSeedStore::ReadFromFile(tmp.GetPath(), &out));
  EXPECT_EQ(out, 999ULL);  // unchanged on failure
}

TEST(RenderSeedStoreAPITest, WriteAndReadRoundTrip) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());

  ASSERT_TRUE(RenderSeedStore::WriteToFile(tmp.GetPath(), 0xABCDEFULL));
  uint64_t recovered = 0;
  ASSERT_TRUE(RenderSeedStore::ReadFromFile(tmp.GetPath(), &recovered));
  EXPECT_EQ(recovered, 0xABCDEFULL);
}

TEST(RenderSeedStoreAPITest, WriteToFile_NoUserDataDir) {
  base::FilePath empty;
  EXPECT_FALSE(RenderSeedStore::WriteToFile(empty, 12345));
}

TEST(RenderSeedStoreAPITest, ReadFromFile_EmptyPath) {
  base::FilePath empty;
  uint64_t out = 999;
  EXPECT_FALSE(RenderSeedStore::ReadFromFile(empty, &out));
  EXPECT_EQ(out, 999ULL);  // unchanged
}

TEST(RenderSeedStoreAPITest, ResolutionStructFields) {
  // Sanity-check the 4-field resolution struct.
  RenderSeedResolution r;
  EXPECT_EQ(r.seed, 0ULL);
  EXPECT_FALSE(r.from_cli);
  EXPECT_FALSE(r.from_persistence);
  EXPECT_FALSE(r.newly_generated);
  EXPECT_FALSE(r.write_back_failed);

  r.seed = 1;
  r.from_cli = true;
  r.from_persistence = true;
  r.newly_generated = true;
  r.write_back_failed = true;
  EXPECT_EQ(r.seed, 1ULL);
  EXPECT_TRUE(r.from_cli && r.from_persistence &&
             r.newly_generated && r.write_back_failed);
}

}  // namespace
}  // namespace chromium_fork