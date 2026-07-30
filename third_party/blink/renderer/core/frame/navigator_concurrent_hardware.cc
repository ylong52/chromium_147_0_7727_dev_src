// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/frame/navigator_concurrent_hardware.h"

#include "base/system/sys_info.h"
#include "chromium_fork/virtual_environment_manager.h"

namespace blink {

unsigned NavigatorConcurrentHardware::hardwareConcurrency() const {
  const auto* virtual_environment =
      chromium_fork::GetVirtualEnvironmentManager();
  if (virtual_environment && virtual_environment->is_initialized() &&
      virtual_environment->logical_threads() > 0) {
    return static_cast<unsigned>(virtual_environment->logical_threads());
  }
  return static_cast<unsigned>(base::SysInfo::NumberOfProcessors());
}

}  // namespace blink
