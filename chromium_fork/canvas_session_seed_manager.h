// Copyright 2026 Dchromium_fork
//
// Phase P2 / P4 (2026-07-25): Canvas test session seed + renderer-side
// configuration holder.
//
// The SessionSeedManager is a process-lifetime singleton that owns:
//   - one SessionSeed per browser process (or per renderer process, if
//     launched with --canvas-fixed-seed)
//   - the algorithm version and the parsed origin allowlist
//   - the timestamp/PID metadata used for structured logging
//
// The holder on the renderer side receives the same configuration from
// command-line switches (which the Browser main process writes before
// spawning the renderer). Renderer code MUST never regenerate the seed;
// this is enforced by keeping DerivePerCanvasSalt as the only entry
// point that touches session-level entropy.
//
// Compilation gate: enabled only when ENABLE_CANVAS_TEST_NOISE=1.
// The functions are still defined when the gate is off, but every call
// becomes a no-op via IsEnabled() returning false.

#ifndef SRC_CHROMIUM_FORK_CANVAS_SESSION_SEED_MANAGER_H_
#define SRC_CHROMIUM_FORK_CANVAS_SESSION_SEED_MANAGER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"

namespace chromium_fork {

// Snapshot of the configured test perturbation for the current process.
//
// Rule-of-Five (P2-2 2026-07-25): user-declared destructor suppresses
// the implicit move ctor/assignment but does NOT suppress the implicit
// copy ctor/assignment. All fields are POD or std::vector<std::string>,
// which are both copy- and move-friendly, so we = default the entire set
// to silence -Wdeprecated-copy / -Wdefaulted-function-deleted and to
// make the intent explicit. No locking or owning-resource fields here.
struct CanvasTestConfig {
  CanvasTestConfig();
  ~CanvasTestConfig();

  CanvasTestConfig(const CanvasTestConfig&);
  CanvasTestConfig& operator=(const CanvasTestConfig&);
  CanvasTestConfig(CanvasTestConfig&&) noexcept;
  CanvasTestConfig& operator=(CanvasTestConfig&&) noexcept;

  bool enabled = false;
  uint64_t session_seed = 0;
  uint32_t algorithm_version = 1;  // bumped when canvas_noise_engine changes
  std::vector<std::string> allowed_origins;  // exact-match url::Origin strings

  // Metadata (logged but never fed into the noise hash).
  int64_t browser_pid = 0;
  int64_t create_time_ms = 0;
  bool seed_overridden_by_cli = false;

  // Canvas anti-fraud seed source (2026-07-26).
  // Resolved by DetermineSessionSeedLocked() via CanvasAntiFraudSeedStore.
  // One of "cli" | "json" | "random". Logged but never affects pixel output.
  std::string seed_source;
  bool seed_from_cli = false;
  bool seed_from_persistence = false;
  bool seed_newly_generated = false;
  bool seed_write_back_failed = false;
};

// Process-wide manager. Singleton accessed via GetCanvasTestSessionSeedManager().
// All methods are thread-safe; the underlying config is guarded by a lock.
class CanvasTestSessionSeedManager {
 public:
  CanvasTestSessionSeedManager(const CanvasTestSessionSeedManager&) = delete;
  CanvasTestSessionSeedManager& operator=(const CanvasTestSessionSeedManager&) = delete;

  static CanvasTestSessionSeedManager* GetInstance();

  // Idempotent. Idempotency is required because both BrowserInit and
  // RendererInit may call this from the same process (Browser main,
  // before fork; Renderer after fork).
  //
  // Priority for SessionSeed:
  //   1. --canvas-fixed-seed=<uint64>           (highest; CLI override)
  //   2. VEM-supplied seed (--dchromium-fork-canvas-test-noise-seed)
  //   3. base::RandUint64()                     (lowest; auto-generated)
  //
  // Priority for enable flag:
  //   1. --canvas-test-noise-enabled=1          (highest)
  //   2. --dchromium-fork-canvas-test-noise-enabled=1 (legacy)
  //   3. false (default)
  //
  // Errors (invalid seed, invalid origin, etc.) flip enabled=false and
  // emit a structured [CANVAS-TEST-ERROR] log; they NEVER crash.
  void InitializeFromCommandLine();

