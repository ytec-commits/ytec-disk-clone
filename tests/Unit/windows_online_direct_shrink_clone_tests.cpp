#include "ytec/windowsapp/online_direct_shrink_clone.h"

#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;
constexpr std::uint32_t kSectorSize = 512U;
constexpr std::wstring_view kVolumeOne =
    L"\\\\?\\Volume{11111111-2222-3333-4444-555555555555}\\";
constexpr std::wstring_view kVolumeTwo =
    L"\\\\?\\Volume{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}\\";

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

template <std::size_t Size>
std::array<std::byte, Size> filled(const std::uint8_t value) {
  std::array<std::byte, Size> result{};
  result.fill(static_cast<std::byte>(value));
  return result;
}

ytec::clonecore::Error mock_error(
    const ytec::clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation) {
  return ytec::clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = L"合成失敗",
  };
}

template <typename T>
ytec::clonecore::Result<T> mock_failure(
    const ytec::clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation) {
  return ytec::clonecore::Result<T>::failure(
      mock_error(code, native_code, std::move(operation)));
}

ytec::diskmodel::PartitionInfo partition(
    const std::uint32_t number,
    const std::uint64_t offset,
    const std::uint64_t size,
    std::wstring name,
    const bool active = false) {
  return ytec::diskmodel::PartitionInfo{
      .number = number,
      .offset_bytes = offset,
      .size_bytes = size,
      .style = ytec::diskmodel::PartitionStyle::mbr,
      .type = L"0x07",
      .identifier = L"MBR-" + std::to_wstring(number),
      .name = std::move(name),
      .bootable = active,
  };
}

ytec::diskmodel::PartitionInfo gpt_partition(
    const std::uint32_t number,
    const std::uint64_t offset,
    const std::uint64_t size,
    std::wstring type,
    std::wstring name) {
  return ytec::diskmodel::PartitionInfo{
      .number = number,
      .offset_bytes = offset,
      .size_bytes = size,
      .style = ytec::diskmodel::PartitionStyle::gpt,
      .type = std::move(type),
      .identifier = L"GPT-" + std::to_wstring(number),
      .name = std::move(name),
      .bootable = false,
  };
}

ytec::diskmodel::DiskInfo source_disk(const bool system_disk = false) {
  ytec::diskmodel::DiskInfo disk{
      .disk_number = 2U,
      .device_path = L"\\\\.\\PhysicalDrive2",
      .device_interface_path = L"\\\\?\\SCSI#Disk&Ven_YTEC&Prod_Source",
      .connection_location_path = L"PCIROOT(0)#PCI(0100)",
      .device_instance_id = L"SCSI\\DISK&VEN_YTEC&PROD_SOURCE\\SOURCE-A",
      .model = L"YTEC SYNTHETIC SOURCE",
      .size_bytes = system_disk ? 32ULL * kGiB : 16ULL * kGiB,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = 4096U,
      .bus_type = L"SATA",
      .serial_suffix = "SOURCE01",
      .partition_style = ytec::diskmodel::PartitionStyle::mbr,
      .disk_identifier = L"0x10203040",
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = system_disk,
  };
  disk.sector_count = disk.size_bytes / disk.logical_sector_size;
  if (system_disk) {
    disk.partitions.push_back(partition(
        1U, 1ULL * kMiB, 512ULL * kMiB, L"System", true));
    disk.partitions.push_back(partition(
        2U, 513ULL * kMiB, 8ULL * kGiB, L"Windows"));
  } else {
    disk.partitions.push_back(partition(
        1U, 1ULL * kMiB, 4ULL * kGiB, L"Data"));
  }
  return disk;
}

ytec::diskmodel::DiskInfo target_disk(
    const std::uint64_t size = 8ULL * kGiB) {
  ytec::diskmodel::DiskInfo disk{
      .disk_number = 5U,
      .device_path = L"\\\\.\\PhysicalDrive5",
      .device_interface_path = L"\\\\?\\SCSI#Disk&Ven_YTEC&Prod_Target",
      .connection_location_path = L"PCIROOT(0)#PCI(0200)",
      .device_instance_id = L"SCSI\\DISK&VEN_YTEC&PROD_TARGET\\TARGET-A",
      .model = L"YTEC SYNTHETIC TARGET",
      .size_bytes = size,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = 4096U,
      .bus_type = L"SATA",
      .serial_suffix = "TARGET01",
      .partition_style = ytec::diskmodel::PartitionStyle::raw,
      .disk_identifier = L"RAW",
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = false,
  };
  disk.sector_count = disk.size_bytes / disk.logical_sector_size;
  return disk;
}

ytec::migrationcore::DirectCloneSourcePartition source_partition(
    const std::uint32_t table_index,
    const ytec::migrationcore::MigrationPartitionRole role,
    const std::uint64_t size,
    const std::uint64_t used,
    const bool active = false) {
  return ytec::migrationcore::DirectCloneSourcePartition{
      .partition = ytec::migrationcore::ShrinkSourcePartition{
          .source_table_index = table_index,
          .role = role,
          .file_system = ytec::migrationcore::MigrationFileSystem::ntfs,
          .source_size_bytes = size,
          .used_bytes = used,
          .cluster_size = 4096U,
          .label = role == ytec::migrationcore::MigrationPartitionRole::windows
              ? L"Windows"
              : L"Data",
          .active = active,
      },
      .selected = true,
      .required_for_windows = false,
  };
}

struct Fixture final {
  ytec::diskmodel::DiskInfo source;
  ytec::diskmodel::DiskInfo target;
  ytec::migrationcore::DirectClonePlanningRequest direct_request;
  std::vector<ytec::windowsapp::WindowsDirectShrinkNtfsVolume> volumes;
};

Fixture data_fixture(const std::uint64_t target_size = 8ULL * kGiB) {
  Fixture fixture{
      .source = source_disk(false),
      .target = target_disk(target_size),
      .direct_request = {
          .mode_choice =
              ytec::migrationcore::DirectCloneModeChoice::shrink,
          .partition_style_choice = ytec::migrationcore::
              DirectClonePartitionStyleChoice::preserve,
          .source_style =
              ytec::migrationcore::MigrationPartitionStyle::mbr,
          .source_size_bytes = 16ULL * kGiB,
          .source_logical_sector_size = kSectorSize,
          .target_size_bytes = target_size,
          .target_logical_sector_size = kSectorSize,
          .source_is_windows_system = false,
          .windows_is_amd64 = true,
          .bitlocker_fully_decrypted = true,
          .surplus_allocation = ytec::migrationcore::
              ShrinkSurplusAllocation::leave_unallocated,
          .source_partitions = {
              source_partition(
                  1U,
                  ytec::migrationcore::MigrationPartitionRole::data,
                  4ULL * kGiB,
                  1ULL * kGiB),
          },
      },
      .volumes = {
          ytec::windowsapp::WindowsDirectShrinkNtfsVolume{
              .source_table_index = 1U,
              .source_offset_bytes = 1ULL * kMiB,
              .source_size_bytes = 4ULL * kGiB,
              .original_volume_guid_path = std::wstring(kVolumeOne),
          },
      },
  };
  return fixture;
}

