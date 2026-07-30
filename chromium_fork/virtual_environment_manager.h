// Copyright 2026 Dchromium_fork

// VirtualEnvironmentManager: centralized simulation profile management.
// All virtual values are set once at startup (via InitFromCommandLine) and
// read-only thereafter. No fields are ever read from real hardware.

#ifndef SRC_CHROMIUM_FORK_VIRTUAL_ENVIRONMENT_MANAGER_H_
#define SRC_CHROMIUM_FORK_VIRTUAL_ENVIRONMENT_MANAGER_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"

namespace chromium_fork {

// UA-CH form factors comma-separated string.
struct FormFactors {
  std::vector<std::string> items;
  FormFactors();
  ~FormFactors();
  bool empty() const { return items.empty(); }
};

// Immutable virtual environment snapshot for one session.
struct VirtualEnvironmentData {
  VirtualEnvironmentData();
  ~VirtualEnvironmentData();

  bool   initialized = false;

  // UA-CH simulation fields.
  std::string platform;          // e.g. "Windows"
  std::string platform_version;  // e.g. "10.0.19045"
  std::string architecture;      // e.g. "x86" (not "x86_64" per spec)
  std::string model;             // e.g. "" (desktop)
  std::string ua_full_version;   // e.g. "147.0.7727.102"
  std::string bitness;           // e.g. "64"
  bool        wow64 = false;     // false for native 64-bit
  FormFactors form_factors;      // e.g. {"Desktop"}

  // Locale / timezone.
  std::string timezone;        // IANA tz name, e.g. "Asia/Shanghai"
  std::string accept_languages; // comma-separated, e.g. "en-US,en"

  // CPU / memory.
  int    cpu_cores = 0;
  int    logical_threads = 0;
  int64_t memory_mb = 0;

  // Screen (Phase 2 v2).
  int screen_avail_height = 0;  // 0 = no override

  // Touch simulation (Phase 2 v2).
  int max_touch_points = -1;     // -1 = no override
  int touch_capable = -1;        // -1 = no override, 0 = desktop, 1 = touch

  // Audio (Phase 2 v2).
  uint32_t audio_sample_rate = 0;       // 0 = no override
  double   audio_base_latency = 0.0;    // seconds
  double   audio_output_latency = 0.0;  // seconds

  // Battery (Phase 2 v2).
  int    battery_is_laptop = -1;   // -1 = no override, 0 = desktop, 1 = laptop
  int    battery_is_mobile = -1;     // -1 = no override, 0 = desktop, 1 = mobile
  int    battery_charging = -1;      // -1 = no override, 0 = discharging, 1 = charging
  double battery_level_min = 0.0;    // 0 = no override
  double battery_level_max = 0.0;    // 0 = no override

  // WebGL (Phase 1-4).
  std::string webgl_vendor;
  std::string webgl_renderer;
  std::string webgl_version;

  // Media devices (Phase 3).
  std::string media_devices;              // comma-separated; empty = no override
  std::string voices_platform_version;    // e.g. "10.0.19045"
  std::vector<std::string> voices;        // e.g. {"Microsoft David Desktop"}

  // Geolocation override (Phase 5; 2026-07-21).
  double geo_latitude = 0.0;     // degrees; 0 = no override (equator is valid)
  double geo_longitude = 0.0;    // degrees; 0 = no override (prime meridian is valid)
  double geo_accuracy = 0.0;     // metres; 0 = no override
  double geo_altitude = -10000.0;  // metres; -10000 = not reported
  double geo_altitude_accuracy = -1.0;  // metres; -1 = not reported
  double geo_heading = -1.0;     // degrees [0,360); -1 = not reported
  double geo_speed = -1.0;       // m/s >=0; -1 = not reported
  // Phase 5.c (2026-07-25): ISO 3166-1 alpha-2 country code (e.g. "HK").
  // Mirrors the IP-derived country at launcher side. NOT used by JS layer.
  std::string geo_country_code;

  // Phase 5.b (2026-07-24): range form fields.
  bool   geo_has_latitude_range  = false;
  bool   geo_has_longitude_range = false;
  double geo_latitude_range[2]   = {0.0, 0.0};  // [min, max] in degrees
  double geo_longitude_range[2]  = {0.0, 0.0};  // [min, max] in degrees
  bool   geo_has_accuracy_range = false;
  double geo_accuracy_range[2] = {0.0, 0.0};     // [min, max] in metres

  // Geolocation permission status override.
  std::string geo_permission_override;  // "granted"/"prompt"/"denied"; empty = no override

