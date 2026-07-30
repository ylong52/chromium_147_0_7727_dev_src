// Copyright 2026 Dchromium_fork
// Forward declarations for VirtualEnvironmentManager.
// Include this instead of virtual_environment_manager.h to avoid circular deps
// between embedder_support and chromium_fork.

#ifndef COMPONENTS_EMBEDDER_SUPPORT_VIRTUAL_ENV_FWD_H_
#define COMPONENTS_EMBEDDER_SUPPORT_VIRTUAL_ENV_FWD_H_

namespace chromium_fork {

class VirtualEnvironmentManager;

// Accesses the process-wide VEM instance.
VirtualEnvironmentManager* GetVirtualEnvironmentManager();

}  // namespace chromium_fork

#endif  // COMPONENTS_EMBEDDER_SUPPORT_VIRTUAL_ENV_FWD_H_
