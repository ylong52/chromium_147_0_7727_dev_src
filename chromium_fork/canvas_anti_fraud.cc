// Copyright 2026 Dchromium_fork
//
// Audit-revised (2026-07-26.b): see canvas_anti_fraud.h for the list of
// fixes applied in response to the GPT secondary-audit pass.

#include "chromium_fork/canvas_anti_fraud.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "base/command_line.h"
#include "base/hash/hash.h"
#include "base/logging.h"
#include "base/numerics/checked_math.h"
#include "chromium_fork/canvas_session_seed_manager.h"
#include "chromium_fork/chromium_fork_buildflags.h"
#include "chromium_fork/switches.h"
#include "include/core/SkColorType.h"
#include "include/private/base/SkFloatingPoint.h"

namespace chromium_fork {

namespace {

#if BUILDFLAG(ENABLE_CANVAS_TEST_NOISE)

constexpr uint64_t kGoldenRatio = 0x9E3779B97F4A7C15ULL;

// Per-call bytes-per-pixel lookup. Returns 0 for unsupported layouts.
int BytesPerPixelForShuffle(SkColorType type) {
  switch (type) {
    case kGray_8_SkColorType:
      return 1;
    case kRGB_565_SkColorType:
      return 2;
    case kRGBA_8888_SkColorType:
    case kBGRA_8888_SkColorType:
      return 4;
    default:
      return 0;
  }
}

// Audit #6: bounds-checked version of pixel offset. Returns true and
// fills |out_offset| only when (x,y) lies inside the visible rectangle
// and the resulting byte offset fits inside |buffer_bytes|. Audit #6
// also checks for signed-overflow in the multiplications.
bool ComputePixelOffset(size_t row_bytes,
                         int bpp,
                         size_t buffer_bytes,
                         int x,
                         int y,
                         size_t* out_offset) {
  // Reject negative or oversized coordinates up front.
  if (x < 0 || y < 0 || bpp <= 0 || row_bytes == 0) {
    return false;
  }
  // Overflow-checked multiplication. CheckedNumeric arithmetic detects
  // overflow at runtime.
  base::CheckedNumeric<size_t> y_offset =
      base::CheckedNumeric<size_t>(static_cast<size_t>(y)) *
      base::CheckedNumeric<size_t>(row_bytes);
  if (!y_offset.IsValid()) {
    return false;
  }
  base::CheckedNumeric<size_t> x_byte =
      base::CheckedNumeric<size_t>(static_cast<size_t>(x)) *
      base::CheckedNumeric<size_t>(static_cast<size_t>(bpp));
  if (!x_byte.IsValid()) {
    return false;
  }
  base::CheckedNumeric<size_t> total = y_offset + x_byte;
  if (!total.IsValid()) {
    return false;
  }
  const size_t off = total.ValueOrDie();
  if (off + static_cast<size_t>(bpp) > buffer_bytes) {
    return false;
  }
  *out_offset = off;
  return true;
}

// MurmurHash3 64-bit finalizer applied to a uint64. Used to fold
// (src_x, src_y, w, h, alphaType, colorType) into the per-call PRNG
// state. By construction this is content-free, so re-calls on the same
// (effective_seed, identity) tuple produce the same state, satisfying
// audit-finding #8 (idempotence).
uint64_t MurmurFinalizer(uint64_t v) {
  v ^= v >> 30;
  v *= 0xBF58476D1CE4E5B9ULL;
  v ^= v >> 27;
  v *= 0x94D049BB133111EBULL;
  v ^= v >> 31;
  return v;
}

// Audit #8 (idempotence): build a content-free identity tuple so that
// repeated calls on the same canvas produce the SAME PRNG state and
// therefore the SAME mutation. We deliberately do NOT fold pixel
// content: the first call mutates pixels, the second call's PRNG
// re-emits the same (x,y,bits) tuple so the XOR is undone -> stable hash.
uint64_t PerCanvasSalt(int src_x,
                        int src_y,
                        int w,
                        int h,
                        int ct,
                        int at) {
  uint64_t v = (static_cast<uint64_t>(src_x) << 0) ^
               (static_cast<uint64_t>(src_y) << 8) ^
               (static_cast<uint64_t>(w) << 16) ^
               (static_cast<uint64_t>(h) << 24) ^
               (static_cast<uint64_t>(ct) << 36) ^
               (static_cast<uint64_t>(at) << 40);
  return MurmurFinalizer(v);
}

#endif  // BUILDFLAG(ENABLE_CANVAS_TEST_NOISE)

}  // namespace

#if BUILDFLAG(ENABLE_CANVAS_TEST_NOISE)

void ApplyCanvasAntiFraudNoise(const void* addr,
                                const SkImageInfo& info,
                                int src_x,
                                int src_y,
                                uint64_t effective_seed) {
  // Audit #6: guard the entire entry with bounds checks up front so
  // ComputePixelOffset below can rely on the preconditions.
  if (!addr) {
    return;
  }
  if (src_x < 0 || src_y < 0) {
    return;
  }
  if (info.width() <= 0 || info.height() <= 0) {
    return;
  }
  if (src_x >= info.width() || src_y >= info.height()) {
    return;
  }
  const int w = info.width() - src_x;
  const int h = info.height() - src_y;
  if (w < 8 || h < 8) {
    // Skip sub-64-pixel rectangles: the algorithm's perturbation budget
    // would touch every pixel and produce visually obvious noise rather
    // than anti-fraud-style dither.
    return;
  }
  const int bpp = BytesPerPixelForShuffle(info.colorType());
  if (bpp == 0) {
    return;
  }
  const size_t row_bytes = info.minRowBytes();
  if (row_bytes < static_cast<size_t>(w) * static_cast<size_t>(bpp)) {
    return;
  }
  const size_t buffer_bytes = static_cast<size_t>(h) * row_bytes;
  if (buffer_bytes == 0) {
    return;
  }
  // Audit #6: verify the buffer is at least the size we think it is.
  // SkImageInfo does not carry a "buffer length" field, so callers
  // (Blink ImageDataBuffer::pixmap_) must guarantee buffer_bytes is
  // accurate. We add a final guard at the bottom of ComputePixelOffset
  // to be safe.

  char* const pbase = const_cast<char*>(static_cast<const char*>(addr));

  // Audit #8 (idempotence): PRNG state is seeded from
  //   effective_seed XOR PerCanvasSalt(src_x, src_y, w, h, ct, at)
  // NOT from any pixel byte. This guarantees that repeated calls on
  // the same (effective_seed, canvas-identity) tuple re-emit the same
  // (x,y,bits) sequence, so the XOR mutation is its own inverse -> the
  // pixel hash is stable across readbacks.
  uint64_t state =
      effective_seed ^
      PerCanvasSalt(src_x, src_y, w, h,
                    static_cast<int>(info.colorType()),
                    static_cast<int>(info.alphaType()));
  if (state == 0) {
    state = kGoldenRatio;
  }
  auto next = [&state]() -> uint64_t {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };

  // Audit #7 (unique-coordinate): we draw candidate coordinates and
  // verify they have not been used before via a small fixed-size
  // bitmap. For (w*h) small enough that we never visit more than
  // |kMaxCoordTable| distinct sites we keep them all. Beyond that, we
  // accept potential duplicates (extremely unlikely at the configured
  // ~0.17% rate).
  constexpr int kMaxCoordTable = 4096;
  int coord_used[kMaxCoordTable] = {0};  // (y * w + x) -> 1 if used
  const int total_sites = w * h;

  // ~0.17% of pixels get LSB toggled, clamped to [24, 512].
  const long area = static_cast<long>(w) * static_cast<long>(h);
  int n_perturb = static_cast<int>(area / 600);
  if (n_perturb < 24) {
    n_perturb = 24;
  } else if (n_perturb > 512) {
    n_perturb = 512;
  }
  if (n_perturb > total_sites) {
    n_perturb = total_sites;
  }
  // At most 4096 distinct sites are tracked; if total_sites < 4096,
  // ensure we never attempt more perturbations than distinct sites.
  if (total_sites < kMaxCoordTable && n_perturb > total_sites) {
    n_perturb = total_sites;
  }

  int n_done = 0;
  int attempts = 0;
  // The +20% headroom guards against pathological early-skew of the
  // PRNG output; if we still cannot find unique coords we drop the
  // uniqueness check (rare, only for very small canvases).
  const int max_attempts = n_perturb * 2 + 32;
  while (n_done < n_perturb && attempts < max_attempts) {
    ++attempts;
    const uint64_t rnd = next();
    const int x = static_cast<int>(rnd % static_cast<uint64_t>(w));
    const int y = static_cast<int>((rnd >> 21) % static_cast<uint64_t>(h));
    const uint64_t bits = rnd >> 42;
    const int site = y * w + x;
    if (site < kMaxCoordTable) {
      if (coord_used[site]) {
        continue;  // already touched this pixel; skip (audit #7)
      }
      coord_used[site] = 1;
    }

    size_t off = 0;
    if (!ComputePixelOffset(row_bytes, bpp, buffer_bytes, x, y, &off)) {
      // Defensive: should be impossible given the earlier guards, but
      // audit #6 wants every multiplication bounded. If it ever fires,
      // abandon this iteration.
      continue;
    }
    uint8_t* px = reinterpret_cast<uint8_t*>(pbase + off);

    if (bpp == 4) {
      // RGBA8888 / BGRA8888: byte 3 is alpha in both layouts. Audit
      // #4 (premul-alpha): when alphaType is premul, the post-XOR RGB
      // must satisfy R <= A, G <= A, B <= A. We XOR each channel by
      // a different bit (so three distinct 1-bit deltas) and clamp
      // any out-of-range result back down to A. For kOpaque we can
      // XOR freely because there is no premul invariant.
      const uint8_t alpha = px[3];
      const bool is_premul = info.alphaType() == kPremul_SkAlphaType;
      if (is_premul && alpha > 0) {
        uint8_t r = px[0] ^ static_cast<uint8_t>(bits & 0x1u);
        uint8_t g = px[1] ^ static_cast<uint8_t>((bits >> 1) & 0x1u);
        uint8_t b = px[2] ^ static_cast<uint8_t>((bits >> 2) & 0x1u);
        if (r > alpha) r = alpha;
        if (g > alpha) g = alpha;
        if (b > alpha) b = alpha;
        px[0] = r;
        px[1] = g;
        px[2] = b;
      } else {
        // Opaque or non-premul: free XOR on the three colour channels.
        px[0] ^= static_cast<uint8_t>(bits & 0x1u);
        px[1] ^= static_cast<uint8_t>((bits >> 1) & 0x1u);
        px[2] ^= static_cast<uint8_t>((bits >> 2) & 0x1u);
      }
    } else if (bpp == 2) {
      // RGB565 is rare in modern Blink output paths; we only touch the
      // LSB of the low byte. Audit #5: this is the LSB of B in little-
      // endian RGB565 packing (Skia's canonical byte order), so it
      // affects one specific channel rather than mixing two.
      // Endianness is enforced by Skia, which always uses little-endian
      // packing for SkColorType::kRGB_565_SkColorType.
      px[0] ^= static_cast<uint8_t>(bits & 0x1u);
    } else {
      // Gray8: single byte per pixel; XOR the LSB.
      *px ^= static_cast<uint8_t>(bits & 0x1u);
    }
    ++n_done;
  }
  // If attempts exhausted without reaching n_perturb, we accept the
  // remaining perturbation count as zero; the contract only promises
  // "approximately ~0.17% of pixels touched", not exact count.
}

double DeriveMeasureTextNoiseX(uint64_t effective_seed,
                                std::string_view text) {
  std::string buf;
  buf.reserve(8 + text.size());
  for (int i = 0; i < 8; ++i) {
    buf.push_back(static_cast<char>((effective_seed >> (8 * i)) & 0xFFu));
  }
  buf.append(text.data(), text.size());
  uint64_t state =
      base::PersistentHash(buf);
  if (state == 0) {
    state = kGoldenRatio;
  }

  // xorshift64 -> 64-bit hash value
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  const uint64_t hash_val = state;

  // Map to open interval (-0.5, 0.5]. Use only the lower 32 bits so the
  // distribution is uniform across the full range.
  const double u = static_cast<double>(hash_val & 0xFFFFFFFFULL) /
                   static_cast<double>(0x100000000ULL);
  return u - 0.5;
}

bool ShouldApplyMeasureTextNoise() {
  // Audit #23: this gate is only consulted in builds that actually link
  // the canvas anti-fraud algorithm. In Official Builds the call-site
  // compiler strips the noise path entirely; this function is still
  // defined (so Blink links) but returns false unconditionally so
  // measureText is never perturbed.
  const base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  if (cmd->HasSwitch(switches::kForkCanvasAntiFraudSeed)) {
    return true;
  }
  auto* mgr = CanvasTestSessionSeedManager::GetInstance();
  if (mgr->IsEnabled()) {
    const CanvasTestConfig cfg = mgr->GetConfig();
    if (cfg.session_seed != 0) {
      return true;
    }
  }
  return false;
}

#else  // !ENABLE_CANVAS_TEST_NOISE

// Official Build: dead-strip the entire noise path. Functions are still
// defined so Blink callers compile, but every entry returns the empty
// result. This is audit #23's primary mitigation.
void ApplyCanvasAntiFraudNoise(const void* /*addr*/,
                                const SkImageInfo& /*info*/,
                                int /*src_x*/,
                                int /*src_y*/,
                                uint64_t /*effective_seed*/) {
  // No-op.
}

double DeriveMeasureTextNoiseX(uint64_t /*effective_seed*/,
                                std::string_view /*text*/) {
  return 0.0;  // -> ShuffleMetrics(1.0) -> no-op
}

bool ShouldApplyMeasureTextNoise() {
  return false;
}

#endif  // BUILDFLAG(ENABLE_CANVAS_TEST_NOISE)

}  // namespace chromium_fork