  // M4 (Phase 0): hide automation (webdriver) flag.
  bool hide_webdriver = false;  // false = show webdriver normally

  // Reduced motion preference.
  bool prefers_reduced_motion = false;  // false = no reduced motion preference

  // Test-only Canvas 2D RGBA8 readback perturbation. Disabled by default.
  bool canvas_test_noise_enabled = false;
  uint64_t canvas_test_noise_seed = 0;
  uint32_t canvas_test_noise_case_id = 0;
  double canvas_test_noise_sample_rate = 0.0;
  uint32_t canvas_test_noise_max_pixels = 0;
  int canvas_test_noise_delta_min = 0;
  int canvas_test_noise_delta_max = 0;
  int canvas_test_noise_max_total_delta = 0;
  std::vector<std::string> canvas_test_noise_allowed_origins;

};

class VirtualEnvironmentManager {
 public:
  VirtualEnvironmentManager() = default;
  VirtualEnvironmentManager(const VirtualEnvironmentManager&) = delete;
  VirtualEnvironmentManager& operator=(const VirtualEnvironmentManager&) = delete;

  // Initialization: reads profile JSON, promotes session geolocation, writes
  // switches to the current process command line (for child-process inheritance).
  void InitFromProfileJSON(const base::FilePath& profile_path);

  // Initialization: reads command-line switches already written by InitFromProfileJSON.
  // Called in each subprocess (browser, renderer, gpu) after forking.
  void InitFromCommandLine();

  // Returns true after InitFromCommandLine() or InitFromProfileJSON() completes.
  bool is_initialized() const { return data_.initialized; }

  // UA-CH fields.
  const std::string& platform() const { return data_.platform; }
  const std::string& platform_version() const { return data_.platform_version; }
  const std::string& architecture() const { return data_.architecture; }
  const std::string& model() const { return data_.model; }
  const std::string& ua_full_version() const { return data_.ua_full_version; }
  const std::string& bitness() const { return data_.bitness; }
  bool wow64() const { return data_.wow64; }
  const FormFactors& form_factors() const { return data_.form_factors; }

  // Locale / timezone.
  const std::string& timezone() const { return data_.timezone; }
  const std::string& accept_languages() const { return data_.accept_languages; }

  // CPU / memory.
  int cpu_cores() const { return data_.cpu_cores; }
  int logical_threads() const { return data_.logical_threads; }
  int64_t memory_mb() const { return data_.memory_mb; }

  // Screen (Phase 2 v2).
  int screen_avail_height() const { return data_.screen_avail_height; }

  // Geolocation permission status override (Phase 5).
  const std::string& permission_geolocation() const {
    return data_.geo_permission_override;
  }

  // M4: hide webdriver flag.
  bool hide_webdriver() const { return data_.hide_webdriver; }

  // Reduced motion preference.
  bool prefers_reduced_motion() const { return data_.prefers_reduced_motion; }

  bool canvas_test_noise_enabled() const {
    return data_.canvas_test_noise_enabled;
  }
  uint64_t canvas_test_noise_seed() const {
    return data_.canvas_test_noise_seed;
  }
  uint32_t canvas_test_noise_case_id() const {
    return data_.canvas_test_noise_case_id;
  }
  double canvas_test_noise_sample_rate() const {
    return data_.canvas_test_noise_sample_rate;
  }
  uint32_t canvas_test_noise_max_pixels() const {
    return data_.canvas_test_noise_max_pixels;
  }
  int canvas_test_noise_delta_min() const {
    return data_.canvas_test_noise_delta_min;
  }
  int canvas_test_noise_delta_max() const {
    return data_.canvas_test_noise_delta_max;
  }
  int canvas_test_noise_max_total_delta() const {
    return data_.canvas_test_noise_max_total_delta;
  }
  const std::vector<std::string>& canvas_test_noise_allowed_origins() const {
    return data_.canvas_test_noise_allowed_origins;
  }

  // Touch simulation (Phase 2 v2).
  // touch_capable: -1 = no override, 0 = desktop (non-touch), 1 = touch.
  // max_touch_points: -1 = no override, >= 0 = maximum touch points.
  int max_touch_points() const { return data_.max_touch_points; }
  int touch_capable() const { return data_.touch_capable; }

  // Audio (Phase 2 v2).
  uint32_t audio_sample_rate() const { return data_.audio_sample_rate; }
  double   audio_base_latency() const { return data_.audio_base_latency; }
  double   audio_output_latency() const { return data_.audio_output_latency; }