Fixture system_fixture() {
  Fixture fixture{
      .source = source_disk(true),
      .target = target_disk(24ULL * kGiB),
      .direct_request = {
          .mode_choice =
              ytec::migrationcore::DirectCloneModeChoice::shrink,
          .partition_style_choice = ytec::migrationcore::
              DirectClonePartitionStyleChoice::preserve,
          .source_style =
              ytec::migrationcore::MigrationPartitionStyle::mbr,
          .source_size_bytes = 32ULL * kGiB,
          .source_logical_sector_size = kSectorSize,
          .target_size_bytes = 24ULL * kGiB,
          .target_logical_sector_size = kSectorSize,
          .source_is_windows_system = true,
          .windows_is_amd64 = true,
          .bitlocker_fully_decrypted = true,
          .surplus_allocation = ytec::migrationcore::
              ShrinkSurplusAllocation::leave_unallocated,
          .source_partitions = {
              source_partition(
                  1U,
                  ytec::migrationcore::MigrationPartitionRole::bios_system,
                  512ULL * kMiB,
                  100ULL * kMiB,
                  true),
              source_partition(
                  2U,
                  ytec::migrationcore::MigrationPartitionRole::windows,
                  8ULL * kGiB,
                  1ULL * kGiB),
          },
      },
      .volumes = {
          ytec::windowsapp::WindowsDirectShrinkNtfsVolume{
              .source_table_index = 1U,
              .source_offset_bytes = 1ULL * kMiB,
              .source_size_bytes = 512ULL * kMiB,
              .original_volume_guid_path = std::wstring(kVolumeOne),
          },
          ytec::windowsapp::WindowsDirectShrinkNtfsVolume{
              .source_table_index = 2U,
              .source_offset_bytes = 513ULL * kMiB,
              .source_size_bytes = 8ULL * kGiB,
              .original_volume_guid_path = std::wstring(kVolumeTwo),
          },
      },
  };
  return fixture;
}

struct GptProductFixture final {
  ytec::diskmodel::DiskInfo source;
  ytec::diskmodel::DiskInfo target;
  ytec::windowsshrink::ShrinkSourceAnalysis analysis;
  ytec::windowsapp::WindowsDirectShrinkProductPlanningRequest request;
};

std::unique_ptr<GptProductFixture> gpt_product_fixture() {
  constexpr std::uint64_t kEspOffset = 1ULL * kMiB;
  constexpr std::uint64_t kEspSize = 260ULL * kMiB;
  constexpr std::uint64_t kMsrOffset = kEspOffset + kEspSize;
  constexpr std::uint64_t kMsrSize = 16ULL * kMiB;
  constexpr std::uint64_t kWindowsOffset = kMsrOffset + kMsrSize;
  constexpr std::uint64_t kWindowsSize = 40ULL * kGiB;
  constexpr std::uint64_t kRecoveryOffset = kWindowsOffset + kWindowsSize;
  constexpr std::uint64_t kRecoverySize = 1ULL * kGiB;

  auto source = source_disk(true);
  source.size_bytes = 64ULL * kGiB;
  source.sector_count = source.size_bytes / source.logical_sector_size;
  source.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  source.disk_identifier = L"{11111111-2222-3333-4444-555555555555}";
  source.partitions = {
      gpt_partition(1U, kEspOffset, kEspSize, L"EFI System", L"SYSTEM"),
      gpt_partition(2U, kMsrOffset, kMsrSize, L"MSR", L""),
      gpt_partition(
          3U, kWindowsOffset, kWindowsSize, L"Basic data", L"Windows"),
      gpt_partition(
          4U,
          kRecoveryOffset,
          kRecoverySize,
          L"Windows Recovery",
          L"Recovery"),
  };
  auto target = target_disk(24ULL * kGiB);

  auto source_identity_result =
      ytec::diskmodel::make_stable_disk_identity(source, true);
  check(
      source_identity_result.has_value(),
      "synthetic GPT source identity must build");
  auto source_identity = source_identity_result.take_value();
  ytec::windowsshrink::ShrinkSourceAnalysis analysis{
      .source = source_identity,
      .physical_sector_size = source.physical_sector_size,
      .partition_style =
          ytec::migrationcore::MigrationPartitionStyle::gpt,
      .windows_version = ytec::windowsshrink::WindowsSourceVersion{
          .major = 10U,
          .minor = 0U,
          .build = 22631U,
          .architecture = "AMD64",
      },
      .bitlocker_fully_decrypted = true,
      .created_utc = "2026-08-13T00:00:00Z",
      .app_version = "1.0.0-beta",
      .partitions = {
          ytec::windowsshrink::AnalyzedShrinkPartition{
              .source_table_index = 1U,
              .role = ytec::migrationcore::MigrationPartitionRole::efi_system,
              .file_system =
                  ytec::migrationcore::MigrationFileSystem::unsupported,
              .source_offset_bytes = kEspOffset,
              .source_size_bytes = kEspSize,
              .used_bytes = kEspSize,
              .name = L"SYSTEM",
          },
          ytec::windowsshrink::AnalyzedShrinkPartition{
              .source_table_index = 2U,
              .role = ytec::migrationcore::MigrationPartitionRole::
                  microsoft_reserved,
              .file_system = ytec::migrationcore::MigrationFileSystem::none,
              .source_offset_bytes = kMsrOffset,
              .source_size_bytes = kMsrSize,
              .used_bytes = kMsrSize,
          },
          ytec::windowsshrink::AnalyzedShrinkPartition{
              .source_table_index = 3U,
              .role = ytec::migrationcore::MigrationPartitionRole::windows,
              .file_system = ytec::migrationcore::MigrationFileSystem::ntfs,
              .source_offset_bytes = kWindowsOffset,
              .source_size_bytes = kWindowsSize,
              .used_bytes = 8ULL * kGiB,
              .cluster_size = 4096U,
              .label = L"Windows",
              .name = L"Windows",
          },
          ytec::windowsshrink::AnalyzedShrinkPartition{
              .source_table_index = 4U,
              .role = ytec::migrationcore::MigrationPartitionRole::recovery,
              .file_system = ytec::migrationcore::MigrationFileSystem::ntfs,
              .source_offset_bytes = kRecoveryOffset,
              .source_size_bytes = kRecoverySize,
              .used_bytes = 700ULL * kMiB,
              .cluster_size = 4096U,
              .label = L"Recovery",
              .name = L"Recovery",
          },
      },
      .content_volumes = {
          ytec::windowsshrink::AnalyzedShrinkVolume{
              .source_table_index = 3U,
              .volume_guid_path = std::wstring(kVolumeOne),
          },
          ytec::windowsshrink::AnalyzedShrinkVolume{
              .source_table_index = 4U,
              .volume_guid_path = std::wstring(kVolumeTwo),
          },
      },
  };
  auto request = ytec::windowsapp::WindowsDirectShrinkProductPlanningRequest{
      .administrator = true,
      .target_is_active_rescue_media = false,
      .reviewed_source = source,
      .reviewed_target = target,
      .operation_id = filled<16U>(0x7AU),
      .surplus_allocation = ytec::migrationcore::
          ShrinkSurplusAllocation::automatic_proportional,
      .windows_major = 10U,
      .windows_minor = 0U,
      .windows_build = 22631U,
      .windows_architecture = "AMD64",
      .analysis_created_utc = "2026-08-13T00:00:00Z",
      .app_version = "1.0.0-beta",
  };
  return std::make_unique<GptProductFixture>(GptProductFixture{
      .source = std::move(source),
      .target = std::move(target),
      .analysis = std::move(analysis),
      .request = std::move(request),
  });
}

ytec::clonecore::StableDiskIdentity identity(
    const ytec::diskmodel::DiskInfo& disk) {
  auto result = ytec::diskmodel::make_stable_disk_identity(
      disk, disk.is_system_disk);
  check(result.has_value(), "synthetic stable identity must build");
  return result.take_value();
}

