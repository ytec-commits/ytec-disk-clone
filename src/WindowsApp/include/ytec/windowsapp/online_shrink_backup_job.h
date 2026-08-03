#pragma once

#include "ytec/clonecore/log.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/migrationengine/bundle_capture.h"
#include "ytec/vssrequester/windows_backend.h"

#include <cstdint>
#include <string>

namespace ytec::windowsapp {

struct OnlineShrinkBackupJobRequest final {
  diskmodel::DiskInfo selected_source;
  std::wstring final_bundle_directory;
  std::wstring scratch_directory;
  bool administrator{};
  std::uint32_t windows_major{};
  std::uint32_t windows_minor{};
  std::uint32_t windows_build{};
  std::string windows_architecture;
  std::string created_utc;
  std::string app_version;
  vssrequester::AsyncWaitOptions async_wait;
  clonecore::DiskOperationCallbacks callbacks;
  const clonecore::Logger* logger{};
};

struct OnlineShrinkBackupJobReport final {
  vssrequester::WorkflowReport workflow;
  migrationengine::ShrinkBundleCaptureReport bundle;
  bool final_bundle_committed_after_vss_cleanup{};
};

// Online shrink image creation. The source disk is held read-only, DISM reads
// only the exact VSS snapshot device paths, and the visible final .dcmig name
// is committed only after BackupComplete and exact snapshot-set deletion.
[[nodiscard]] clonecore::Result<OnlineShrinkBackupJobReport>
execute_online_shrink_backup_job_with_windows_apis(
    const OnlineShrinkBackupJobRequest& request);

}  // namespace ytec::windowsapp
