#include "ytec/winpeapp/restore_execution_readiness.h"

#include <algorithm>
#include <cwctype>

namespace ytec::winpeapp {
namespace {

constexpr std::wstring_view kLdmMetadataType =
    L"{5808C8AA-7E8F-42E0-85D2-E1E90434CFB3}";
constexpr std::wstring_view kLdmDataType =
    L"{AF9B60A0-1431-4F62-BC68-3311714A69AD}";
constexpr std::wstring_view kStorageSpacesProtectiveType =
    L"{E75CAF8F-F680-4CEE-AFA3-B001E56EFC2D}";

bool equals_case_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  return left.size() == right.size() &&
         std::ranges::equal(
             left,
             right,
             [](const wchar_t left_character,
                const wchar_t right_character) {
               return std::towupper(left_character) ==
                      std::towupper(right_character);
             });
}

bool has_partition_type(
    const diskmodel::DiskInfo& target,
    const std::wstring_view type) noexcept {
  return std::ranges::any_of(
      target.partitions,
      [&](const diskmodel::PartitionInfo& partition) {
        return equals_case_insensitive(partition.type, type);
      });
}

bool image_file_system_layout_supported(
    const imageformat::RestoreImageInspectionReport& image) noexcept {
  if (!image.complete_container_verified ||
      !image.metadata_verified ||
      !image.restore_layout_verified ||
      image.manifest.partitions.empty()) {
    return false;
  }
  return std::ranges::all_of(
      image.manifest.partitions,
      [](const imageformat::BackupManifestPartition& partition) {
        switch (partition.file_system) {
          case imageformat::BackupFileSystem::none:
            return partition.role ==
                   imageformat::BackupPartitionRole::microsoft_reserved;
          case imageformat::BackupFileSystem::ntfs:
            return partition.role ==
                       imageformat::BackupPartitionRole::windows_ntfs ||
                   partition.role ==
                       imageformat::BackupPartitionRole::recovery_ntfs ||
                   partition.role ==
                       imageformat::BackupPartitionRole::ntfs_data;
          case imageformat::BackupFileSystem::fat32:
            return partition.role ==
                       imageformat::BackupPartitionRole::efi_system ||
                   partition.role ==
                       imageformat::BackupPartitionRole::fat32_data;
        }
        return false;
      });
}

}  // namespace

RestoreExecutionSafetyObservation
derive_restore_execution_safety_observation(
    const diskmodel::DiskInfo& target,
    const imageformat::RestoreImageInspectionReport& image,
    const RestoreSafetyState power_state) noexcept {
  const bool dynamic_disk =
      has_partition_type(target, L"0x42") ||
      has_partition_type(target, kLdmMetadataType) ||
      has_partition_type(target, kLdmDataType);
  const bool storage_spaces =
      equals_case_insensitive(target.bus_type, L"Storage Spaces") ||
      has_partition_type(target, kStorageSpacesProtectiveType);
  const bool storage_bus_known =
      !target.bus_type.empty() &&
      !equals_case_insensitive(target.bus_type, L"Unknown");

  return RestoreExecutionSafetyObservation{
      .bitlocker_fully_decrypted =
          image.manifest.bitlocker_fully_decrypted
              ? RestoreSafetyState::passed
              : RestoreSafetyState::blocked,
      .dynamic_disk_absent =
          dynamic_disk ? RestoreSafetyState::blocked
                       : RestoreSafetyState::passed,
      .storage_spaces_absent =
          storage_spaces
              ? RestoreSafetyState::blocked
              : (storage_bus_known ? RestoreSafetyState::passed
                                   : RestoreSafetyState::unknown),
      .file_system_layout_supported =
          image_file_system_layout_supported(image)
              ? RestoreSafetyState::passed
              : RestoreSafetyState::blocked,
      .stable_power = power_state,
      .pending_restart_absent = RestoreSafetyState::unknown,
  };
}

RestoreExecutionReadinessReport evaluate_restore_execution_readiness(
    const RestoreExecutionSafetyObservation& observation,
    const RestoreExecutionSafetyPolicy& policy) noexcept {
  RestoreExecutionReadinessReport report{
      .checks =
          {
              RestoreSafetyFinding{
                  .check =
                      RestoreSafetyCheck::bitlocker_fully_decrypted,
                  .state = observation.bitlocker_fully_decrypted,
                  .required = true,
              },
              RestoreSafetyFinding{
                  .check = RestoreSafetyCheck::dynamic_disk_absent,
                  .state = observation.dynamic_disk_absent,
                  .required = true,
              },
              RestoreSafetyFinding{
                  .check = RestoreSafetyCheck::storage_spaces_absent,
                  .state = observation.storage_spaces_absent,
                  .required = true,
              },
              RestoreSafetyFinding{
                  .check =
                      RestoreSafetyCheck::file_system_layout_supported,
                  .state = observation.file_system_layout_supported,
                  .required = true,
              },
              RestoreSafetyFinding{
                  .check = RestoreSafetyCheck::stable_power,
                  .state = observation.stable_power,
                  .required = policy.require_stable_power,
              },
              RestoreSafetyFinding{
                  .check = RestoreSafetyCheck::pending_restart_absent,
                  .state = observation.pending_restart_absent,
                  .required = false,
              },
          },
  };
  report.required_checks_passed = std::ranges::all_of(
      report.checks,
      [](const RestoreSafetyFinding& finding) {
        return !finding.required ||
               finding.state == RestoreSafetyState::passed;
      });
  report.all_checks_passed = std::ranges::all_of(
      report.checks,
      [](const RestoreSafetyFinding& finding) {
        return finding.state == RestoreSafetyState::passed;
      });
  return report;
}

std::string_view restore_safety_state_name(
    const RestoreSafetyState state) noexcept {
  switch (state) {
    case RestoreSafetyState::passed:
      return "passed";
    case RestoreSafetyState::blocked:
      return "blocked";
    case RestoreSafetyState::unknown:
      return "unknown";
  }
  return "unknown";
}

std::string_view restore_safety_check_id(
    const RestoreSafetyCheck check) noexcept {
  switch (check) {
    case RestoreSafetyCheck::bitlocker_fully_decrypted:
      return "bitLockerFullyDecrypted";
    case RestoreSafetyCheck::dynamic_disk_absent:
      return "dynamicDiskAbsent";
    case RestoreSafetyCheck::storage_spaces_absent:
      return "storageSpacesAbsent";
    case RestoreSafetyCheck::file_system_layout_supported:
      return "fileSystemLayoutSupported";
    case RestoreSafetyCheck::stable_power:
      return "stablePower";
    case RestoreSafetyCheck::pending_restart_absent:
      return "pendingRestartAbsent";
  }
  return "unknown";
}

std::string_view restore_safety_check_label_ja(
    const RestoreSafetyCheck check) noexcept {
  switch (check) {
    case RestoreSafetyCheck::bitlocker_fully_decrypted:
      return "BitLocker完全復号";
    case RestoreSafetyCheck::dynamic_disk_absent:
      return "動的ディスクではない";
    case RestoreSafetyCheck::storage_spaces_absent:
      return "Storage Spaces構成ではない";
    case RestoreSafetyCheck::file_system_layout_supported:
      return "対応ファイルシステム構成";
    case RestoreSafetyCheck::stable_power:
      return "安定電源";
    case RestoreSafetyCheck::pending_restart_absent:
      return "再起動保留なし";
  }
  return "不明な検査";
}

}  // namespace ytec::winpeapp
