// Copyright 2026 Dchromium_fork
//
// Unit tests for CanvasAntiFraudSeedStore (2026-07-26).
//
// Covers the cross-process seed resolution contract:
//   1. CLI override wins over JSON.
//   2. JSON persistence wins over RandUint64.
//   3. RandUint64 + write JSON is the last-resort path.
//   4. CLI seed == 0 falls through (does not poison the PRNG with 0).
//   5. Corrupted JSON is renamed to .bak and the resolver regenerates.
//
// All tests run in a freshly-created temp dir under base::GetTempDir()
// so they don't touch the real user_data_dir.

#include <cstdint>
#include <string>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chromium_fork/canvas_anti_fraud_seed_store.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace chromium_fork {
namespace {

base::FilePath MakeTempDir(base::ScopedTempDir* out) {
  EXPECT_TRUE(out->CreateUniqueTempDir());
  return out->GetPath();
}

void WriteSeedJson(const base::FilePath& user_data_dir, uint64_t seed) {
  base::DictValue dict;
  dict.Set("schema_version",
           static_cast<int>(CanvasAntiFraudSeedStoreConfig::kSchemaVersion));
  dict.Set("render_seed", base::NumberToString(seed));
  dict.Set("created_ms",
           static_cast<double>(base::Time::Now().InMillisecondsSinceUnixEpoch()));
  std::string text;
  base::JSONWriter::Write(dict, &text);
  EXPECT_TRUE(base::WriteFile(
      user_data_dir.AppendASCII(
          CanvasAntiFraudSeedStoreConfig::kFileName),
      text));
}

TEST(CanvasAntiFraudSeedStore, NoInputUsesRand) {
  base::ScopedTempDir tmp;
  auto dir = MakeTempDir(&tmp);
  auto r = CanvasAntiFraudSeedStore::Resolve(dir, false, false, 0);
  EXPECT_TRUE(r.newly_generated);
  EXPECT_FALSE(r.from_persistence);
  EXPECT_FALSE(r.from_cli);
  EXPECT_FALSE(r.write_back_failed);
  EXPECT_NE(r.seed, 0u);
}

TEST(CanvasAntiFraudSeedStore, CliOverridesJson) {
  base::ScopedTempDir tmp;
  auto dir = MakeTempDir(&tmp);
  WriteSeedJson(dir, 12345);

  // CLI present, valid, non-zero wins.
  auto r = CanvasAntiFraudSeedStore::Resolve(dir, true, true, 67890);
  EXPECT_TRUE(r.from_cli);
  EXPECT_FALSE(r.from_persistence);
  EXPECT_FALSE(r.newly_generated);
  EXPECT_EQ(r.seed, 67890u);
}

TEST(CanvasAntiFraudSeedStore, JsonStabilizesAcrossResolves) {
  base::ScopedTempDir tmp;
  auto dir = MakeTempDir(&tmp);

  // First resolve: no JSON yet -> newly_generated, persists.
  auto r1 = CanvasAntiFraudSeedStore::Resolve(dir, false, false, 0);
  EXPECT_TRUE(r1.newly_generated);
  EXPECT_FALSE(r1.write_back_failed);

  // Second resolve on the same dir: should hit JSON path.
  auto r2 = CanvasAntiFraudSeedStore::Resolve(dir, false, false, 0);
  EXPECT_TRUE(r2.from_persistence);
  EXPECT_FALSE(r2.newly_generated);
  EXPECT_EQ(r1.seed, r2.seed);
}

TEST(CanvasAntiFraudSeedStore, ZeroCliFallsThrough) {
  base::ScopedTempDir tmp;
  auto dir = MakeTempDir(&tmp);
  WriteSeedJson(dir, 99999);

  // CLI present + valid + seed == 0 must NOT poison: it falls through
  // to JSON (the documented sentinel semantics).
  auto r = CanvasAntiFraudSeedStore::Resolve(dir, true, true, 0);
  EXPECT_FALSE(r.from_cli);
  EXPECT_TRUE(r.from_persistence);
  EXPECT_EQ(r.seed, 99999u);
}

TEST(CanvasAntiFraudSeedStore, InvalidCliFallsThroughToJson) {
  base::ScopedTempDir tmp;
  auto dir = MakeTempDir(&tmp);
  WriteSeedJson(dir, 424242);

  // cli_valid = false -> persistence layer is consulted.
  auto r = CanvasAntiFraudSeedStore::Resolve(dir, true, false, 0);
  EXPECT_FALSE(r.from_cli);
  EXPECT_TRUE(r.from_persistence);
  EXPECT_EQ(r.seed, 424242u);
}

TEST(CanvasAntiFraudSeedStore, CorruptJsonRenamedToBak) {
  base::ScopedTempDir tmp;
  auto dir = MakeTempDir(&tmp);
  // Write garbage to the seed path.
  const base::FilePath seed_path =
      dir.AppendASCII(CanvasAntiFraudSeedStoreConfig::kFileName);
  ASSERT_TRUE(base::WriteFile(seed_path, "not valid json {{{"));

  auto r = CanvasAntiFraudSeedStore::Resolve(dir, false, false, 0);
  // The corrupt file should have been renamed to *.bak and a fresh
  // seed should have been generated + written.
  EXPECT_TRUE(r.newly_generated);
  EXPECT_FALSE(r.write_back_failed);
  EXPECT_FALSE(r.from_persistence);

  const base::FilePath bak_path = seed_path.AddExtensionASCII(".bak");
  EXPECT_TRUE(base::PathExists(bak_path));
  // After fallback regeneration, seed_path is RE-OCCUPIED by the
  // freshly written JSON (audit correction): the contract is "the
  // corrupt blob is preserved as .bak for inspection; the production
  // path is unblocked by a new JSON". Verify the new JSON is parseable.
  EXPECT_TRUE(base::PathExists(seed_path));
  std::string contents;
  EXPECT_TRUE(base::ReadFileToString(seed_path, &contents));
  EXPECT_FALSE(contents.empty());
  // Should not be the original corrupt blob.
  EXPECT_NE(contents, "not valid json {{{");

  // Subsequent resolve should now hit JSON path.
  auto r2 = CanvasAntiFraudSeedStore::Resolve(dir, false, false, 0);
  EXPECT_TRUE(r2.from_persistence);
  EXPECT_EQ(r.seed, r2.seed);
}

TEST(CanvasAntiFraudSeedStore, EmptyUserDataDirNoWriteAttempt) {
  // No user_data_dir means the resolver cannot read OR write JSON;
  // it falls back to in-memory Rand without attempting a write.
  auto r = CanvasAntiFraudSeedStore::Resolve(base::FilePath(), false, false, 0);
  EXPECT_TRUE(r.newly_generated);
  EXPECT_FALSE(r.write_back_failed);
  EXPECT_NE(r.seed, 0u);
}

TEST(CanvasAntiFraudSeedStore, ForwardIncompatibleSchemaRegenerates) {
  base::ScopedTempDir tmp;
  auto dir = MakeTempDir(&tmp);

  // Manually craft a JSON file with a higher schema_version.
  base::DictValue dict;
  dict.Set("schema_version", 999);
  dict.Set("render_seed", "111");
  std::string text;
  base::JSONWriter::Write(dict, &text);
  ASSERT_TRUE(base::WriteFile(
      dir.AppendASCII(CanvasAntiFraudSeedStoreConfig::kFileName), text));

  auto r = CanvasAntiFraudSeedStore::Resolve(dir, false, false, 0);
  EXPECT_TRUE(r.newly_generated);  // forward-incompatible -> regen
}

}  // namespace
}  // namespace chromium_fork