ytec::imageformat::Sha256Digest layout_hash(
    const ytec::diskmodel::DiskInfo& disk) {
  auto result = ytec::imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(disk);
  check(result.has_value(), "synthetic layout hash must build");
  return result.take_value();
}

ytec::migrationcore::DirectClonePlan direct_plan(const Fixture& fixture) {
  auto result = ytec::migrationcore::plan_direct_clone(
      fixture.direct_request);
  check(result.has_value(), "synthetic direct shrink plan must build");
  return result.take_value();
}

ytec::windowsapp::WindowsDirectShrinkPlanningRequest planning_request(
    const Fixture& fixture) {
  return ytec::windowsapp::WindowsDirectShrinkPlanningRequest{
      .administrator = true,
      .bitlocker_fully_decrypted = true,
      .target_is_active_rescue_media = false,
      .reviewed_source = fixture.source,
      .reviewed_target = fixture.target,
      .expected_source = identity(fixture.source),
      .expected_target = identity(fixture.target),
      .expected_source_layout_hash = layout_hash(fixture.source),
      .expected_target_layout_hash = layout_hash(fixture.target),
      .operation_id = filled<16U>(0x31U),
      .ntfs_volumes = fixture.volumes,
  };
}

ytec::windowsapp::WindowsDirectShrinkClonePlan product_plan(
    const Fixture& fixture) {
  auto direct = direct_plan(fixture);
  auto request = planning_request(fixture);
  auto result = ytec::windowsapp::build_windows_direct_shrink_clone_plan(
      request, direct);
  check(result.has_value(), "synthetic product shrink plan must build");
  return result.take_value();
}

ytec::diskmodel::ReidentifiedPhysicalClone observation(
    const ytec::windowsapp::WindowsDirectShrinkPlanningRequest& request) {
  return ytec::diskmodel::ReidentifiedPhysicalClone{
      .source = request.reviewed_source,
      .target = request.reviewed_target,
      .source_identity = request.expected_source,
      .target_identity = request.expected_target,
  };
}

struct MockState final {
  std::vector<std::string> events;
  ytec::clonecore::StableDiskIdentity target_identity;
  bool abort_called{};
  bool snapshots_deleted{};
  bool final_commit_called{};
  bool malformed_apply_readback{};
  bool malformed_boot_readback{};
  bool fail_final_commit{};
  bool mismatch_workflow_snapshot_set{};
  bool duplicate_snapshot_device_path{};
  bool fail_capture_capacity{};
  bool partial_archive_cleaned{};
};

