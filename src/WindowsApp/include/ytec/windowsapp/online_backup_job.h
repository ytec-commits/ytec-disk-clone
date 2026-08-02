#pragma once

#include "ytec/diskmodel/physical_disk.h"
#include "ytec/vssrequester/online_backup.h"

#include <functional>
#include <string>
#include <vector>

namespace ytec::windowsapp {

struct OnlineBackupJobRequest final {
  diskmodel::DiskInfo selected_source;
  std::wstring final_path;
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

using ReadOnlyDiskOpener = std::function<clonecore::Result<
    diskmodel::ReadOnlyPhysicalDiskHandle>(
    const clonecore::StableDiskIdentity&)>;

using GptVolumeBindingQuery = std::function<clonecore::Result<
    std::vector<clonecore::VolumeBitmapBinding>>(
    const diskmodel::DiskInfo&,
    const clonecore::GptDisk&)>;

using MbrVolumeBindingQuery = std::function<clonecore::Result<
    std::vector<clonecore::VolumeBitmapBinding>>(
    const diskmodel::DiskInfo&,
    const clonecore::MbrDisk&)>;

using OnlineBackupExecutor = std::function<clonecore::Result<
    vssrequester::OnlineImageBackupReport>(
    const vssrequester::WindowsOnlineImageBackupRequest&)>;

struct OnlineBackupJobDependencies final {
  ReadOnlyDiskOpener open_read_only_disk;
  GptVolumeBindingQuery query_gpt_bindings;
  MbrVolumeBindingQuery query_mbr_bindings;
  OnlineBackupExecutor execute_backup;
};

// Product orchestration seam. It rejects standard-user and non-system-disk
// requests before opening a physical disk, then keeps the verified read-only
// reader alive through metadata capture, VSS planning and final execution.
[[nodiscard]] clonecore::Result<vssrequester::OnlineImageBackupReport>
execute_online_backup_job(
    const OnlineBackupJobRequest& request,
    const OnlineBackupJobDependencies& dependencies);

// Uses only audited Windows adapters. This function never requests elevation;
// a standard-user caller receives a safe access-denied result.
[[nodiscard]] clonecore::Result<vssrequester::OnlineImageBackupReport>
execute_online_backup_job_with_windows_apis(
    const OnlineBackupJobRequest& request);

}  // namespace ytec::windowsapp
