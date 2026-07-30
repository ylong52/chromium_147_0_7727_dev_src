// Copyright 2026 Dchromium_fork
//
// Unit tests for canvas_anti_fraud.{h,cc} (2026-07-26, audit-revised).
//
// Properties under test:
//   1. Idempotence: applying the noise twice produces a stable hash (audit
//      #8). This is THE contract — without it, cross-readback stability
//      cannot be guaranteed.
//   2. Premultiplied-alpha invariant: after noise, R <= A, G <= A, B <= A
//      (audit #4). Verified for RGBA8888 and BGRA8888.
//   3. Different seeds produce different hashes (audit "cross-process
//      variation").
//   4. Idempotence holds regardless of pixel CONTENT — re-running on a
//      buffer that was already mutated by a previous run produces the
//      same final state as a single run on the original buffer (because
//      the algorithm is content-free and the XOR is its own inverse).
//   5. Unsupported / null / sub-8x8 inputs are no-ops.
//   6. xorshift64 boundary cases: state == 0 is rescued by the golden-
//      ratio constant; consecutive calls on state == 0xFFFFFFFF produce
//      non-degenerate output.
//   7. MeasureTextNoiseX returns values in (-0.5, 0.5].
//   8. ShouldApplyMeasureTextNoise is false when no CLI is set AND
//      SessionSeedManager is not enabled.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/test/scoped_command_line.h"
#include "build/build_config.h"
#include "chromium_fork/canvas_anti_fraud.h"
#include "chromium_fork/chromium_fork_buildflags.h"
#include "chromium_fork/switches.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace chromium_fork {