  // Battery (Phase 2 v2).
  int battery_is_laptop() const { return data_.battery_is_laptop; }
  int battery_is_mobile() const { return data_.battery_is_mobile; }
  int battery_charging() const { return data_.battery_charging; }
  double battery_level_min() const { return data_.battery_level_min; }
  double battery_level_max() const { return data_.battery_level_max; }

  // WebGL (Phase 1-4).
  const std::string& webgl_vendor() const { return data_.webgl_vendor; }
  const std::string& webgl_renderer() const { return data_.webgl_renderer; }
  const std::string& webgl_version() const { return data_.webgl_version; }

  // Media (Phase 3).
  const std::string& media_devices() const { return data_.media_devices; }
  const std::string& voices_platform_version() const { return data_.voices_platform_version; }
  const std::vector<std::string>& voices() const { return data_.voices; }

  // Geolocation (Phase 5; 2026-07-21).
  // NOTE: geo_latitude == 0.0 is VALID (equator); use has_geolocation() to detect override.
  bool has_geolocation() const {
    return data_.geo_accuracy > 0.0 &&
           data_.geo_latitude != 0.0 &&
           data_.geo_longitude != 0.0;
  }
  double geo_latitude()         const { return data_.geo_latitude; }
  double geo_longitude()        const { return data_.geo_longitude; }
  double geo_accuracy()         const { return data_.geo_accuracy; }
  double geo_altitude()         const { return data_.geo_altitude; }
  double geo_altitude_accuracy() const { return data_.geo_altitude_accuracy; }
  double geo_heading()          const { return data_.geo_heading; }
  double geo_speed()            const { return data_.geo_speed; }

  // Phase 5.c (2026-07-25): ISO 3166-1 alpha-2 country code. Empty = no override.
  bool has_geo_country_code() const { return !data_.geo_country_code.empty(); }
  const std::string& geo_country_code() const { return data_.geo_country_code; }

  // Phase 5.b (2026-07-24): range accessors.
  bool   has_geo_latitude_range()  const { return data_.geo_has_latitude_range; }
  bool   has_geo_longitude_range() const { return data_.geo_has_longitude_range; }
  double geo_latitude_range_min()  const { return data_.geo_latitude_range[0]; }
  double geo_latitude_range_max()  const { return data_.geo_latitude_range[1]; }
  double geo_longitude_range_min() const { return data_.geo_longitude_range[0]; }
  double geo_longitude_range_max() const { return data_.geo_longitude_range[1]; }
  bool   has_geo_accuracy_range() const { return data_.geo_has_accuracy_range; }
  double geo_accuracy_range_min()  const { return data_.geo_accuracy_range[0]; }
  double geo_accuracy_range_max()  const { return data_.geo_accuracy_range[1]; }

  // Phase 5: geolocation permission status override.
  // Returns empty if no override, otherwise "granted"/"prompt"/"denied".
  const std::string& geo_permission_override() const {
    return data_.geo_permission_override;
  }

  // True when geolocation override is active (single-point or range).
  bool has_geolocation_override() const {
    return has_geolocation() || data_.geo_has_latitude_range ||
           data_.geo_has_longitude_range || data_.geo_has_accuracy_range;
  }

 private:
  VirtualEnvironmentData data_;
};

// Reads a fork switch from |cmdline| into |out| if present. Returns true if found.
// Defined in .cc.
bool GetForkSwitch(const base::CommandLine& cmdline,
                  const char* switch_name,
                  std::string* out);

// Global singleton accessor.
VirtualEnvironmentManager* GetVirtualEnvironmentManager();

// Phase 5.c (2026-07-25): ISO 3166-1 alpha-2 country code validator.
// Returns true iff the input is exactly two ASCII letters (case-insensitive).
// Defined in .cc.
bool IsValidIso3166Alpha2(const std::string& code);

// Voices debug logging (2026-07-27). Enabled when the environment variable
// DCHROMIUM_FORK_DEBUG_VOICES is set to a non-empty, non-zero value at
// process startup. Output goes to stderr with the [FORK-DBG-VOICES] prefix
// so that logs can be filtered / collected without polluting DLOG/LOG output.
//
// Scope:
//   - VEM voices resolution path (InitFromCommandLine / InitFromProfileJSON)
//   - speechSynthesis.getVoices() Blinker side
//
// Defined in .cc.
bool ForkVoicesDebugEnabled();

}  // namespace chromium_fork

#endif  // SRC_CHROMIUM_FORK_VIRTUAL_ENVIRONMENT_MANAGER_H_