class MockPlatform final
    : public ytec::windowsapp::IWindowsDirectShrinkClonePlatform {
 public:
  explicit MockPlatform(std::shared_ptr<MockState> state)
      : state_(std::move(state)) {}

  ytec::clonecore::Result<
      ytec::windowsapp::WindowsDirectShrinkCheckpointEvidence>
  begin_target_owned_staging(
      const ytec::windowsapp::WindowsDirectShrinkClonePlan&,
      const ytec::operationcore::Sha256Digest& operation_plan_hash) override {
    state_->events.push_back("begin_staging");
    return ytec::clonecore::Result<
        ytec::windowsapp::WindowsDirectShrinkCheckpointEvidence>::success({
        .phase = ytec::windowsapp::
            WindowsDirectShrinkCheckpointPhase::prepared,
        .revision = 1U,
        .plan_hash = operation_plan_hash,
        .staging_identity_hash = filled<32U>(0x51U),
        .record_hash = filled<32U>(0x61U),
        .aggregate_write_digest = {},
        .observed_target = state_->target_identity,
        .completed_task_count = 0U,
        .verified_target_bytes = 0U,
        .durable = true,
        .flushed = true,
        .read_back_verified = true,
        .target_offline = true,
        .final_layout_committed = false,
    });
  }

  ytec::clonecore::Result<
      ytec::windowsapp::WindowsDirectShrinkTargetPreparationEvidence>
  prepare_non_archive_partitions_and_verify(
      const std::span<const ytec::windowsapp::
          WindowsDirectShrinkPartitionTask> tasks) override {
    state_->events.push_back("prepare_non_archive");
    const auto count = static_cast<std::uint64_t>(std::count_if(
        tasks.begin(), tasks.end(), [](const auto& task) {
          return task.kind != ytec::windowsapp::
              WindowsDirectShrinkPartitionTaskKind::apply_ntfs_wim;
        }));
    std::uint64_t verified_bytes{};
    for (const auto& task : tasks) {
      if (task.kind != ytec::windowsapp::
              WindowsDirectShrinkPartitionTaskKind::apply_ntfs_wim) {
        verified_bytes += (std::min)(task.target_size_bytes, 1ULL * kMiB);
      }
    }
    return ytec::clonecore::Result<ytec::windowsapp::
        WindowsDirectShrinkTargetPreparationEvidence>::success({
        .prepared_task_count = count,
        .verified_target_bytes = verified_bytes,
        .write_digest = count == 0U ? ytec::imageformat::Sha256Digest{}
                                    : filled<32U>(0x71U),
        .every_write_flushed = true,
        .every_write_read_back = true,
        .target_offline = true,
        .final_layout_committed = false,
    });
  }

  ytec::clonecore::Result<
      ytec::windowsapp::WindowsDirectShrinkStagedArchiveEvidence>
  capture_ntfs_wim_to_owned_staging(
      const ytec::windowsapp::WindowsDirectShrinkPartitionTask& task,
      const ytec::vssrequester::SnapshotMapping& snapshot) override {
    state_->events.push_back(
        "capture_" + std::to_string(task.source_table_index.value_or(0U)));
    if (state_->fail_capture_capacity) {
      state_->partial_archive_cleaned = true;
      state_->events.push_back("discard_partial_wim_handle_bound");
      return mock_failure<ytec::windowsapp::
          WindowsDirectShrinkStagedArchiveEvidence>(
          ytec::clonecore::ErrorCode::io_failed,
          ERROR_DISK_FULL,
          L"合成target-owned WIM容量超過");
    }
    return ytec::clonecore::Result<ytec::windowsapp::
        WindowsDirectShrinkStagedArchiveEvidence>::success({
        .source_table_index = task.source_table_index.value_or(0U),
        .target_number = task.target_number,
        .snapshot_id = snapshot.snapshot_id,
        .snapshot_device_path = snapshot.snapshot_device_path,
        .archive_length = 1ULL * kMiB,
        .archive_hash = filled<32U>(
            static_cast<std::uint8_t>(0x80U + task.target_number)),
        .sealed_no_write_delete_sharing = true,
        .flushed = true,
        .complete_read_back_hash_verified = true,
        .target_offline = true,
    });
  }

  ytec::clonecore::Result<
      ytec::windowsapp::WindowsDirectShrinkAppliedPartitionEvidence>
  apply_staged_ntfs_wim_and_verify(
      const ytec::windowsapp::WindowsDirectShrinkPartitionTask& task,
      const ytec::windowsapp::
          WindowsDirectShrinkStagedArchiveEvidence& archive) override {
    state_->events.push_back(
        "apply_" + std::to_string(task.source_table_index.value_or(0U)));
    return ytec::clonecore::Result<ytec::windowsapp::
        WindowsDirectShrinkAppliedPartitionEvidence>::success({
        .source_table_index = task.source_table_index.value_or(0U),
        .target_number = task.target_number,
        .verified_target_bytes = (std::min)(
            task.source_used_bytes, task.target_size_bytes),
        .archive_hash = archive.archive_hash,
        .target_write_digest = filled<32U>(
            static_cast<std::uint8_t>(0x90U + task.target_number)),
        .every_write_flushed = true,
        .every_write_read_back = !state_->malformed_apply_readback,
        .file_system_metadata_verified = true,
        .target_offline = true,
    });
  }

  ytec::clonecore::Status discard_exact_staged_archive(
      const ytec::windowsapp::
          WindowsDirectShrinkStagedArchiveEvidence& archive) override {
    state_->events.push_back(
        "discard_" + std::to_string(archive.source_table_index));
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Result<
      ytec::windowsapp::WindowsDirectShrinkCheckpointEvidence>
  persist_prepared_partitions_checkpoint(
      const ytec::windowsapp::WindowsDirectShrinkCheckpointEvidence& previous,
      const std::uint64_t completed_task_count,
      const std::uint64_t verified_target_bytes,
      const ytec::imageformat::Sha256Digest& aggregate_write_digest) override {
    state_->events.push_back("persist_prepared_partitions");
    auto current = previous;
    current.phase =
        ytec::windowsapp::WindowsDirectShrinkCheckpointPhase::applying;
    ++current.revision;
    current.record_hash = filled<32U>(
        static_cast<std::uint8_t>(0x60U + current.revision));
    current.completed_task_count = completed_task_count;
    current.verified_target_bytes = verified_target_bytes;
    current.aggregate_write_digest = aggregate_write_digest;
    return ytec::clonecore::Result<ytec::windowsapp::
        WindowsDirectShrinkCheckpointEvidence>::success(std::move(current));
  }

  ytec::clonecore::Result<
      ytec::windowsapp::WindowsDirectShrinkCheckpointEvidence>
  persist_progress_checkpoint(
      const ytec::windowsapp::WindowsDirectShrinkCheckpointEvidence& previous,
      const std::uint64_t completed_task_count,
      const std::uint64_t verified_target_bytes,
      const ytec::imageformat::Sha256Digest& aggregate_write_digest) override {
    state_->events.push_back("persist_progress");
    auto current = previous;
    current.phase =
        ytec::windowsapp::WindowsDirectShrinkCheckpointPhase::applying;
    ++current.revision;
    current.record_hash = filled<32U>(
        static_cast<std::uint8_t>(0x60U + current.revision));
    current.completed_task_count = completed_task_count;
    current.verified_target_bytes = verified_target_bytes;
    current.aggregate_write_digest = aggregate_write_digest;
    return ytec::clonecore::Result<ytec::windowsapp::
        WindowsDirectShrinkCheckpointEvidence>::success(std::move(current));
  }

  ytec::clonecore::Result<ytec::windowsapp::WindowsDirectShrinkBootEvidence>
  finalize_boot_from_staged_layout_and_verify(
      const ytec::windowsapp::WindowsDirectShrinkClonePlan&) override {
    state_->events.push_back("finalize_boot");
    return ytec::clonecore::Result<
        ytec::windowsapp::WindowsDirectShrinkBootEvidence>::success({
        .required = true,
        .completed = true,
        .boot_files_read_back_verified =
            !state_->malformed_boot_readback,
        .recovery_configuration_verified =
            !state_->malformed_boot_readback,
        .target_offline = true,
    });
  }

  ytec::clonecore::Result<
      ytec::windowsapp::WindowsDirectShrinkCheckpointEvidence>
  seal_commit_ready_checkpoint(
      const ytec::windowsapp::WindowsDirectShrinkCheckpointEvidence& previous,
      const std::uint64_t completed_task_count,
      const std::uint64_t verified_target_bytes,
      const ytec::imageformat::Sha256Digest& aggregate_write_digest) override {
    state_->events.push_back("seal_commit_ready");
    auto current = previous;
    current.phase =
        ytec::windowsapp::WindowsDirectShrinkCheckpointPhase::commit_ready;
    ++current.revision;
    current.record_hash = filled<32U>(
        static_cast<std::uint8_t>(0x60U + current.revision));
    current.completed_task_count = completed_task_count;
    current.verified_target_bytes = verified_target_bytes;
    current.aggregate_write_digest = aggregate_write_digest;
    return ytec::clonecore::Result<ytec::windowsapp::
        WindowsDirectShrinkCheckpointEvidence>::success(std::move(current));
  }

  ytec::clonecore::Result<
      ytec::windowsapp::WindowsDirectShrinkCheckpointEvidence>
  revalidate_before_final_commit(
      const ytec::windowsapp::WindowsDirectShrinkClonePlan&,
      const ytec::windowsapp::
          WindowsDirectShrinkCheckpointEvidence& expected) override {
    state_->events.push_back("revalidate_after_snapshot_delete");
    return ytec::clonecore::Result<ytec::windowsapp::
        WindowsDirectShrinkCheckpointEvidence>::success(expected);
  }

  ytec::clonecore::Result<
      ytec::windowsapp::WindowsDirectShrinkFinalCommitEvidence>
  commit_final_layout_last(
      const ytec::windowsapp::WindowsDirectShrinkClonePlan& plan,
      const ytec::windowsapp::
          WindowsDirectShrinkCheckpointEvidence& commit_ready) override {
    state_->events.push_back("commit_final_layout");
    state_->final_commit_called = true;
    if (state_->fail_final_commit || !state_->snapshots_deleted) {
      return mock_failure<ytec::windowsapp::
          WindowsDirectShrinkFinalCommitEvidence>(
          ytec::clonecore::ErrorCode::io_failed,
          ERROR_WRITE_FAULT,
          L"合成最終commit");
    }
    return ytec::clonecore::Result<ytec::windowsapp::
        WindowsDirectShrinkFinalCommitEvidence>::success({
        .committed_layout_hash = plan.final_layout_hash(),
        .aggregate_write_digest = commit_ready.aggregate_write_digest,
        .target_reidentified = true,
        .staging_identity_reverified = true,
        .checkpoint_reverified = true,
        .staging_removed = true,
        .checkpoint_retired = true,
        .hidden_final_layout_published_and_read_back = true,
        .extended_ntfs_partition_count = plan.ntfs_extension_task_count(),
        .every_required_ntfs_extension_verified = true,
        .every_write_flushed = true,
        .every_write_read_back = true,
        .primary_layout_committed_last = true,
        .target_offline = true,
    });
  }

  void abort_keep_offline_incomplete() noexcept override {
    state_->abort_called = true;
    try {
      state_->events.push_back("abort_offline_incomplete");
    } catch (...) {
    }
  }

 private:
  std::shared_ptr<MockState> state_;
};

std::size_t event_index(
    const MockState& state,
    const std::string_view event) {
  const auto found = std::find(
      state.events.begin(), state.events.end(), std::string(event));
  check(found != state.events.end(), "expected event was not recorded");
  return static_cast<std::size_t>(found - state.events.begin());
}

struct ExecutionHarness final {
  ytec::windowsapp::WindowsDirectShrinkCloneDependencies dependencies;
  ytec::windowsapp::WindowsDirectShrinkCloneExecutionOptions options;
  std::shared_ptr<MockState> state;
};

ExecutionHarness harness(
    const ytec::windowsapp::WindowsDirectShrinkPlanningRequest& request) {
  auto state = std::make_shared<MockState>();
  state->target_identity = request.expected_target;
  auto observed = observation(request);
  ExecutionHarness result{
      .dependencies = {},
      .options = {
          .async_wait = {},
          .confirmation = {
              .first_step_acknowledged = true,
              .typed_token = L"OK",
          },
          .callbacks = {},
          .logger = nullptr,
      },
      .state = state,
  };
  result.dependencies.reidentify_selection =
      [state, observed](const auto&, const auto&) mutable {
        state->events.push_back("reidentify_selection");
        return ytec::clonecore::Result<
            ytec::diskmodel::ReidentifiedPhysicalClone>::success(observed);
      };
  result.dependencies.reidentify_confirmed =
      [state, observed](const auto&, const auto&, const auto&) mutable {
        state->events.push_back("reidentify_confirmed");
        return ytec::clonecore::Result<
            ytec::diskmodel::ReidentifiedPhysicalClone>::success(observed);
      };
  result.dependencies.make_platform =
      [state](const auto&, const auto&) {
        state->events.push_back("make_platform");
        std::unique_ptr<ytec::windowsapp::
            IWindowsDirectShrinkClonePlatform> platform =
            std::make_unique<MockPlatform>(state);
        return ytec::clonecore::Result<std::unique_ptr<ytec::windowsapp::
            IWindowsDirectShrinkClonePlatform>>::success(
            std::move(platform));
      };
  result.dependencies.run_snapshot_workflow =
      [state](
          const ytec::vssrequester::WorkflowRequest& workflow,
          const auto&,
          const auto*,
          ytec::vssrequester::SnapshotCopyCallback callback) {
        state->events.push_back("vss_begin");
        ytec::vssrequester::SnapshotCopyContext context{
            .snapshot_set_id = L"SYNTHETIC-SET-A",
        };
        for (std::size_t index = 0U;
             index < workflow.volumes.size();
             ++index) {
          context.mappings.push_back(ytec::vssrequester::SnapshotMapping{
              .original_volume_guid_path =
                  workflow.volumes[index].volume_guid_path,
              .snapshot_id = L"SNAP-" + std::to_wstring(index + 1U),
              .snapshot_device_path =
                  L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy" +
                  std::to_wstring(
                      state->duplicate_snapshot_device_path ? 1U : index + 1U),
          });
        }
        const auto copied = callback(context);
        state->events.push_back("snapshot_callback_returned");
        state->events.push_back("backup_complete");
        state->snapshots_deleted = true;
        state->events.push_back("snapshots_deleted");
        if (!copied) {
          return ytec::clonecore::Result<
              ytec::vssrequester::WorkflowReport>::failure(copied.error());
        }
        return ytec::clonecore::Result<
            ytec::vssrequester::WorkflowReport>::success({
            .snapshot_set_id = state->mismatch_workflow_snapshot_set
                ? L"SYNTHETIC-SET-B"
                : context.snapshot_set_id,
            .volume_count = workflow.volumes.size(),
            .writer_count = 1U,
            .snapshot_data_copied = true,
            .backup_completed = true,
            .snapshots_deleted = true,
        });
      };
  return result;
}

void test_safe_plan_binds_reviewed_extents_and_disjoint_staging() {
  const auto fixture = data_fixture();
  const auto direct = direct_plan(fixture);
  auto request = planning_request(fixture);
  const auto result = ytec::windowsapp::
      build_windows_direct_shrink_clone_plan(request, direct);
  check(result.has_value(), "safe NTFS/unallocated plan should be accepted");
  const auto& plan = result.value();
  check(
      plan.tasks().size() == 1U && plan.archive_task_count() == 1U &&
          plan.tasks()[0].source_table_index == 1U &&
          plan.tasks()[0].source_offset_bytes == 1ULL * kMiB &&
          plan.tasks()[0].source_size_bytes == 4ULL * kGiB &&
          plan.tasks()[0].archive_upper_bound_bytes ==
              plan.staging().archive_capacity_bytes &&
          plan.maximum_archive_upper_bound_bytes() ==
              plan.staging().archive_capacity_bytes &&
          plan.workflow().volumes.size() == 1U,
      "plan must bind the WIM route to the reviewed source extent and exact target-owned capacity");
  const std::uint64_t final_end =
      plan.tasks()[0].target_offset_bytes + plan.tasks()[0].target_size_bytes;
  check(
      plan.staging().offset_bytes == final_end &&
          plan.staging().archive_offset_bytes > plan.staging().offset_bytes &&
          plan.staging().archive_capacity_bytes ==
              plan.maximum_archive_upper_bound_bytes() &&
          plan.staging().offset_bytes + plan.staging().length_bytes <=
              fixture.target.size_bytes - 1ULL * kMiB,
      "target-owned staging must be final-layout-disjoint and preserve tail metadata");
}

void test_product_analysis_builds_representative_gpt_windows_plan() {
  const auto fixture = gpt_product_fixture();
  const auto result = ytec::windowsapp::
      build_windows_direct_shrink_clone_plan_from_analysis(
          fixture->request, fixture->analysis);
  check(
      result.has_value(),
      "representative Windows 10/11 GPT analysis must build a direct shrink plan");
  const auto& plan = result.value();
  const auto role_count = [&](const auto role) {
    return std::count_if(
        plan.tasks().begin(), plan.tasks().end(), [&](const auto& task) {
          return task.role == role;
        });
  };
  const auto task_count = [&](const auto kind) {
    return std::count_if(
        plan.tasks().begin(), plan.tasks().end(), [&](const auto& task) {
          return task.kind == kind;
        });
  };
  check(
      plan.partition_style() ==
              ytec::migrationcore::MigrationPartitionStyle::gpt &&
          plan.boot_finalization_required() &&
          plan.surplus_allocation() == ytec::migrationcore::
              ShrinkSurplusAllocation::automatic_proportional &&
          plan.archive_task_count() == 2U &&
          plan.workflow().volumes.size() == 2U &&
          plan.staging().final_growth_owner_target_number.has_value() &&
          role_count(ytec::migrationcore::MigrationPartitionRole::efi_system) ==
              1U &&
          role_count(
              ytec::migrationcore::MigrationPartitionRole::
                  microsoft_reserved) == 1U &&
          role_count(ytec::migrationcore::MigrationPartitionRole::windows) ==
              1U &&
          role_count(ytec::migrationcore::MigrationPartitionRole::recovery) ==
              1U &&
          task_count(ytec::windowsapp::
                         WindowsDirectShrinkPartitionTaskKind::
                             recreate_efi_system) == 1U &&
          task_count(ytec::windowsapp::
                         WindowsDirectShrinkPartitionTaskKind::
                             recreate_microsoft_reserved) == 1U &&
          task_count(ytec::windowsapp::
                         WindowsDirectShrinkPartitionTaskKind::
                             apply_ntfs_wim) == 2U,
      "product plan must recreate ESP/MSR, capture Windows/Recovery, and reserve verified automatic staging");
}

void test_product_analysis_fails_closed_before_execution() {
  {
    auto fixture = gpt_product_fixture();
    fixture->analysis.windows_version->build += 1U;
    const auto result = ytec::windowsapp::
        build_windows_direct_shrink_clone_plan_from_analysis(
            fixture->request, fixture->analysis);
    check(
        !result.has_value(),
        "reviewed and analyzed Windows builds must match before a plan is shown");
  }
  {
    auto fixture = gpt_product_fixture();
    fixture->analysis.partitions[2].source_size_bytes -= kMiB;
    const auto result = ytec::windowsapp::
        build_windows_direct_shrink_clone_plan_from_analysis(
            fixture->request, fixture->analysis);
    check(
        !result.has_value(),
        "analysis extent drift must not bind a plan to the reviewed source layout");
  }
  {
    auto fixture = gpt_product_fixture();
    fixture->analysis.partitions[0].source_offset_bytes += kMiB;
    const auto result = ytec::windowsapp::
        build_windows_direct_shrink_clone_plan_from_analysis(
            fixture->request, fixture->analysis);
    check(
        !result.has_value(),
        "generated ESP input must still bind the exact reviewed source extent");
  }
  {
    auto fixture = gpt_product_fixture();
    fixture->analysis.partitions[2].used_bytes = 0U;
    const auto result = ytec::windowsapp::
        build_windows_direct_shrink_clone_plan_from_analysis(
            fixture->request, fixture->analysis);
    const bool windows_is_archived = result.has_value() && std::any_of(
        result.value().tasks().begin(),
        result.value().tasks().end(),
        [](const auto& task) {
          return task.role ==
                     ytec::migrationcore::MigrationPartitionRole::windows &&
              task.kind == ytec::windowsapp::
                  WindowsDirectShrinkPartitionTaskKind::apply_ntfs_wim;
        });
    check(
        result.has_value() && result.value().archive_task_count() == 2U &&
            result.value().workflow().volumes.size() == 2U &&
            windows_is_archived,
        "zero used-byte counters are advisory and must not discard an analyzed NTFS archive");
  }
  {
    auto fixture = gpt_product_fixture();
    fixture->request.reviewed_target.size_bytes = 8ULL * kGiB;
    fixture->request.reviewed_target.sector_count =
        fixture->request.reviewed_target.size_bytes /
        fixture->request.reviewed_target.logical_sector_size;
    const auto result = ytec::windowsapp::
        build_windows_direct_shrink_clone_plan_from_analysis(
            fixture->request, fixture->analysis);
    check(
        !result.has_value() && result.error().native_code == ERROR_DISK_FULL,
        "final minimum layout and disjoint staging must fit before VSS or target I/O");
  }
  {
    auto fixture = gpt_product_fixture();
    fixture->request.reviewed_source.partition_style =
        ytec::diskmodel::PartitionStyle::mbr;
    for (auto& partition : fixture->request.reviewed_source.partitions) {
      partition.style = ytec::diskmodel::PartitionStyle::mbr;
    }
    const auto result = ytec::windowsapp::
        plan_windows_direct_shrink_clone_with_windows_apis(fixture->request);
    check(
        !result.has_value() && result.error().native_code == ERROR_NOT_SUPPORTED,
        "MBR preserve and MBR-to-GPT must be rejected before opening a physical source");
  }
}

void test_planner_rejects_unsupported_or_unbound_inputs_before_execution() {
  {
    auto fixture = std::make_unique<Fixture>(data_fixture(5ULL * kGiB));
    const auto direct = direct_plan(*fixture);
    auto request = planning_request(*fixture);
    const auto result = ytec::windowsapp::
        build_windows_direct_shrink_clone_plan(request, direct);
    check(
        result.has_value() &&
            result.value().staging().archive_capacity_bytes <
                result.value().tasks().front().source_size_bytes &&
            result.value().tasks().front().archive_upper_bound_bytes ==
                result.value().staging().archive_capacity_bytes,
        "planner must not require a source-partition-sized WIM reservation");
  }
  {
    auto fixture = std::make_unique<Fixture>(data_fixture(3ULL * kGiB));
    const auto direct = direct_plan(*fixture);
    auto request = planning_request(*fixture);
    const auto result = ytec::windowsapp::
        build_windows_direct_shrink_clone_plan(request, direct);
    check(
        !result.has_value() && result.error().native_code == ERROR_DISK_FULL,
        "a target without the minimum disjoint staging extent must fail before VSS or I/O");
  }
  {
    auto fixture = std::make_unique<Fixture>(data_fixture());
    fixture->direct_request.surplus_allocation = ytec::migrationcore::
        ShrinkSurplusAllocation::automatic_proportional;
    const auto direct = direct_plan(*fixture);
    auto request = planning_request(*fixture);
    const auto automatic =
        ytec::windowsapp::build_windows_direct_shrink_clone_plan(
            request, direct);
    check(
        automatic.has_value() &&
            automatic.value().surplus_allocation() == ytec::migrationcore::
                ShrinkSurplusAllocation::automatic_proportional &&
            automatic.value().ntfs_extension_task_count() != 0U &&
            automatic.value().staging().final_growth_owner_target_number
                .has_value(),
        "automatic surplus must reserve staging inside one reviewed NTFS growth extent");
  }
  {
    auto fixture = std::make_unique<Fixture>(data_fixture());
    const auto direct = direct_plan(*fixture);
    auto request = planning_request(*fixture);
    request.bitlocker_fully_decrypted = false;
    check(
        !ytec::windowsapp::build_windows_direct_shrink_clone_plan(
             request, direct)
             .has_value(),
        "encrypted/merely unlocked NTFS must remain outside this first slice");
  }
  {
    auto fixture = std::make_unique<Fixture>(data_fixture());
    const auto direct = direct_plan(*fixture);
    auto request = planning_request(*fixture);
    request.target_is_active_rescue_media = true;
    check(
        !ytec::windowsapp::build_windows_direct_shrink_clone_plan(
             request, direct)
             .has_value(),
        "the active rescue medium must be rejected before target I/O");
  }
  {
    auto fixture = std::make_unique<Fixture>(data_fixture());
    fixture->target.bus_type = L"RAID";
    const auto direct = direct_plan(*fixture);
    auto request = planning_request(*fixture);
    check(
        !ytec::windowsapp::build_windows_direct_shrink_clone_plan(
             request, direct)
             .has_value(),
        "unresolved RAID targets must fail closed before VSS or target I/O");
  }
  {
    auto fixture = std::make_unique<Fixture>(data_fixture());
    fixture->direct_request.source_partitions[0].partition.file_system =
        ytec::migrationcore::MigrationFileSystem::exfat;
    const auto direct = direct_plan(*fixture);
    auto request = planning_request(*fixture);
    check(
        !ytec::windowsapp::build_windows_direct_shrink_clone_plan(
             request, direct)
             .has_value(),
        "exFAT must not silently enter the NTFS-only executor");
  }
}

void test_planner_rejects_identity_and_sector_inputs_before_execution() {
  {
    auto fixture = std::make_unique<Fixture>(data_fixture());
    fixture->source.read_only = true;
    const auto direct = direct_plan(*fixture);
    auto request = planning_request(*fixture);
    check(
        !ytec::windowsapp::build_windows_direct_shrink_clone_plan(
             request, direct)
             .has_value(),
        "a read-only Windows VSS source must fail before VSS creation");
  }
  {
    auto fixture = std::make_unique<Fixture>(data_fixture());
    fixture->volumes[0].source_offset_bytes += kSectorSize;
    const auto direct = direct_plan(*fixture);
    auto request = planning_request(*fixture);
    check(
        !ytec::windowsapp::build_windows_direct_shrink_clone_plan(
             request, direct)
             .has_value(),
        "a Volume GUID not bound to the reviewed extent must be rejected");
  }
  {
    auto fixture = std::make_unique<Fixture>(data_fixture());
    fixture->target.bus_type = L"USB";
    fixture->target.serial_suffix.clear();
    fixture->target.device_interface_path =
        L"\\\\?\\USBSTOR#Disk&Ven_YTEC&Prod_Target#PORT-A";
    fixture->target.connection_location_path =
        L"PCIROOT(0)#PCI(1400)#USBROOT(0)#USB(3)";
    fixture->target.device_instance_id =
        L"USBSTOR\\DISK&VEN_YTEC&PROD_TARGET\\PORT-A";
    const auto direct = direct_plan(*fixture);
    auto request = planning_request(*fixture);
    check(
        !ytec::windowsapp::build_windows_direct_shrink_clone_plan(
             request, direct)
             .has_value(),
        "serial-less fixed USB must wait for a stateful same-connection adapter");
  }
  {
    auto fixture = std::make_unique<Fixture>(data_fixture());
    fixture->source.logical_sector_size = 4096U;
    fixture->source.sector_count =
        fixture->source.size_bytes / fixture->source.logical_sector_size;
    fixture->target.logical_sector_size = 4096U;
    fixture->target.sector_count =
        fixture->target.size_bytes / fixture->target.logical_sector_size;
    fixture->direct_request.source_logical_sector_size = 4096U;
    fixture->direct_request.target_logical_sector_size = 4096U;
    const auto direct = direct_plan(*fixture);
    auto request = planning_request(*fixture);
    check(
        !ytec::windowsapp::build_windows_direct_shrink_clone_plan(
             request, direct)
             .has_value(),
        "4Kn real I/O must remain outside the initial 512-byte slice");
  }
}

void test_duplicate_volume_guid_is_rejected_before_vss() {
  auto fixture = system_fixture();
  fixture.volumes[1].original_volume_guid_path =
      L"\\\\?\\volume{11111111-2222-3333-4444-555555555555}\\";
  const auto direct = direct_plan(fixture);
  auto request = planning_request(fixture);
  const auto result = ytec::windowsapp::
      build_windows_direct_shrink_clone_plan(request, direct);
  check(
      !result.has_value(),
      "two source partitions must not share one case-insensitive Volume GUID");
}

void test_success_orders_vss_cleanup_before_commit_and_keeps_offline() {
  const auto fixture = system_fixture();
  auto request = planning_request(fixture);
  auto plan = product_plan(fixture);
  auto run = harness(request);
  std::vector<ytec::clonecore::DiskOperationProgress> progress;
  run.options.callbacks.progress = [&](const auto& update) {
    progress.push_back(update);
  };
  const auto result = ytec::windowsapp::execute_windows_direct_shrink_clone(
      plan, run.options, run.dependencies);
  check(result.has_value(), "well-formed mock execution should run");
  check(
      result.value().lifecycle.outcome ==
              ytec::operationcore::OperationOutcome::completed &&
          result.value().execution.has_value() &&
          result.value().execution->applied_archive_count == 2U &&
          result.value().execution->boot.required &&
          result.value().execution->target_left_offline &&
          !run.state->abort_called,
      "success requires every archive, boot evidence, and offline completion");
  check(
      !progress.empty() &&
          std::any_of(
              progress.begin(), progress.end(), [](const auto& update) {
                return update.stage ==
                        ytec::clonecore::DiskOperationStage::copying_data &&
                    update.verified_bytes != 0U;
              }) &&
          std::any_of(
              progress.begin(), progress.end(), [](const auto& update) {
                return update.stage == ytec::clonecore::
                    DiskOperationStage::committing_partition_table &&
                    !update.cancellation_allowed && !update.pause_allowed;
              }) &&
          std::any_of(
              progress.begin(), progress.end(), [](const auto& update) {
                return update.stage ==
                    ytec::clonecore::DiskOperationStage::completed;
              }),
      "progress must expose verified bytes and a non-cancellable commit interval");
  check(
      event_index(*run.state, "begin_staging") <
              event_index(*run.state, "capture_1") &&
          event_index(*run.state, "capture_1") <
              event_index(*run.state, "apply_1") &&
          event_index(*run.state, "apply_1") <
              event_index(*run.state, "discard_1") &&
          event_index(*run.state, "discard_1") <
              event_index(*run.state, "capture_2") &&
          event_index(*run.state, "capture_2") <
              event_index(*run.state, "apply_2") &&
          event_index(*run.state, "apply_2") <
              event_index(*run.state, "discard_2") &&
          event_index(*run.state, "seal_commit_ready") <
              event_index(*run.state, "snapshots_deleted") &&
          event_index(*run.state, "snapshots_deleted") <
              event_index(*run.state, "revalidate_after_snapshot_delete") &&
          event_index(*run.state, "revalidate_after_snapshot_delete") <
              event_index(*run.state, "commit_final_layout"),
      "target publication must be last, after VSS cleanup and fresh revalidation");
}

void test_cancellation_aborts_offline_without_capture_or_commit() {
  const auto fixture = data_fixture();
  auto request = planning_request(fixture);
  auto plan = product_plan(fixture);
  auto run = harness(request);
  run.options.callbacks.safe_boundary = [](const auto&) {
    return ytec::clonecore::DiskOperationControlDecision::cancel_operation;
  };
  const auto result = ytec::windowsapp::execute_windows_direct_shrink_clone(
      plan, run.options, run.dependencies);
  check(result.has_value(), "cancelled lifecycle should return a report");
  check(
      result.value().lifecycle.outcome ==
              ytec::operationcore::OperationOutcome::cancelled &&
          !result.value().execution.has_value() && run.state->abort_called &&
          std::find(
              run.state->events.begin(),
              run.state->events.end(),
              "capture_1") == run.state->events.end() &&
          !run.state->final_commit_called,
      "cancellation at the first safe boundary must abort incomplete/offline");
}

void test_target_owned_archive_capacity_failure_cleans_up_without_commit() {
  const auto fixture = data_fixture(5ULL * kGiB);
  auto request = planning_request(fixture);
  auto plan = product_plan(fixture);
  auto run = harness(request);
  run.state->fail_capture_capacity = true;
  const auto result = ytec::windowsapp::execute_windows_direct_shrink_clone(
      plan, run.options, run.dependencies);
  check(
      result.has_value() &&
          result.value().lifecycle.outcome ==
              ytec::operationcore::OperationOutcome::failed &&
          run.state->partial_archive_cleaned && run.state->abort_called &&
          run.state->snapshots_deleted && !run.state->final_commit_called,
      "DISM capacity exhaustion must clean the owned partial, abort, delete VSS, and withhold publication");
  check(
      event_index(*run.state, "capture_1") <
              event_index(*run.state, "discard_partial_wim_handle_bound") &&
          event_index(*run.state, "discard_partial_wim_handle_bound") <
              event_index(*run.state, "abort_offline_incomplete") &&
          event_index(*run.state, "abort_offline_incomplete") <
              event_index(*run.state, "snapshot_callback_returned") &&
          event_index(*run.state, "snapshot_callback_returned") <
              event_index(*run.state, "snapshots_deleted"),
      "capacity failure evidence must order handle cleanup and invalidation before VSS deletion");
}

void test_malformed_apply_or_boot_evidence_never_commits() {
  {
    const auto fixture = data_fixture();
    auto request = planning_request(fixture);
    auto plan = product_plan(fixture);
    auto run = harness(request);
    run.state->malformed_apply_readback = true;
    const auto result = ytec::windowsapp::execute_windows_direct_shrink_clone(
        plan, run.options, run.dependencies);
    check(
        result.has_value() &&
            result.value().lifecycle.outcome ==
                ytec::operationcore::OperationOutcome::failed &&
            run.state->abort_called && !run.state->final_commit_called &&
            std::find(
                run.state->events.begin(),
                run.state->events.end(),
                "discard_1") != run.state->events.end(),
        "missing target readback evidence must abort without publication");
  }
  {
    const auto fixture = system_fixture();
    auto request = planning_request(fixture);
    auto plan = product_plan(fixture);
    auto run = harness(request);
    run.state->malformed_boot_readback = true;
    const auto result = ytec::windowsapp::execute_windows_direct_shrink_clone(
        plan, run.options, run.dependencies);
    check(
        result.has_value() &&
            result.value().lifecycle.outcome ==
                ytec::operationcore::OperationOutcome::failed &&
            run.state->abort_called && !run.state->final_commit_called,
        "missing boot readback evidence must abort before commit-ready");
  }
}

void test_snapshot_set_mismatch_or_commit_failure_stays_incomplete() {
  {
    const auto fixture = data_fixture();
    auto request = planning_request(fixture);
    auto plan = product_plan(fixture);
    auto run = harness(request);
    run.state->mismatch_workflow_snapshot_set = true;
    const auto result = ytec::windowsapp::execute_windows_direct_shrink_clone(
        plan, run.options, run.dependencies);
    check(
        result.has_value() &&
            result.value().lifecycle.outcome ==
                ytec::operationcore::OperationOutcome::failed &&
            run.state->abort_called && !run.state->final_commit_called,
        "callback/report Snapshot Set mismatch must stop before publication");
  }
  {
    const auto fixture = data_fixture();
    auto request = planning_request(fixture);
    auto plan = product_plan(fixture);
    auto run = harness(request);
    run.state->fail_final_commit = true;
    const auto result = ytec::windowsapp::execute_windows_direct_shrink_clone(
        plan, run.options, run.dependencies);
    check(
        result.has_value() &&
            result.value().lifecycle.outcome ==
                ytec::operationcore::OperationOutcome::failed &&
            run.state->abort_called && run.state->final_commit_called,
        "final commit failure must leave the target offline and incomplete");
  }
}

void test_duplicate_snapshot_mapping_stops_before_target_io() {
  const auto fixture = system_fixture();
  auto request = planning_request(fixture);
  auto plan = product_plan(fixture);
  auto run = harness(request);
  run.state->duplicate_snapshot_device_path = true;
  const auto result = ytec::windowsapp::execute_windows_direct_shrink_clone(
      plan, run.options, run.dependencies);
  check(
      result.has_value() &&
          result.value().lifecycle.outcome ==
              ytec::operationcore::OperationOutcome::failed &&
          std::find(
              run.state->events.begin(),
              run.state->events.end(),
              "make_platform") != run.state->events.end() &&
          std::find(
              run.state->events.begin(),
              run.state->events.end(),
              "begin_staging") == run.state->events.end(),
      "duplicate Snapshot device paths must fail after pure factory preflight but before target I/O");
}

void test_platform_preflight_rejection_stops_before_vss() {
  const auto fixture = data_fixture();
  auto request = planning_request(fixture);
  auto plan = product_plan(fixture);
  auto run = harness(request);
  run.dependencies.make_platform =
      [state = run.state](const auto&, const auto&) {
        state->events.push_back("make_platform_rejected");
        return mock_failure<std::unique_ptr<
            ytec::windowsapp::IWindowsDirectShrinkClonePlatform>>(
                ytec::clonecore::ErrorCode::unsupported_layout,
                ERROR_NOT_SUPPORTED,
                L"合成production preflight");
      };
  const auto result = ytec::windowsapp::execute_windows_direct_shrink_clone(
      plan, run.options, run.dependencies);
  check(
      result.has_value() &&
          result.value().lifecycle.outcome ==
              ytec::operationcore::OperationOutcome::failed &&
          std::find(
              run.state->events.begin(),
              run.state->events.end(),
              "make_platform_rejected") != run.state->events.end() &&
          std::find(
              run.state->events.begin(),
              run.state->events.end(),
              "vss_begin") == run.state->events.end(),
      "production factory rejection must prevent Snapshot Set creation");
}

void test_confirmation_and_layout_drift_stop_before_target_platform() {
  {
    const auto fixture = data_fixture();
    auto request = planning_request(fixture);
    auto plan = product_plan(fixture);
    auto run = harness(request);
    run.options.confirmation.first_step_acknowledged = false;
    const auto result = ytec::windowsapp::execute_windows_direct_shrink_clone(
        plan, run.options, run.dependencies);
    check(
        !result.has_value() &&
            result.error().code ==
                ytec::clonecore::ErrorCode::invalid_argument &&
            std::find(
                run.state->events.begin(),
                run.state->events.end(),
                "make_platform") == run.state->events.end(),
        "first-step acknowledgement must be revalidated before target I/O");
  }
  {
    const auto fixture = data_fixture();
    auto request = planning_request(fixture);
    auto plan = product_plan(fixture);
    auto run = harness(request);
    auto drifted = observation(request);
    drifted.target.partition_style = ytec::diskmodel::PartitionStyle::mbr;
    drifted.target.partitions.push_back(partition(
        1U, 1ULL * kMiB, 1ULL * kGiB, L"Unexpected"));
    run.dependencies.reidentify_selection =
        [state = run.state, drifted](const auto&, const auto&) mutable {
          state->events.push_back("reidentify_selection");
          return ytec::clonecore::Result<
              ytec::diskmodel::ReidentifiedPhysicalClone>::success(drifted);
        };
    const auto result = ytec::windowsapp::execute_windows_direct_shrink_clone(
        plan, run.options, run.dependencies);
    check(
        result.has_value() &&
            result.value().lifecycle.outcome ==
                ytec::operationcore::OperationOutcome::failed &&
            std::find(
                run.state->events.begin(),
                run.state->events.end(),
                "vss_begin") == run.state->events.end(),
        "layout drift at OperationCore reidentification must prevent VSS and I/O");
  }
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"safe_plan_binds_reviewed_extents_and_disjoint_staging",
       test_safe_plan_binds_reviewed_extents_and_disjoint_staging},
      {"product_analysis_builds_representative_gpt_windows_plan",
       test_product_analysis_builds_representative_gpt_windows_plan},
      {"product_analysis_fails_closed_before_execution",
       test_product_analysis_fails_closed_before_execution},
      {"planner_rejects_unsupported_or_unbound_inputs_before_execution",
       test_planner_rejects_unsupported_or_unbound_inputs_before_execution},
      {"planner_rejects_identity_and_sector_inputs_before_execution",
       test_planner_rejects_identity_and_sector_inputs_before_execution},
      {"duplicate_volume_guid_is_rejected_before_vss",
       test_duplicate_volume_guid_is_rejected_before_vss},
      {"success_orders_vss_cleanup_before_commit_and_keeps_offline",
       test_success_orders_vss_cleanup_before_commit_and_keeps_offline},
      {"cancellation_aborts_offline_without_capture_or_commit",
       test_cancellation_aborts_offline_without_capture_or_commit},
      {"target_owned_archive_capacity_failure_cleans_up_without_commit",
       test_target_owned_archive_capacity_failure_cleans_up_without_commit},
      {"malformed_apply_or_boot_evidence_never_commits",
       test_malformed_apply_or_boot_evidence_never_commits},
      {"snapshot_set_mismatch_or_commit_failure_stays_incomplete",
       test_snapshot_set_mismatch_or_commit_failure_stays_incomplete},
      {"duplicate_snapshot_mapping_stops_before_target_io",
       test_duplicate_snapshot_mapping_stops_before_target_io},
      {"platform_preflight_rejection_stops_before_vss",
       test_platform_preflight_rejection_stops_before_vss},
      {"confirmation_and_layout_drift_stop_before_target_platform",
       test_confirmation_and_layout_drift_stop_before_target_platform},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << failure.message << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}