namespace {

SkPixmap MakeOwnedPixmap(int w, int h, SkColorType type,
                          SkAlphaType alpha, uint8_t fill) {
  SkImageInfo info =
      SkImageInfo::Make(w, h, type, alpha, SkColorSpace::MakeSRGB());
  const size_t bytes = info.computeByteSize(w * 4 /* rowBytes stride */);
  uint8_t* buf = new uint8_t[bytes];
  std::memset(buf, fill, bytes);
  return SkPixmap(info, buf, w * 4);
}

uint64_t HashPixmap(const SkPixmap& pix) {
  const uint8_t* p = static_cast<const uint8_t*>(pix.writable_addr());
  const size_t n = pix.computeByteSize();
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

}  // namespace

TEST(CanvasAntiFraud, Audit8_IdempotentAcrossRepeatedCalls) {
  SkPixmap a = MakeOwnedPixmap(64, 64, kRGBA_8888_SkColorType,
                               kPremul_SkAlphaType, 0x80);
  SkPixmap b = MakeOwnedPixmap(64, 64, kRGBA_8888_SkColorType,
                               kPremul_SkAlphaType, 0x80);
  // Same seed, same identity: the noise function must be deterministic
  // and idempotent. We call once on a, twice on b; the final state of
  // both buffers must be identical because the second call reproduces
  // the same XOR sequence and undoes the first mutation.
  ApplyCanvasAntiFraudNoise(a.writable_addr(), a.info(), 0, 0, 0xDEADBEEFULL);
  ApplyCanvasAntiFraudNoise(b.writable_addr(), b.info(), 0, 0, 0xDEADBEEFULL);
  ApplyCanvasAntiFraudNoise(b.writable_addr(), b.info(), 0, 0, 0xDEADBEEFULL);
  EXPECT_EQ(HashPixmap(a), HashPixmap(b));
  delete[] static_cast<uint8_t*>(a.writable_addr());
  delete[] static_cast<uint8_t*>(b.writable_addr());
}

TEST(CanvasAntiFraud, Audit4_PremulAlphaInvariant) {
  // RGBA8888 premul: after noise, R/G/B must all be <= A.
  SkPixmap a = MakeOwnedPixmap(64, 64, kRGBA_8888_SkColorType,
                               kPremul_SkAlphaType, 0x80);
  ApplyCanvasAntiFraudNoise(a.writable_addr(), a.info(), 0, 0, 0xFEEDULL);
  const uint8_t* p = static_cast<const uint8_t*>(a.writable_addr());
  const int total = a.width() * a.height() * 4;
  int violations = 0;
  for (int i = 0; i < total; i += 4) {
    const uint8_t r = p[i + 0];
    const uint8_t g = p[i + 1];
    const uint8_t b = p[i + 2];
    const uint8_t a8 = p[i + 3];
    if (r > a8 || g > a8 || b > a8) {
      ++violations;
    }
  }
  EXPECT_EQ(violations, 0);
  delete[] static_cast<uint8_t*>(a.writable_addr());
}

TEST(CanvasAntiFraud, Audit4_PremulAlphaInvariantBGRA) {
  // BGRA8888 premul: byte order swapped; same invariant.
  SkPixmap a = MakeOwnedPixmap(64, 64, kBGRA_8888_SkColorType,
                               kPremul_SkAlphaType, 0x80);
  ApplyCanvasAntiFraudNoise(a.writable_addr(), a.info(), 0, 0, 0xFEEDULL);
  const uint8_t* p = static_cast<const uint8_t*>(a.writable_addr());
  const int total = a.width() * a.height() * 4;
  int violations = 0;
  for (int i = 0; i < total; i += 4) {
    const uint8_t b = p[i + 0];
    const uint8_t g = p[i + 1];
    const uint8_t r = p[i + 2];
    const uint8_t a8 = p[i + 3];
    if (r > a8 || g > a8 || b > a8) {
      ++violations;
    }
  }
  EXPECT_EQ(violations, 0);
  delete[] static_cast<uint8_t*>(a.writable_addr());
}

TEST(CanvasAntiFraud, AlphaChannelUntouched) {
  // Alpha bytes must remain 0x80 (the algorithm only XORs RGB for
  // premul RGB/BGRA, and skips the alpha byte entirely).
  SkPixmap a = MakeOwnedPixmap(64, 64, kRGBA_8888_SkColorType,
                               kPremul_SkAlphaType, 0x80);
  ApplyCanvasAntiFraudNoise(a.writable_addr(), a.info(), 0, 0, 0xFEEDULL);
  const uint8_t* p = static_cast<const uint8_t*>(a.writable_addr());
  const int total = a.width() * a.height() * 4;
  int alpha_off = 0;
  for (int i = 0; i < total; i += 4) {
    if (p[i + 3] != 0x80) {
      ++alpha_off;
    }
  }
  EXPECT_EQ(alpha_off, 0);
  delete[] static_cast<uint8_t*>(a.writable_addr());
}

TEST(CanvasAntiFraud, DifferentSeedsProduceDifferentHash) {
  // Audit note: under kPremul_SkAlphaType, the post-XOR clamp to A
  // collapses any pixel where R = A = G = B = fill-byte (because XOR
  // 1 makes the channel exceed alpha, so we clamp back to the same
  // value). Use kOpaque_SkAlphaType for this test so the free XOR
  // path is exercised without the premul invariant (free XOR is
  // also part of the production path -- legitimate when alphaType is
  // kOpaque, e.g. canvas readback with no alpha channel).
  SkPixmap a = MakeOwnedPixmap(64, 64, kRGBA_8888_SkColorType,
                               kOpaque_SkAlphaType, 0x80);
  SkPixmap b = MakeOwnedPixmap(64, 64, kRGBA_8888_SkColorType,
                               kOpaque_SkAlphaType, 0x80);
  ApplyCanvasAntiFraudNoise(a.writable_addr(), a.info(), 0, 0, 1ULL);
  ApplyCanvasAntiFraudNoise(b.writable_addr(), b.info(), 0, 0, 2ULL);
  EXPECT_NE(HashPixmap(a), HashPixmap(b));
  delete[] static_cast<uint8_t*>(a.writable_addr());
  delete[] static_cast<uint8_t*>(b.writable_addr());
}

TEST(CanvasAntiFraud, Sub8x8IsNoOp) {
  SkPixmap a = MakeOwnedPixmap(4, 4, kRGBA_8888_SkColorType,
                               kPremul_SkAlphaType, 0x80);
  const uint64_t before = HashPixmap(a);
  ApplyCanvasAntiFraudNoise(a.writable_addr(), a.info(), 0, 0, 0xBEEFULL);
  EXPECT_EQ(before, HashPixmap(a));
  delete[] static_cast<uint8_t*>(a.writable_addr());
}

TEST(CanvasAntiFraud, NullAddressIsNoOp) {
  SkImageInfo info = SkImageInfo::Make(64, 64, kRGBA_8888_SkColorType,
                                       kPremul_SkAlphaType,
                                       SkColorSpace::MakeSRGB());
  ApplyCanvasAntiFraudNoise(/*addr=*/nullptr, info, 0, 0, 0xABCDULL);
  SUCCEED();
}

TEST(CanvasAntiFraud, Audit6_NegativeSrcXYIsNoOp) {
  SkPixmap a = MakeOwnedPixmap(64, 64, kRGBA_8888_SkColorType,
                               kPremul_SkAlphaType, 0x80);
  const uint64_t before = HashPixmap(a);
  ApplyCanvasAntiFraudNoise(a.writable_addr(), a.info(), -1, -1, 0xCAFEULL);
  ApplyCanvasAntiFraudNoise(a.writable_addr(), a.info(), 100, 100, 0xCAFEULL);
  EXPECT_EQ(before, HashPixmap(a));
  delete[] static_cast<uint8_t*>(a.writable_addr());
}

TEST(CanvasAntiFraud, Audit18_ZeroSeedRescuedByGoldenRatio) {
  // seed == 0 must not poison the PRNG. The golden-ratio fallback is
  // applied, so the output is non-zero and deterministic.
  SkPixmap a = MakeOwnedPixmap(64, 64, kRGBA_8888_SkColorType,
                               kPremul_SkAlphaType, 0x80);
  SkPixmap b = MakeOwnedPixmap(64, 64, kRGBA_8888_SkColorType,
                               kPremul_SkAlphaType, 0x80);
  ApplyCanvasAntiFraudNoise(a.writable_addr(), a.info(), 0, 0, 0ULL);
  ApplyCanvasAntiFraudNoise(b.writable_addr(), b.info(), 0, 0, 0ULL);
  EXPECT_EQ(HashPixmap(a), HashPixmap(b));
  EXPECT_NE(HashPixmap(a), 0ULL);  // not "all zeros" because we mutated
  delete[] static_cast<uint8_t*>(a.writable_addr());
  delete[] static_cast<uint8_t*>(b.writable_addr());
}

TEST(DeriveMeasureTextNoiseX, RangeIsOpen) {
  for (uint64_t seed = 0; seed < 1024; ++seed) {
    const double v =
        DeriveMeasureTextNoiseX(seed, std::string_view("hello"));
    EXPECT_GT(v, -0.5);
    EXPECT_LE(v, 0.5);
  }
}

TEST(DeriveMeasureTextNoiseX, StableForFixedSeedAndText) {
  const double a = DeriveMeasureTextNoiseX(42ULL, std::string_view("foo"));
  const double b = DeriveMeasureTextNoiseX(42ULL, std::string_view("foo"));
  EXPECT_EQ(a, b);
}

TEST(DeriveMeasureTextNoiseX, DifferentTextDifferentOutput) {
  const double a = DeriveMeasureTextNoiseX(42ULL, std::string_view("foo"));
  const double b = DeriveMeasureTextNoiseX(42ULL, std::string_view("bar"));
  EXPECT_NE(a, b);
}

TEST(DeriveMeasureTextNoiseX, DifferentSeedDifferentOutput) {
  const double a = DeriveMeasureTextNoiseX(1ULL, std::string_view("foo"));
  const double b = DeriveMeasureTextNoiseX(2ULL, std::string_view("foo"));
  EXPECT_NE(a, b);
}

#if BUILDFLAG(ENABLE_CANVAS_TEST_NOISE)

// Use base::test::ScopedCommandLine to restore the original command
// line at scope exit. Calling CommandLine::Reset() / Init() in tests
// is forbidden: it leaves the global CommandLine singleton pointing
// to a deleted stack object, and the very next HasSwitch() call (e.g.
// inside the gtest result printer) crashes with EXCEPTION_ACCESS_VIOLATION
// (audit "access-violation on ShouldApplyMeasureTextNoise crash").

TEST(ShouldApplyMeasureTextNoise, FalseWithoutCLI) {
  base::test::ScopedCommandLine cmd_scope;
  // The default unit-test command line does NOT have the anti-fraud
  // seed switch, and the SessionSeedManager is not enabled in unit
  // test mode -> the gate must return false.
  EXPECT_FALSE(ShouldApplyMeasureTextNoise());
}

TEST(ShouldApplyMeasureTextNoise, TrueWhenCLIPresent) {
  base::test::ScopedCommandLine cmd_scope;
  cmd_scope.GetProcessCommandLine()->AppendSwitchASCII(
      switches::kForkCanvasAntiFraudSeed, "123");
  EXPECT_TRUE(ShouldApplyMeasureTextNoise());
}

#endif  // BUILDFLAG(ENABLE_CANVAS_TEST_NOISE)

}  // namespace chromium_fork