  // Accessors. Returns a copy because the internal config may be mutated
  // concurrently by InitializeFromCommandLine on another thread during
  // early startup; copies are stable.
  CanvasTestConfig GetConfig() const;

  // Convenience: combined enabled + non-empty allowlist gate.
  // Used by the Canvas 2D readback hook to verify both the feature has
  // been turned on AND at least one allowed origin is configured.
  bool IsEnabled() const;

  // Light-weight readiness probe for callers that don't care about
  // origin allowlists but DO need a stable per-session seed value (e.g.
  // the WebGL readback hook in production builds). Returns true once
  // InitializeFromCommandLine() has resolved a session_seed in the
  // current process, regardless of whether any allowed_origin was
  // supplied. The Browser main process resolves a seed during
  // InitFromCommandLine() / InitFromProfileJSON() and propagates it to
  // children via --canvas-anti-fraud-seed=<n>; children re-resolve the
  // same value through this manager's CLI-short-circuit branch.
  bool IsSeedAvailable() const;

  // Read-only access to the parsed allowlist (exact-match url::Origin
  // strings). Empty list when disabled.
  std::vector<std::string> GetAllowedOrigins() const;

  // True iff |origin| is present verbatim in the configured allowlist.
  // Renderer-side: the Browser already validated this list, but the
  // Renderer MUST re-check on every readback (defence-in-depth, and to
  // reject origins injected from JS via tampering).
  bool IsOriginAllowed(const std::string& origin) const;

  // True iff the seed was set by an explicit CLI flag (vs auto-generated).
  bool IsSeedOverriddenByCli() const;

  // Emit a single structured log line (JSON object on one line). Safe
  // to call multiple times; typically called once per InitializeFromCommandLine.
  void LogSessionSeedStructured() const;

 private:
  CanvasTestSessionSeedManager() = default;
  ~CanvasTestSessionSeedManager() = default;

  // P2-2 2026-07-25: base::NoDestructor must be a friend so it can invoke
  // the private constructor/destructor when materializing the function-local
  // static instance in GetInstance(). Chromium's existing singletons all
  // follow this pattern (see e.g. base::LazyInstance variants).
  friend class base::NoDestructor<CanvasTestSessionSeedManager>;

  // Helpers - all assume the caller holds |lock_|. The annotation is
  // required because the method names end in "Locked" (Chromium style)
  // and clang's -Wthread-safety enforces it.
  void ParseAllowedOriginsLocked(const std::string& origins_csv)
      EXCLUSIVE_LOCKS_REQUIRED(lock_);
  uint64_t DetermineSessionSeedLocked(bool* out_was_overridden)
      EXCLUSIVE_LOCKS_REQUIRED(lock_);
  void WriteStructuredLogLocked() const EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // Lazily init if not yet initialized. Required because in component
  // build, `chromium_fork` source_set is linked into BOTH `content.dll`
  // (where `content_main_runner_impl.cc` calls `InitializeFromCommandLine`)
  // AND `chrome.dll` (where Blink modules call `ApplyCanvasReadbackNoise`).
  // Each DLL has its OWN static singleton because NoDestructor is
  // per-DLL. The first call from the chrome.dll side may not have a
  // matching content.dll-side init; this helper triggers the
  // same-code-path init on the chrome.dll instance so that the Blink
  // readback-path sees enabled=true.
  //
  // Safe to call repeatedly and from multiple threads; the lock guards
  // the `initialized_` check.
  void EnsureInitializedLocked() EXCLUSIVE_LOCKS_REQUIRED(lock_);

  mutable base::Lock lock_;
  CanvasTestConfig config_ GUARDED_BY(lock_);
  bool initialized_ GUARDED_BY(lock_) = false;
};

// Free function for callers that just want a boolean.
bool IsCanvasTestNoiseEnabled();

}  // namespace chromium_fork

#endif  // SRC_CHROMIUM_FORK_CANVAS_SESSION_SEED_MANAGER_H_
