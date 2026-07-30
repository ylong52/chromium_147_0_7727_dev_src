// Copyright 2026 Dchromium_fork

#include "chromium_fork/farble_seed.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "base/hash/hash.h"
#include "chromium_fork/canvas_session_seed_manager.h"

namespace chromium_fork {

namespace {

// MurmurHash3 64-bit finalizer. Same constants as
// canvas_noise_engine.cc::MixHash so the two functions agree on byte
// distribution for the same input.
uint64_t MurmurHash3Finalizer(uint64_t value) {
  value ^= value >> 30;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27;
  value *= 0x94d049bb133111ebULL;
  value ^= value >> 31;
  return value;
}

// Build the (session_seed || etld1) buffer used for hashing.
std::string BuildHashBuffer(uint64_t base_seed, std::string_view etld1) {
  std::string buf;
  buf.reserve(8 + etld1.size());
  for (int i = 0; i < 8; ++i) {
    buf.push_back(static_cast<char>((base_seed >> (8 * i)) & 0xFFu));
  }
  buf.append(etld1.data(), etld1.size());
  return buf;
}

}  // namespace

bool IsAntiFraudNoiseEnabled() {
  return IsCanvasTestNoiseEnabled();
}

uint64_t GetAntiFraudSeed64(std::string_view etld1) {
  const uint64_t base_seed =
      CanvasTestSessionSeedManager::GetInstance()->GetConfig().session_seed;
  const std::string buf = BuildHashBuffer(base_seed, etld1);
  const uint64_t raw =
      base::PersistentHash(buf);
  return MurmurHash3Finalizer(raw);
}

std::string GetAntiFraudSeedString(std::string_view etld1) {
  return std::to_string(GetAntiFraudSeed64(etld1));
}

}  // namespace chromium_fork