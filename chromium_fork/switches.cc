// Copyright 2026 Dchromium_fork

#include "chromium_fork/switches.h"

namespace chromium_fork {
namespace switches {

const char kForkProfilePath[] = "dchromium-fork-profile-path";

const char kForkPlatform[] = "dchromium-fork-platform";
const char kForkPlatformVersion[] = "dchromium-fork-platform-version";
const char kForkArchitecture[] = "dchromium-fork-architecture";
const char kForkModel[] = "dchromium-fork-model";
const char kForkUAFullVersion[] = "dchromium-fork-ua-full-version";
const char kForkBitness[] = "dchromium-fork-bitness";
const char kForkWow64[] = "dchromium-fork-wow64";
const char kForkFormFactors[] = "dchromium-fork-form-factors";

const char kForkTimezone[] = "dchromium-fork-timezone";
const char kForkAcceptLanguages[] = "dchromium-fork-accept-languages";

const char kForkCpuCores[] = "dchromium-fork-cpu-cores";
const char kForkMemoryMB[] = "dchromium-fork-memory-mb";

const char kForkScreenAvailHeight[] = "dchromium-fork-screen-avail-height";

const char kForkMaxTouchPoints[] = "dchromium-fork-max-touch-points";
const char kForkTouchCapable[] = "dchromium-fork-touch-capable";

const char kForkAudioSampleRate[] = "dchromium-fork-audio-sample-rate";
const char kForkAudioBaseLatency[] = "dchromium-fork-audio-base-latency";
const char kForkAudioOutputLatency[] = "dchromium-fork-audio-output-latency";

const char kForkBatteryIsLaptop[] = "dchromium-fork-battery-is-laptop";
const char kForkBatteryIsMobile[] = "dchromium-fork-battery-is-mobile";
const char kForkBatteryCharging[] = "dchromium-fork-battery-charging";
const char kForkBatteryLevelMin[] = "dchromium-fork-battery-level-min";
const char kForkBatteryLevelMax[] = "dchromium-fork-battery-level-max";

const char kForkWebGLVendor[] = "dchromium-fork-webgl-vendor";
const char kForkWebGLRenderer[] = "dchromium-fork-webgl-renderer";
const char kForkWebGLVersion[] = "dchromium-fork-webgl-version";

const char kForkMediaDevices[] = "dchromium-fork-media-devices";
const char kForkVoicesPlatformVersion[] = "dchromium-fork-voices-platform-version";
const char kForkVoicesList[] = "dchromium-fork-voices-list";

const char kForkGeoLatitude[] = "dchromium-fork-geo-latitude";
const char kForkGeoLongitude[] = "dchromium-fork-geo-longitude";
const char kForkGeoLatitudeRange[] = "dchromium-fork-geo-latitude-range";
const char kForkGeoLongitudeRange[] = "dchromium-fork-geo-longitude-range";
const char kForkGeoAccuracyRange[] = "dchromium-fork-geo-accuracy-range";
const char kForkGeoAccuracy[] = "dchromium-fork-geo-accuracy";
const char kForkGeoAltitude[] = "dchromium-fork-geo-altitude";
const char kForkGeoAltitudeAccuracy[] = "dchromium-fork-geo-altitude-accuracy";
const char kForkGeoHeading[] = "dchromium-fork-geo-heading";
const char kForkGeoSpeed[] = "dchromium-fork-geo-speed";
const char kForkGeoCountryCode[] = "dchromium-fork-geo-country-code";
const char kForkPermissionGeolocation[] = "dchromium-fork-permission-geolocation";

const char kForkHideWebdriver[] = "dchromium-fork-hide-webdriver";
const char kForkPrefersReducedMotion[] = "dchromium-fork-prefers-reduced-motion";

const char kForkCanvasTestNoiseEnabled[] =
    "dchromium-fork-canvas-test-noise-enabled";
const char kForkCanvasTestNoiseSeed[] = "dchromium-fork-canvas-test-noise-seed";
const char kForkCanvasTestNoiseCaseId[] =
    "dchromium-fork-canvas-test-noise-case-id";
const char kForkCanvasTestNoiseSampleRate[] =
    "dchromium-fork-canvas-test-noise-sample-rate";
const char kForkCanvasTestNoiseMaxPixels[] =
    "dchromium-fork-canvas-test-noise-max-pixels";
const char kForkCanvasTestNoiseDeltaMin[] =
    "dchromium-fork-canvas-test-noise-delta-min";
const char kForkCanvasTestNoiseDeltaMax[] =
    "dchromium-fork-canvas-test-noise-delta-max";
const char kForkCanvasTestNoiseMaxTotalDelta[] =
    "dchromium-fork-canvas-test-noise-max-total-delta";
const char kForkCanvasTestNoiseAllowedOrigins[] =
    "dchromium-fork-canvas-test-noise-allowed-origins";

// VEM x RenderFingerprint (2026-07-26).
// Note: --user-data-dir is a Chromium-native switch, declared here only as
// a named reference for the InitFromCommandLine source-side resolution code.
// Do NOT add to the actual command line via AppendSwitch; it must be
// supplied by the Go manager launcher.
const char kForkUserDataDir[] = "user-data-dir";

// Canvas anti-fraud seed (2026-07-26).
const char kForkCanvasAntiFraudSeed[] = "canvas-anti-fraud-seed";

// Canvas anti-fraud session seed — force new per-launch (2026-07-27).
const char kForkCanvasAntiFraudNewSessionSeed[] =
    "canvas-anti-fraud-new-session-seed";

// WebGL readback noise runtime gate (2026-07-27).
const char kForkWebGLNoiseEnabled[] = "webgl-noise-enabled";

}  // namespace switches
}  // namespace chromium_fork
