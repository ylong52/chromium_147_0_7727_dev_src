// Copyright 2026 Dchromium_fork
//
// Per-site anti-fraud seed derivation (2026-07-26).
//
// Replaces ungoogled::GetFarbleSeed64() / GetFarbleSeedString() with an
// in-tree implementation that depends only on base:: and chromium_fork
// modules. The output is stable for a fixed (session_seed, etld1) tuple
// across processes, and varies when either input changes.
//
// Algorithm:
//   1. base_seed = CanvasTestSessionSeedManager.session_seed
//   2. mix       = base::PersistentHash(base_seed || etld1)
//   3. return    = MurmurHash3 64-bit finalizer applied to mix
//
// The Murmur finalizer is the same one used by canvas_noise_engine.cc
// and is intentionally NOT a cryptographic hash: we want fast, well-
// distributed, deterministic mixing, not collision resistance.

#ifndef SRC_CHROMIUM_FORK_FARBLE_SEED_H_
#define SRC_CHROMIUM_FORK_FARBLE_SEED_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace chromium_fork {

// True when the canvas anti-fraud path is enabled in this process.
// Honours the existing CanvasTestSessionSeedManager::IsEnabled() gate
// (which already factors in the allowlist + enable switch).
//
// Thread safety: forwards to CanvasTestSessionSeedManager which is
// internally synchronized.
bool IsAntiFraudNoiseEnabled();

// Derive the per-site seed for the anti-fraud algorithm.
//
//   effective_seed = Murmur3_64_finalizer(
//                      base::PersistentHash(8B(session_seed) || etld1))
//
// |etld1| should be the registrable domain (eTLD+1) of the embedding
// origin when one exists; for opaque origins (file://, blob://, etc.)
// callers should fall back to the full url::Origin string. The function
// does NOT do the fallback itself so the API stays free of url/Origin
// coupling.
uint64_t GetAntiFraudSeed64(std::string_view etld1);

// String form of GetAntiFraudSeed64 for logging.
std::string GetAntiFraudSeedString(std::string_view etld1);

}  // namespace chromium_fork

#endif  // SRC_CHROMIUM_FORK_FARBLE_SEED_H_