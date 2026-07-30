// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/battery/battery_status_service.h"

#include <cmath>
#include <limits>
#include <memory>
#include <string>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/process/process_handle.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/sequence_local_storage_slot.h"
#include "services/device/battery/battery_monitor_impl.h"
#include "services/device/battery/battery_status_manager.h"

namespace device {

namespace {

// Fork battery override logic.
//
// Command-line switches (set by VirtualEnvironmentManager::InitFromProfileJSON):
//   --dchromium-fork-battery-is-laptop=0/1
//   --dchromium-fork-battery-charging=0/1
//   --dchromium-fork-battery-level-min=N  (0.0..1.0)
//   --dchromium-fork-battery-level-max=N  (0.0..1.0)
//
// Semantics:
//   - is_laptop=0  → desktop AC power: always {charging=true, level=1.0, charging_time=0}
//   - is_laptop=1  → laptop/mobile: apply per-field overrides with jitter
//   - no switches  → use real PowerMonitor (unchanged Chromium behavior)

bool HasBatteryForkOverride() {
  const base::CommandLine& cmd = *base::CommandLine::ForCurrentProcess();
  return cmd.HasSwitch("dchromium-fork-battery-is-laptop") ||
         cmd.HasSwitch("dchromium-fork-battery-charging") ||
         cmd.HasSwitch("dchromium-fork-battery-level-min");
}

mojom::BatteryStatus GetCachedBatteryOverride() {
  const base::CommandLine& cmd = *base::CommandLine::ForCurrentProcess();
  fprintf(stderr, "[FORK-DBG-BATTERY] GetCachedBatteryOverride called: is_laptop_switch=%d pid=%lu\n",
          cmd.HasSwitch("dchromium-fork-battery-is-laptop") ? 1 : 0,
          static_cast<unsigned long>(base::GetCurrentProcId()));
  fflush(stderr);

  mojom::BatteryStatus s;

  int is_laptop = -1;
  if (cmd.HasSwitch("dchromium-fork-battery-is-laptop")) {
    is_laptop =
        cmd.GetSwitchValueASCII("dchromium-fork-battery-is-laptop") == "1" ? 1 : 0;
  }

  if (is_laptop == 0) {
    // Desktop: Chrome on AC power, no battery
    s.charging = true;
    s.level = 1.0;
    s.charging_time = 0.0;
    s.discharging_time = std::numeric_limits<double>::infinity();
    return s;
  }

  // Laptop/mobile: apply per-field overrides
  s.charging =
      !cmd.HasSwitch("dchromium-fork-battery-charging") ||
      cmd.GetSwitchValueASCII("dchromium-fork-battery-charging") == "1";

  double level_min = 0.55;
  double level_max = 0.98;
  if (cmd.HasSwitch("dchromium-fork-battery-level-min")) {
    double v;
    if (base::StringToDouble(
            cmd.GetSwitchValueASCII("dchromium-fork-battery-level-min"), &v)) {
      level_min = std::max(0.0, std::min(1.0, v));
    }
  }
  if (cmd.HasSwitch("dchromium-fork-battery-level-max")) {
    double v;
    if (base::StringToDouble(
            cmd.GetSwitchValueASCII("dchromium-fork-battery-level-max"), &v)) {
      level_max = std::max(0.0, std::min(1.0, v));
    }
  }
  double range = level_max - level_min;
  s.level = (range > 0.0) ? level_min + range * base::RandDouble() : level_min;
  s.level = std::round(s.level * 100.0) / 100.0;

  if (s.charging) {
    s.charging_time = 0.0;
    s.discharging_time = std::numeric_limits<double>::infinity();
  } else {
    s.charging_time = std::numeric_limits<double>::infinity();
  }
  return s;
}

}  // namespace

BatteryStatusService::BatteryStatusService()
    : main_thread_task_runner_(
          base::SingleThreadTaskRunner::GetCurrentDefault()),
      update_callback_(
          base::BindRepeating(&BatteryStatusService::NotifyConsumers,
                              base::Unretained(this))),
      status_updated_(false),
      is_shutdown_(false) {
  callback_list_.set_removal_callback(base::BindRepeating(
      &BatteryStatusService::ConsumersChanged, base::Unretained(this)));
}

BatteryStatusService::~BatteryStatusService() = default;

BatteryStatusService* BatteryStatusService::GetInstance() {
  static base::NoDestructor<BatteryStatusService> service_wrapper;
  return service_wrapper.get();
}

base::CallbackListSubscription BatteryStatusService::AddCallback(
    const BatteryUpdateCallback& callback) {
  DCHECK(main_thread_task_runner_->BelongsToCurrentThread());
  DCHECK(!is_shutdown_);

  if (!battery_fetcher_)
    battery_fetcher_ = BatteryStatusManager::Create(update_callback_);

  // Fork override: pre-populate status_ from command-line switches.
  // Without this, NotifyConsumersOnMainThread() never fires on desktop Windows
  // (no battery hardware → BatteryStatusHost never gets updates).
  if (!status_updated_ && HasBatteryForkOverride()) {
    status_ = GetCachedBatteryOverride();
    status_updated_ = true;
    callback.Run(status_);
    return callback_list_.Add(base::BindRepeating(
        &BatteryStatusService::NotifyConsumers, base::Unretained(this)));
  }

  if (callback_list_.empty()) {
    bool success = battery_fetcher_->StartListeningBatteryChange();
    // On failure pass the default values back.
    if (!success)
      callback.Run(mojom::BatteryStatus());
  }

  if (status_updated_) {
    // Send recent status to the new callback if already available.
    callback.Run(status_);
  }

  return callback_list_.Add(callback);
}

void BatteryStatusService::ConsumersChanged() {
  if (is_shutdown_)
    return;

  if (callback_list_.empty()) {
    battery_fetcher_->StopListeningBatteryChange();
    status_updated_ = false;
  }
}

void BatteryStatusService::NotifyConsumers(const mojom::BatteryStatus& status) {
  DCHECK(!is_shutdown_);

  main_thread_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&BatteryStatusService::NotifyConsumersOnMainThread,
                     base::Unretained(this), status));
}

void BatteryStatusService::NotifyConsumersOnMainThread(
    const mojom::BatteryStatus& status) {
  DCHECK(main_thread_task_runner_->BelongsToCurrentThread());
  if (callback_list_.empty())
    return;

  mojom::BatteryStatus final_status = HasBatteryForkOverride()
                                          ? GetCachedBatteryOverride()
                                          : status;
  status_ = final_status;
  status_updated_ = true;
  callback_list_.Notify(status_);
}

void BatteryStatusService::Shutdown() {
  if (!callback_list_.empty())
    battery_fetcher_->StopListeningBatteryChange();
  battery_fetcher_.reset();
  is_shutdown_ = true;
}

const BatteryStatusService::BatteryUpdateCallback&
BatteryStatusService::GetUpdateCallbackForTesting() const {
  return update_callback_;
}

void BatteryStatusService::SetBatteryManagerForTesting(
    std::unique_ptr<BatteryStatusManager> test_battery_manager) {
  battery_fetcher_ = std::move(test_battery_manager);
  status_ = mojom::BatteryStatus();
  status_updated_ = false;
  is_shutdown_ = false;
  main_thread_task_runner_ = base::SingleThreadTaskRunner::GetCurrentDefault();
}

}  // namespace device
