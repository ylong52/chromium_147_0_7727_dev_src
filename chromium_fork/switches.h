// Copyright 2026 Dchromium_fork
// Command-line switches for VirtualEnvironmentManager (fork profile injection).
// Switch names are intentionally hyphen-separated (Dchromium convention).
// Do NOT include components/embedder_support/switches.h here to avoid a
// build-system circular dependency (chromium_fork deps embedder_support).

#ifndef SRC_CHROMIUM_FORK_SWITCHES_H_
#define SRC_CHROMIUM_FORK_SWITCHES_H_

#include <string>

namespace chromium_fork {
namespace switches {

// Full path to the profile JSON file (read by the main process).
// e.g. --dchromium-fork-profile-path=F:\chromium_147.0.7727.102_dev\go_manager\profile.json
extern const char kForkProfilePath[];

// UA-CH simulation switches 鈥?written by the main process before spawning
// renderer/gpu processes, read by VirtualEnvironmentManager in each subprocess.
extern const char kForkPlatform[];           // e.g. "Windows"
extern const char kForkPlatformVersion[];     // e.g. "10.0.19045"
extern const char kForkArchitecture[];      // e.g. "x86"
extern const char kForkModel[];             // e.g. "" (desktop)
extern const char kForkUAFullVersion[];     // e.g. "147.0.7727.102"
extern const char kForkBitness[];          // e.g. "64"
extern const char kForkWow64[];             // "0" or "1"
extern const char kForkFormFactors[];      // comma-separated, e.g. "Desktop"

extern const char kForkTimezone[];         // IANA tz name, e.g. "Asia/Shanghai"
extern const char kForkAcceptLanguages[];  // comma-separated, e.g. "en-US,en"

extern const char kForkCpuCores[];         // e.g. "6" 鈥?overrides SysInfo::NumberOfProcessors()
extern const char kForkMemoryMB[];         // e.g. "16384" 鈥?overrides SysInfo::AmountOfPhysicalMemory()

// Screen simulation switches (Phase 2 v2 schema).
extern const char kForkScreenAvailHeight[]; // e.g. "1040" 鈥?screen height minus taskbar

// Touch simulation switches (Phase 2 v2).
extern const char kForkMaxTouchPoints[];  // e.g. "0" - 0 = non-touch desktop, >=1 = touch
extern const char kForkTouchCapable[];     // e.g. "0" - 0 = desktop, 1 = touch

// Audio simulation switches (Phase 2 v2 schema).
extern const char kForkAudioSampleRate[];     // e.g. "48000" 鈥?AudioContext.sampleRate()
extern const char kForkAudioBaseLatency[];    // e.g. "0.010000" 鈥?baseLatency() (seconds)
extern const char kForkAudioOutputLatency[];  // e.g. "0.015000" 鈥?outputLatency() (seconds)

// Battery simulation switches (Phase 2 v2 schema; v1 鈫?v2 split is_laptop + is_mobile).
extern const char kForkBatteryIsLaptop[];     // "0" / "1" 鈥?desktop: 0, laptop: 1
extern const char kForkBatteryIsMobile[];     // "0" / "1" 鈥?desktop: 0, mobile: 1
extern const char kForkBatteryCharging[];     // "0" / "1" 鈥?charging()
extern const char kForkBatteryLevelMin[];     // e.g. "0.550000" 鈥?level() lower bound
extern const char kForkBatteryLevelMax[];     // e.g. "0.980000" 鈥?level() upper bound

// WebGL simulation switches 鈥?written by the main process before spawning GPU/renderer
// child processes. They map to ANGLE_GL_VENDOR / ANGLE_GL_RENDERER / ANGLE_GL_VERSION
// environment variables (set in InitFromProfileJSON), which ANGLE's
// Context::init{Vendor,Renderer,Version}String() reads in preference to the real
// DXGI vendor/renderer/version. This is the official ANGLE control surface 鈥?// no JS-layer interception, no upper-API override.
extern const char kForkWebGLVendor[];      // e.g. "Google Inc. (Intel)" -> ANGLE_GL_VENDOR
extern const char kForkWebGLRenderer[];    // e.g. "ANGLE (Intel(R) UHD Graphics 630 ...)" -> ANGLE_GL_RENDERER
extern const char kForkWebGLVersion[];      // e.g. "OpenGL ES 3.2.0 ... ANGLE ..." -> ANGLE_GL_VERSION

// WebGL source variants are retired. The supported VEM WebGL configuration
// is limited to explicit vendor/renderer/version values above. Canvas test
// noise is configured independently through CanvasTestSessionSeedManager.

// Media simulation switches (Phase 3 v2 schema; written by the main process before
// spawning renderer child processes).
// - kForkMediaDevices: comma-separated entries. Each entry is encoded as
//     kind|label_b64|deviceId_b64|groupId_b64
//   kind is one of "audioinput"/"audiooutput"/"videoinput".
//   Label/deviceId/groupId are base64-encoded so they may contain ',' or '|'.
// - kForkVoicesPlatformVersion: the OS build key (e.g. "10.0.19045") used to look up
//   profile.json environment.media.voices_by_platform[].
// - kForkVoicesList: comma-separated voice names (e.g.
//   "Microsoft David Desktop,Microsoft Zira Desktop").
extern const char kForkMediaDevices[];
extern const char kForkVoicesPlatformVersion[];
extern const char kForkVoicesList[];

// Geolocation override (Phase 5; 2026-07-21).
// Injected by main process before spawning renderer/browser children.
// WebContentsImpl::Init() reads these in the browser process and calls
// device::mojom::GeolocationContext::SetOverride() so that the renderer
// receives a forged position matching the IP/locale persona - without
// touching JS-level navigator.geolocation.
//
// Convention (per VEM style):
//   - "0"  for latitude/longitude   = no override (use real Windows Location API)
//   - "-1" for altitude_accuracy/heading/speed  = field not reported
//   - positive accuracy > 0 = position is reported
//
// Empty/absent latitude & longitude both  = no override.
extern const char kForkGeoLatitude[];          // e.g. "34.052200" - LA downtown
extern const char kForkGeoLongitude[];         // e.g. "-118.243700"
// Phase 5.b (2026-07-24): range form. Format "min,max" (e.g.
//   "22.2,22.6"). When set, VEM samples uniformly inside the bracket at
//   InitFromCommandLine time and writes the sample into kForkGeoLatitude /
//   kForkGeoLongitude. Empty/absent = single-point form governs.
extern const char kForkGeoLatitudeRange[];     // e.g. "22.2,22.6"
extern const char kForkGeoLongitudeRange[];    // e.g. "113.8,114.4"
// Phase 5.b (2026-07-24): accuracy range form. Format "min,max" in metres.
extern const char kForkGeoAccuracyRange[];    // e.g. "30.0,50.0"
extern const char kForkGeoAccuracy[];          // metres, e.g. "50.0"
extern const char kForkGeoAltitude[];          // meters, e.g. "71.0"; -10000 = not reported
extern const char kForkGeoAltitudeAccuracy[];  // meters, e.g. "10.0"; -1 = not reported
extern const char kForkGeoHeading[];           // degrees [0, 360); -1 = not reported
extern const char kForkGeoSpeed[];             // m/s, >= 0; -1 = not reported
// Phase 5.c (2026-07-25): ISO 3166-1 alpha-2 country code derived from the
// outbound IP via MaxMind at launcher side (Go start-of-session). NOT exposed
// to JS (W3C GeolocationCoordinates has no such field) - consumed by:
//   - internal bookkeeping (effective-profile.json, chrome://dchromium-fork)
//   - downstream hooks that need a stable country string (e.g. D platform
//     business-side handshake that mirrors the locale).
// Empty/absent = no override (caller should fall back to MaxMind on demand).
extern const char kForkGeoCountryCode[];       // e.g. "HK" / "CN" / "JP" / "US" / "GB"

// Permission status override for navigator.permissions.query() (Phase 5).
// "prompt" / "granted" / "denied". Empty/absent = no override (use real
// content_settings state).
extern const char kForkPermissionGeolocation[];

// M4 (Phase 0): hide automation flag.
extern const char kForkHideWebdriver[];  // "0" = expose webdriver, "1" = hide

// Reduced motion preference.
extern const char kForkPrefersReducedMotion[];  // "0" / "1"

// ============================================================================
// Test-only deterministic Canvas 2D RGBA8 readback perturbation
// Phase P1 (2026-07-25): unified switch names + legacy aliases.
// New switches take precedence; legacy switches are still read with a
// one-shot [CANVAS-TEST-INFO] warning.
// ============================================================================
extern const char kForkCanvasTestNoiseEnabled[];
extern const char kForkCanvasTestNoiseSeed[];
extern const char kForkCanvasTestNoiseCaseId[];
extern const char kForkCanvasTestNoiseSampleRate[];
extern const char kForkCanvasTestNoiseMaxPixels[];
extern const char kForkCanvasTestNoiseDeltaMin[];
extern const char kForkCanvasTestNoiseDeltaMax[];
extern const char kForkCanvasTestNoiseMaxTotalDelta[];
extern const char kForkCanvasTestNoiseAllowedOrigins[];

// The VEM render_seed switch and RenderSeedStore integration were retired.
// Keeping Canvas-test seed ownership in CanvasTestSessionSeedManager avoids
// two unrelated seed stores and prevents an unused seed from being mistaken
// for a production rendering control.
// Chromium-native --user-data-dir reference used only to locate the Canvas
// test-seed store. The VEM does not append this switch itself.
extern const char kForkUserDataDir[];

// ============================================================================
// Canvas anti-fraud seed (2026-07-26)
// ----------------------------------------------------------------------------
// Cross-process behaviour for canvas anti-fraud noise:
//
//   --canvas-anti-fraud-seed=<n>     CLI override (highest precedence).
//                                     n == 0 -> falls through to JSON / Rand.
//   <user_data_dir>/canvas_anti_fraud_seed.json
//                                     Per-profile persistence (default).
//   base::RandUint64()               First-touch generation (last resort).
//
// Default cross-process behaviour: STABLE (read JSON if present).
// Pass this CLI to force a specific value.
// ============================================================================
extern const char kForkCanvasAntiFraudSeed[];        // --canvas-anti-fraud-seed

// ============================================================================
// Canvas anti-fraud session seed — force new per-launch (2026-07-27)
// ----------------------------------------------------------------------------
// When present on the command line, CanvasAntiFraudSeedStore::Resolve()
// SKIPS reading canvas_anti_fraud_seed.json and ALWAYS generates a fresh
// random seed via base::RandUint64(). The generated seed is NOT written
// back to disk (WriteToFile is suppressed), so every Chrome launch gets
// a new random seed.
//
// Use case: prevent canvas fingerprint correlation across browser restarts
// within the same profile — identical content produces different noise,
// breaking fingerprint stability tracking.
//
// Note: --canvas-anti-fraud-seed=<n> still takes precedence over this flag
// (seed is pinned to n). Only when no --canvas-anti-fraud-seed is passed
// does this flag have any effect.
// ============================================================================
extern const char kForkCanvasAntiFraudNewSessionSeed[];  // --canvas-anti-fraud-new-session-seed

// ============================================================================
// WebGL readback noise runtime gate (2026-07-27)
// ----------------------------------------------------------------------------
// Production runtime switch for WebGL::readPixels() noise injection.
// Compiled in only when ENABLE_CANVAS_TEST_NOISE=1 (the same compile-time
// gate that brings in webgl_readback_noise.cc). At runtime the gate is
// independent of the canvas-noise commandline -- it directly controls
// whether the WebGL readback hook in ReadPixelsHelper() applies the noise
// transform. When the switch is absent / explicitly 0 the production
// WebGL output matches native Chrome byte-for-byte.
//
// Allowed values:
//   --webgl-noise-enabled=1   enable WebGL noise on all readbacks
//   --webgl-noise-enabled=0   force-disabled (default; matches Official Build)
//   (switch absent)          treat as 0
// ============================================================================
extern const char kForkWebGLNoiseEnabled[];   // --webgl-noise-enabled

}  // namespace switches
}  // namespace chromium_fork

#endif  // SRC_CHROMIUM_FORK_SWITCHES_H_
