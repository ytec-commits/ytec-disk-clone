#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/imageformat/job_manifest.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace ytec::windowsapp {

struct CloneJobCreationRequest final {
  diskmodel::DiskInfo source;
  diskmodel::DiskInfo target;
  bool first_step_acknowledged{};
  std::wstring typed_confirmation;
  imageformat::RequestedConversion requested_conversion{
      imageformat::RequestedConversion::preserve};
  imageformat::TransferMode transfer_mode{imageformat::TransferMode::exact};
  std::wstring shrink_bundle_directory;
  bool auto_execute_once{};
  std::string created_utc;
  std::string app_version;
};

struct RestoreJobCreationRequest final {
  diskmodel::DiskInfo target;
  std::wstring verified_image_path;
  std::uint64_t verified_image_length{};
  imageformat::Sha256Digest verified_image_global_hash{};
  bool first_step_acknowledged{};
  std::wstring typed_confirmation;
  bool auto_execute_once{};
  imageformat::TransferMode transfer_mode{imageformat::TransferMode::exact};
  std::string created_utc;
  std::string app_version;
};

[[nodiscard]] std::wstring clone_job_confirmation_token(
    const diskmodel::DiskInfo& target);

[[nodiscard]] std::wstring restore_job_confirmation_token(
    const diskmodel::DiskInfo& target);

// Creates only an in-memory, hashed WinPE handoff job. It performs no disk or
// file write and requires the exact target-specific two-step confirmation.
[[nodiscard]] clonecore::Result<std::vector<std::byte>>
create_confirmed_clone_job(
    const CloneJobCreationRequest& request);

// Creates only an in-memory, hashed WinPE handoff job. The image must already
// have passed the product's complete read-only preflight. This function never
// opens the image or target disk and never starts a restore.
[[nodiscard]] clonecore::Result<std::vector<std::byte>>
create_confirmed_restore_job(
    const RestoreJobCreationRequest& request);

}  // namespace ytec::windowsapp
