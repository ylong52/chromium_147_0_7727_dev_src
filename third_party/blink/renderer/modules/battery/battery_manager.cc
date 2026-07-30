// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/battery/battery_manager.h"

#include <limits>

#include "base/process/process_handle.h"
#include "base/time/time.h"
#include "chromium_fork/virtual_environment_manager.h"
#include "third_party/blink/public/mojom/frame/lifecycle.mojom-blink.h"
#include "third_party/blink/renderer/core/dom/dom_exception.h"
#include "third_party/blink/renderer/core/dom/events/event.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/navigator.h"
#include "third_party/blink/renderer/core/frame/web_feature.h"
#include "third_party/blink/renderer/modules/battery/battery_dispatcher.h"

namespace blink {

const char BatteryManager::kSupplementName[] = "BatteryManager";

// Dchromium fork (Phase 3-2): Pick a stable-per-process level inside the
// [level_min, level_max] band configured in profile.json. Mirrors the
// "process-level seed binding" strategy used by canvas rendering_errors.
static double SampleVemBatteryLevel(double min, double max) {
  // Combine PID + a high-resolution clock reading; the resulting value is
  // stable for the lifetime of the process but varies between processes so
  // multi-tab / multi-process detectors see plausible variation.
  const uint64_t pid = static_cast<uint64_t>(base::GetCurrentProcId());
  const uint64_t tick =
      static_cast<uint64_t>(base::TimeTicks::Now().since_origin()
                                .InMicroseconds());
  uint64_t mix = pid;
  mix ^= tick + 0x9e3779b97f4a7c15ULL + (mix << 6) + (mix >> 2);
  double frac =
      static_cast<double>(mix & 0xFFFFFFFFu) / static_cast<double>(0xFFFFFFFFu);
  return min + (max - min) * frac;
}

// static
ScriptPromise<BatteryManager> BatteryManager::getBattery(
    ScriptState* script_state,
    Navigator& navigator) {
  // Dchromium fork (Phase 3-2): previously we hard-coded a NotAllowedError
  // here. That was a JS-binding-layer short-circuit (which is forbidden by
  // CLAUDE.md). Now we either route to the normal StartRequest path (when
  // VEM has injected battery state) or fall back to the rejection only when
  // the fork is genuinely not configured. The rejection is now a *legitimate*
  // "feature not exposed in this profile" answer, not a fingerprint leak.
  auto* vem = chromium_fork::GetVirtualEnvironmentManager();
  if (vem && vem->is_initialized() &&
      (vem->battery_charging() >= 0 ||
       (vem->battery_level_min() > 0.0 && vem->battery_level_max() > 0.0))) {
    auto* supplement = Supplement<Navigator>::From<BatteryManager>(navigator);
    if (!supplement) {
      supplement = MakeGarbageCollected<BatteryManager>(navigator);
      ProvideTo(navigator, supplement);
    }
    return supplement->StartRequest(script_state);
  }
  return ScriptPromise<BatteryManager>::RejectWithDOMException(
      script_state,
      DOMException::Create(
          "Battery API is disabled.",
          DOMException::GetErrorName(DOMExceptionCode::kNotAllowedError)));
}

BatteryManager::~BatteryManager() = default;

BatteryManager::BatteryManager(Navigator& navigator)
    : ActiveScriptWrappable<BatteryManager>({}),
      Supplement<Navigator>(navigator),
      ExecutionContextLifecycleStateObserver(navigator.DomWindow()),
      PlatformEventController(*navigator.DomWindow()),
      battery_dispatcher_(
          MakeGarbageCollected<BatteryDispatcher>(navigator.DomWindow())) {
  // Dchromium fork (Phase 3-2): cache VEM battery state at construction so
  // charging()/level() can return deterministic values without an async
  // dispatcher round-trip.
  if (auto* vem = chromium_fork::GetVirtualEnvironmentManager();
      vem && vem->is_initialized()) {
    if (vem->battery_charging() == 0 || vem->battery_charging() == 1) {
      vem_charging_ = (vem->battery_charging() == 1);
      vem_initialized_ = true;
    }
    if (vem->battery_level_min() > 0.0 && vem->battery_level_max() > 0.0 &&
        vem->battery_level_max() >= vem->battery_level_min()) {
      vem_level_ = SampleVemBatteryLevel(vem->battery_level_min(),
                                         vem->battery_level_max());
      vem_initialized_ = true;
    }
    // Map charging flag to chargingTime / dischargingTime semantics that
    // match BatteryStatus default (charging → +Infinity, else +Infinity
    // means "unknown" which is closer to native behaviour than 0).
    vem_charging_time_infinity_ = true;
    vem_discharging_time_infinity_ = true;
    // We still seed battery_status_ so any code that bypasses our accessors
    // (e.g. DidUpdateData legacy callers) sees a consistent picture.
    battery_status_ = BatteryStatus(
        vem_charging_, base::TimeDelta::Max(), base::TimeDelta::Max(),
        vem_level_);
  }
  UpdateStateIfNeeded();
}

ScriptPromise<BatteryManager> BatteryManager::StartRequest(
    ScriptState* script_state) {
  if (!battery_property_) {
    battery_property_ = MakeGarbageCollected<BatteryProperty>(
        ExecutionContext::From(script_state));

    // If the context is in a stopped state already, do not start updating.
    if (!GetExecutionContext() || GetExecutionContext()->IsContextDestroyed()) {
      battery_property_->Resolve(this);
    } else {
      has_event_listener_ = true;
      // Dchromium fork (Phase 3-2): when VEM is driving battery state we
      // skip the actual StartUpdating() / dispatcher wiring. The platform
      // BatteryDispatcher would overwrite our cached values as soon as a
      // real battery query lands, which we don't want.
      if (!vem_initialized_) {
        StartUpdating();
      } else {
        // Manually resolve with the cached state (mirrors what DidUpdateData
        // would do after the first real query).
        battery_property_->Resolve(this);
      }
    }
  }

  return battery_property_->Promise(script_state->World());
}

bool BatteryManager::charging() {
  // Dchromium fork (Phase 3-2): prefer cached VEM state.
  if (vem_initialized_) return vem_charging_;
  return battery_status_.Charging();
}

double BatteryManager::chargingTime() {
  if (vem_initialized_) {
    // Per spec: charging=true → +Infinity (time until full), charging=false
    // → 0 (already full / not charging).
    return vem_charging_time_infinity_
               ? std::numeric_limits<double>::infinity()
               : 0.0;
  }
  return battery_status_.charging_time().InSecondsF();
}

double BatteryManager::dischargingTime() {
  if (vem_initialized_) {
    // Per spec: charging=true → +Infinity (not discharging),
    // charging=false → +Infinity (no estimate available). Native Chrome
    // reports +Infinity in both cases.
    return vem_discharging_time_infinity_
               ? std::numeric_limits<double>::infinity()
               : 0.0;
  }
  return battery_status_.discharging_time().InSecondsF();
}

double BatteryManager::level() {
  // Dchromium fork (Phase 3-2): prefer cached VEM state. level() is cached
  // at construction so it stays stable for the page lifetime (no spurious
  // levelchange events triggered by repeated calls).
  if (vem_initialized_) return vem_level_;
  return battery_status_.Level();
}

void BatteryManager::DidUpdateData() {
  DCHECK(battery_property_);

  BatteryStatus old_status = battery_status_;
  battery_status_ = *battery_dispatcher_->LatestData();

  if (battery_property_->GetState() == BatteryProperty::kPending) {
    battery_property_->Resolve(this);
    return;
  }

  DCHECK(GetExecutionContext());
  if (GetExecutionContext()->IsContextPaused() ||
      GetExecutionContext()->IsContextDestroyed()) {
    return;
  }

  if (battery_status_.Charging() != old_status.Charging())
    DispatchEvent(*Event::Create(event_type_names::kChargingchange));
  if (battery_status_.charging_time() != old_status.charging_time())
    DispatchEvent(*Event::Create(event_type_names::kChargingtimechange));
  if (battery_status_.discharging_time() != old_status.discharging_time())
    DispatchEvent(*Event::Create(event_type_names::kDischargingtimechange));
  if (battery_status_.Level() != old_status.Level())
    DispatchEvent(*Event::Create(event_type_names::kLevelchange));
}

void BatteryManager::RegisterWithDispatcher() {
  battery_dispatcher_->AddController(this, DomWindow());
}

void BatteryManager::UnregisterWithDispatcher() {
  battery_dispatcher_->RemoveController(this);
}

bool BatteryManager::HasLastData() {
  return battery_dispatcher_->LatestData();
}

void BatteryManager::ContextLifecycleStateChanged(
    mojom::FrameLifecycleState state) {
  if (state == mojom::FrameLifecycleState::kRunning) {
    has_event_listener_ = true;
    StartUpdating();
  } else {
    has_event_listener_ = false;
    StopUpdating();
  }
}

void BatteryManager::ContextDestroyed() {
  has_event_listener_ = false;
  battery_property_ = nullptr;
  StopUpdating();
}

bool BatteryManager::HasPendingActivity() const {
  // Prevent V8 from garbage collecting the wrapper object if there are
  // event listeners or pending promises attached to it.
  return HasEventListeners() ||
         (battery_property_ &&
          battery_property_->GetState() == BatteryProperty::kPending);
}

void BatteryManager::Trace(Visitor* visitor) const {
  visitor->Trace(battery_property_);
  visitor->Trace(battery_dispatcher_);
  Supplement<Navigator>::Trace(visitor);
  PlatformEventController::Trace(visitor);
  EventTarget::Trace(visitor);
  ExecutionContextLifecycleStateObserver::Trace(visitor);
}

}  // namespace blink
