#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"

namespace ytec::winpeapp {

// Product WinPE clone gate shared by the read-only job preflight and the
// destructive service. It accepts only an online, fixed, basic GPT/MBR source
// and an empty RAW fixed target with matching 512-byte logical sectors.
// Unknown buses/layouts and Storage Spaces/LDM metadata fail closed.
[[nodiscard]] clonecore::Status validate_clone_execution_observation(
    const diskmodel::DiskInfo& source,
    const diskmodel::DiskInfo& target,
    bool require_target_same_or_larger = true);

}  // namespace ytec::winpeapp
