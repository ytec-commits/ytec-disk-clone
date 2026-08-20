#include "ytec/windowsapp/windows_direct_shrink_clone_platform.h"

#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"
#include "ytec/migrationcore/direct_clone_plan.h"
#include "ytec/operationcore/operation.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;
constexpr std::uint32_t kSectorSize = 512U;
constexpr std::wstring_view kSourceVolume =
    L"\\\\?\\Volume{11111111-2222-3333-4444-555555555555}\\";
constexpr std::wstring_view kRecoveryVolume =
    L"\\\\?\\Volume{66666666-7777-8888-9999-AAAAAAAAAAAA}\\";

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <std::size_t Size>
std::array<std::byte, Size> filled(const std::uint8_t value) {
  std::array<std::byte, Size> result{};
  result.fill(static_cast<std::byte>(value));
  return result;
}

ytec::clonecore::Error injected_error(
    const ytec::clonecore::ErrorCode code,
    std::wstring operation) {
  return {
      .code = code,
      .native_code = ERROR_GEN_FAILURE,
      .operation = std::move(operation),
      .message = L"合成失敗",
  };
}

template <typename T>
ytec::clonecore::Result<T> injected_failure(
    const ytec::clonecore::ErrorCode code,
    std::wstring operation) {
  return ytec::clonecore::Result<T>::failure(
      injected_error(code, std::move(operation)));
}

ytec::diskmodel::PartitionInfo data_partition(
    const ytec::diskmodel::PartitionStyle style) {
  return {
      .number = 1U,
      .offset_bytes = 1ULL * kMiB,
      .size_bytes = 4ULL * kGiB,
      .style = style,
      .type = style == ytec::diskmodel::PartitionStyle::gpt
          ? L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}"
          : L"0x07",
      .identifier = style == ytec::diskmodel::PartitionStyle::gpt
          ? L"{11111111-1111-1111-1111-111111111111}"
          : L"0x10203040-1",
      .name = L"Data",
      .bootable = false,
  };
}

ytec::diskmodel::DiskInfo source_disk(
    const ytec::diskmodel::PartitionStyle style) {
  ytec::diskmodel::DiskInfo result{
      .disk_number = 2U,
      .device_path = L"\\\\.\\PhysicalDrive2",
      .device_interface_path = L"\\\\?\\SCSI#Disk&Ven_YTEC&Prod_Source",
      .connection_location_path = L"PCIROOT(0)#PCI(0100)",
      .device_instance_id = L"SCSI\\DISK&VEN_YTEC&PROD_SOURCE\\SOURCE-A",
      .model = L"YTEC SYNTHETIC SOURCE",
      .size_bytes = 16ULL * kGiB,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = 4096U,
      .bus_type = L"SATA",
      .serial_suffix = "SOURCE01",
      .partition_style = style,
      .disk_identifier = style == ytec::diskmodel::PartitionStyle::gpt
          ? L"{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}"
          : L"0x10203040",
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = false,
  };
  result.sector_count = result.size_bytes / result.logical_sector_size;
  result.partitions.push_back(data_partition(style));
  return result;
}

ytec::diskmodel::DiskInfo target_disk(
    const std::uint64_t size_bytes = 12ULL * kGiB) {
  ytec::diskmodel::DiskInfo result{
      .disk_number = 5U,
      .device_path = L"\\\\.\\PhysicalDrive5",
      .device_interface_path = L"\\\\?\\SCSI#Disk&Ven_YTEC&Prod_Target",
      .connection_location_path = L"PCIROOT(0)#PCI(0200)",
      .device_instance_id = L"SCSI\\DISK&VEN_YTEC&PROD_TARGET\\TARGET-A",
      .model = L"YTEC SYNTHETIC TARGET",
      .size_bytes = size_bytes,
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
  result.sector_count = result.size_bytes / result.logical_sector_size;
  return result;
}

ytec::clonecore::StableDiskIdentity stable_identity(
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

struct Fixture final {
  ytec::diskmodel::DiskInfo source;
  ytec::diskmodel::DiskInfo target;
  ytec::clonecore::StableDiskIdentity source_identity;
  ytec::clonecore::StableDiskIdentity target_identity;
  ytec::imageformat::Sha256Digest source_layout{};
  ytec::imageformat::Sha256Digest target_layout{};
  ytec::windowsapp::WindowsDirectShrinkClonePlan plan;
};

Fixture fixture(
    const ytec::diskmodel::PartitionStyle source_style,
    const ytec::migrationcore::ShrinkSurplusAllocation allocation =
        ytec::migrationcore::ShrinkSurplusAllocation::leave_unallocated) {
  auto source = source_disk(source_style);
  auto target = target_disk();
  ytec::migrationcore::DirectClonePlanningRequest direct_request{
      .mode_choice = ytec::migrationcore::DirectCloneModeChoice::shrink,
      .partition_style_choice = ytec::migrationcore::
          DirectClonePartitionStyleChoice::preserve,
      .source_style = source_style == ytec::diskmodel::PartitionStyle::gpt
          ? ytec::migrationcore::MigrationPartitionStyle::gpt
          : ytec::migrationcore::MigrationPartitionStyle::mbr,
      .source_size_bytes = source.size_bytes,
      .source_logical_sector_size = kSectorSize,
      .target_size_bytes = target.size_bytes,
      .target_logical_sector_size = kSectorSize,
      .source_is_windows_system = false,
      .windows_is_amd64 = true,
      .bitlocker_fully_decrypted = true,
      .surplus_allocation = allocation,
      .source_partitions = {
          ytec::migrationcore::DirectCloneSourcePartition{
              .partition = {
                  .source_table_index = 1U,
                  .role = ytec::migrationcore::MigrationPartitionRole::data,
                  .file_system =
                      ytec::migrationcore::MigrationFileSystem::ntfs,
                  .source_size_bytes = 4ULL * kGiB,
                  .used_bytes = 1ULL * kGiB,
                  .cluster_size = 4096U,
                  .label = L"Data",
                  .active = false,
              },
              .selected = true,
              .required_for_windows = false,
          },
      },
  };
  auto direct = ytec::migrationcore::plan_direct_clone(direct_request);
  check(direct.has_value(), "synthetic direct plan must build");
  auto source_id = stable_identity(source);
  auto target_id = stable_identity(target);
  auto source_digest = layout_hash(source);
  auto target_digest = layout_hash(target);
  ytec::windowsapp::WindowsDirectShrinkPlanningRequest product_request{
      .administrator = true,
      .bitlocker_fully_decrypted = true,
      .target_is_active_rescue_media = false,
      .reviewed_source = source,
      .reviewed_target = target,
      .expected_source = source_id,
      .expected_target = target_id,
      .expected_source_layout_hash = source_digest,
      .expected_target_layout_hash = target_digest,
      .operation_id = filled<16U>(0x31U),
      .ntfs_volumes = {
          {
              .source_table_index = 1U,
              .source_offset_bytes = 1ULL * kMiB,
              .source_size_bytes = 4ULL * kGiB,
              .original_volume_guid_path = std::wstring(kSourceVolume),
          },
      },
  };
  auto plan = ytec::windowsapp::build_windows_direct_shrink_clone_plan(
      product_request, direct.value());
  check(plan.has_value(), "synthetic product plan must build");
  return {
      .source = std::move(source),
      .target = std::move(target),
      .source_identity = std::move(source_id),
      .target_identity = std::move(target_id),
      .source_layout = source_digest,
      .target_layout = target_digest,
      .plan = plan.take_value(),
  };
}

Fixture system_fixture(
    const ytec::migrationcore::ShrinkSurplusAllocation allocation) {
  auto source = source_disk(ytec::diskmodel::PartitionStyle::gpt);
  source.size_bytes = 64ULL * kGiB;
  source.sector_count = source.size_bytes / source.logical_sector_size;
  source.is_system_disk = true;
  source.partitions = {
      {
          .number = 1U,
          .offset_bytes = 1ULL * kMiB,
          .size_bytes = 200ULL * kMiB,
          .style = ytec::diskmodel::PartitionStyle::gpt,
          .type = L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}",
          .identifier = L"{10000000-0000-0000-0000-000000000001}",
          .name = L"SYSTEM",
      },
      {
          .number = 2U,
          .offset_bytes = 201ULL * kMiB,
          .size_bytes = 16ULL * kMiB,
          .style = ytec::diskmodel::PartitionStyle::gpt,
          .type = L"{E3C9E316-0B5C-4DB8-817D-F92DF00215AE}",
          .identifier = L"{10000000-0000-0000-0000-000000000002}",
          .name = L"",
      },
      {
          .number = 3U,
          .offset_bytes = 217ULL * kMiB,
          .size_bytes = 32ULL * kGiB,
          .style = ytec::diskmodel::PartitionStyle::gpt,
          .type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}",
          .identifier = L"{10000000-0000-0000-0000-000000000003}",
          .name = L"Windows",
      },
      {
          .number = 4U,
          .offset_bytes = 217ULL * kMiB + 32ULL * kGiB,
          .size_bytes = 2ULL * kGiB,
          .style = ytec::diskmodel::PartitionStyle::gpt,
          .type = L"{DE94BBA4-06D1-4D40-A16A-BFD50179D6AC}",
          .identifier = L"{10000000-0000-0000-0000-000000000004}",
          .name = L"Recovery",
      },
  };
  auto target = target_disk(56ULL * kGiB);
  ytec::migrationcore::DirectClonePlanningRequest direct_request{
      .mode_choice = ytec::migrationcore::DirectCloneModeChoice::shrink,
      .partition_style_choice = ytec::migrationcore::
          DirectClonePartitionStyleChoice::preserve,
      .source_style = ytec::migrationcore::MigrationPartitionStyle::gpt,
      .source_size_bytes = source.size_bytes,
      .source_logical_sector_size = kSectorSize,
      .target_size_bytes = target.size_bytes,
      .target_logical_sector_size = kSectorSize,
      .source_is_windows_system = true,
      .windows_is_amd64 = true,
      .bitlocker_fully_decrypted = true,
      .surplus_allocation = allocation,
      .source_partitions = {
          {
              .partition = {
                  .source_table_index = 1U,
                  .role = ytec::migrationcore::MigrationPartitionRole::efi_system,
                  .file_system = ytec::migrationcore::MigrationFileSystem::fat32,
                  .source_size_bytes = 200ULL * kMiB,
                  .used_bytes = 32ULL * kMiB,
                  .cluster_size = 4096U,
                  .label = L"SYSTEM",
              },
              .selected = true,
          },
          {
              .partition = {
                  .source_table_index = 2U,
                  .role = ytec::migrationcore::MigrationPartitionRole::microsoft_reserved,
                  .file_system = ytec::migrationcore::MigrationFileSystem::none,
                  .source_size_bytes = 16ULL * kMiB,
                  .used_bytes = 0U,
                  .cluster_size = 0U,
              },
              .selected = true,
          },
          {
              .partition = {
                  .source_table_index = 3U,
                  .role = ytec::migrationcore::MigrationPartitionRole::windows,
                  .file_system = ytec::migrationcore::MigrationFileSystem::ntfs,
                  .source_size_bytes = 32ULL * kGiB,
                  .used_bytes = 8ULL * kGiB,
                  .cluster_size = 4096U,
                  .label = L"Windows",
              },
              .selected = true,
          },
          {
              .partition = {
                  .source_table_index = 4U,
                  .role = ytec::migrationcore::MigrationPartitionRole::recovery,
                  .file_system = ytec::migrationcore::MigrationFileSystem::ntfs,
                  .source_size_bytes = 2ULL * kGiB,
                  .used_bytes = 1ULL * kGiB,
                  .cluster_size = 4096U,
                  .label = L"Recovery",
              },
              .selected = true,
              .required_for_windows = true,
          },
      },
  };
  auto direct = ytec::migrationcore::plan_direct_clone(direct_request);
  check(direct.has_value(), "synthetic system direct plan must build");
  auto source_id = stable_identity(source);
  auto target_id = stable_identity(target);
  auto source_digest = layout_hash(source);
  auto target_digest = layout_hash(target);
  ytec::windowsapp::WindowsDirectShrinkPlanningRequest product_request{
      .administrator = true,
      .bitlocker_fully_decrypted = true,
      .target_is_active_rescue_media = false,
      .reviewed_source = source,
      .reviewed_target = target,
      .expected_source = source_id,
      .expected_target = target_id,
      .expected_source_layout_hash = source_digest,
      .expected_target_layout_hash = target_digest,
      .operation_id = filled<16U>(0x39U),
      .ntfs_volumes = {
          {
              .source_table_index = 3U,
              .source_offset_bytes = 217ULL * kMiB,
              .source_size_bytes = 32ULL * kGiB,
              .original_volume_guid_path = std::wstring(kSourceVolume),
          },
          {
              .source_table_index = 4U,
              .source_offset_bytes = 217ULL * kMiB + 32ULL * kGiB,
              .source_size_bytes = 2ULL * kGiB,
              .original_volume_guid_path = std::wstring(kRecoveryVolume),
          },
      },
  };
  auto plan = ytec::windowsapp::build_windows_direct_shrink_clone_plan(
      product_request, direct.value());
  check(plan.has_value(), "synthetic system product plan must build");
  return {
      .source = std::move(source),
      .target = std::move(target),
      .source_identity = std::move(source_id),
      .target_identity = std::move(target_id),
      .source_layout = source_digest,
      .target_layout = target_digest,
      .plan = plan.take_value(),
  };
}

class SequenceGuidGenerator final : public ytec::clonecore::IGuidGenerator {
 public:
  ytec::clonecore::Result<ytec::clonecore::GptGuid> next_guid() override {
    ytec::clonecore::GptGuid value{};
    value.bytes[0] = static_cast<std::byte>(++next_);
    return ytec::clonecore::Result<ytec::clonecore::GptGuid>::success(value);
  }

 private:
  std::uint8_t next_{};
};

class SparseWriter final : public ytec::clonecore::ITargetDiskWriter {
 public:
  struct Record final {
    std::uint64_t offset{};
    std::vector<std::byte> bytes;
  };

  explicit SparseWriter(const std::uint64_t size) : size_(size) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return size_;
  }
  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return kSectorSize;
  }
  [[nodiscard]] ytec::clonecore::Status write_target(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    if (offset > size_ || bytes.size() > size_ - offset) {
      return ytec::clonecore::Status::failure(injected_error(
          ytec::clonecore::ErrorCode::invalid_argument,
          L"合成writer範囲"));
    }
    records_.push_back({offset, {bytes.begin(), bytes.end()}});
    ++write_count_;
    return ytec::clonecore::success_status();
  }
  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read_back(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > size_ || length > size_ - offset) {
      return injected_failure<std::vector<std::byte>>(
          ytec::clonecore::ErrorCode::invalid_argument,
          L"合成reader範囲");
    }
    std::vector<std::byte> result(length, std::byte{0});
    const std::uint64_t end = offset + length;
    for (const auto& record : records_) {
      const std::uint64_t record_end = record.offset + record.bytes.size();
      const std::uint64_t begin = (std::max)(offset, record.offset);
      const std::uint64_t overlap_end = (std::min)(end, record_end);
      if (begin >= overlap_end) {
        continue;
      }
      std::copy(
          record.bytes.begin() + static_cast<std::ptrdiff_t>(
              begin - record.offset),
          record.bytes.begin() + static_cast<std::ptrdiff_t>(
              overlap_end - record.offset),
          result.begin() + static_cast<std::ptrdiff_t>(begin - offset));
    }
    if (corrupt_next_read_ && !result.empty()) {
      result[0] ^= std::byte{0x01};
      corrupt_next_read_ = false;
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::move(result));
  }
  [[nodiscard]] ytec::clonecore::Status flush_target() override {
    ++flush_count_;
    return ytec::clonecore::success_status();
  }
  void corrupt_next_read() noexcept { corrupt_next_read_ = true; }
  [[nodiscard]] std::size_t write_count() const noexcept {
    return write_count_;
  }
  [[nodiscard]] std::size_t flush_count() const noexcept {
    return flush_count_;
  }

 private:
  std::uint64_t size_{};
  std::vector<Record> records_;
  mutable bool corrupt_next_read_{};
  std::size_t write_count_{};
  std::size_t flush_count_{};
};

struct PlatformState final {
  Fixture fixture;
  std::vector<std::string> events;
  bool offline{};
  bool reidentifier_mismatch{};
  bool fail_wim_apply{};
  bool fail_ntfs_extension{};
  bool fail_boot_finalization{};
  bool fail_winre_finalization{};
  bool winre_request_exact{};
  std::uint32_t fail_offline_attempts{};
  SparseWriter* writer{};
};

class MockIo final
    : public ytec::windowsapp::IWindowsTsumugiShrinkRestorePlatformIo {
 public:
  explicit MockIo(std::shared_ptr<PlatformState> state)
      : state_(std::move(state)) {}

  [[nodiscard]] ytec::clonecore::Result<
      ytec::windowsapp::WindowsTsumugiShrinkTargetObservation>
  observe_original_target(
      const ytec::imageformat::Sha256Digest& connection_hash) override {
    state_->events.push_back("observe");
    auto target = state_->fixture.target;
    target.offline = state_->offline;
    return ytec::clonecore::Result<
        ytec::windowsapp::WindowsTsumugiShrinkTargetObservation>::success({
        .physical = {
            .target = std::move(target),
            .target_identity = state_->fixture.target_identity,
        },
        .restore_identity = {
            .stable_identity_hash = filled<32U>(0x41U),
            .disk_size = state_->fixture.target.size_bytes,
            .logical_sector_size = kSectorSize,
            .connection_instance_hash = connection_hash,
        },
    });
  }

  [[nodiscard]] ytec::clonecore::Status validate_work_paths_disjoint(
      const ytec::windowsapp::WindowsShrinkWorkPaths&,
      const std::uint32_t) override {
    state_->events.push_back("unexpected-work-paths");
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status set_target_offline(
      const bool offline) override {
    state_->events.push_back(offline ? "disk-offline" : "disk-online");
    if (offline && state_->fail_offline_attempts != 0U) {
      --state_->fail_offline_attempts;
      return ytec::clonecore::Status::failure(injected_error(
          ytec::clonecore::ErrorCode::io_failed,
          L"合成offline失敗"));
    }
    state_->offline = offline;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::diskmodel::PhysicalTargetHandle>
  open_offline_target() override {
    state_->events.push_back("open-writer");
    auto writer = std::make_unique<SparseWriter>(
        state_->fixture.target.size_bytes);
    state_->writer = writer.get();
    auto target = state_->fixture.target;
    target.offline = true;
    return ytec::clonecore::Result<
        ytec::diskmodel::PhysicalTargetHandle>::success({
        .observed = {
            .target = std::move(target),
            .target_identity = state_->fixture.target_identity,
        },
        .target = std::move(writer),
    });
  }

  [[nodiscard]] ytec::clonecore::Status notify_layout_changed() override {
    state_->events.push_back("layout-notify");
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::windowsapp::WindowsTsumugiShrinkVolumeBinding>
  bind_online_volume(
      const std::uint32_t number,
      const std::uint64_t offset,
      const std::uint64_t size) override {
    state_->events.push_back("bind-" + std::to_string(number));
    return ytec::clonecore::Result<
        ytec::windowsapp::WindowsTsumugiShrinkVolumeBinding>::success({
        .final_target_number = number,
        .disk_number = state_->fixture.target.disk_number,
        .target_offset = offset,
        .target_size = size,
        .volume_device_path =
            L"\\\\?\\Volume{" + std::to_wstring(number) + L"}\\",
    });
  }

  [[nodiscard]] ytec::clonecore::Status format_volume(
      const ytec::windowsapp::WindowsTsumugiShrinkVolumeBinding& volume,
      const ytec::imageformat::TsumugiManifestFileSystem,
      const std::uint64_t) override {
    state_->events.push_back(
        "format-" + std::to_string(volume.final_target_number));
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::windowsapp::WindowsTsumugiShrinkFileSystemReadbackEvidence>
  verify_volume_readback(
      const ytec::windowsapp::WindowsTsumugiShrinkVolumeBinding& volume,
      const ytec::imageformat::TsumugiManifestFileSystem,
      const std::uint64_t,
      const bool content) override {
    state_->events.push_back(
        std::string(content ? "verify-content-" : "verify-fs-") +
        std::to_string(volume.final_target_number));
    return ytec::clonecore::Result<ytec::windowsapp::
        WindowsTsumugiShrinkFileSystemReadbackEvidence>::success({
        .directory_count = content ? 4U : 1U,
        .regular_file_count = content ? 3U : 0U,
        .regular_file_bytes_read = content ? 8192U : 0U,
        .reparse_point_count = 0U,
        .file_system_metadata_verified = true,
        .namespace_fully_enumerated = content,
        .every_regular_file_read_to_eof = content,
    });
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::windowsapp::WindowsTsumugiShrinkNtfsExtensionEvidence>
  extend_ntfs_volume_to_exact_extent_and_verify(
      const ytec::windowsapp::WindowsTsumugiShrinkVolumeBinding& volume,
      const std::uint64_t previous_partition_size,
      const std::uint64_t) override {
    state_->events.push_back(
        "extend-" + std::to_string(volume.final_target_number));
    if (state_->fail_ntfs_extension) {
      return injected_failure<ytec::windowsapp::
          WindowsTsumugiShrinkNtfsExtensionEvidence>(
          ytec::clonecore::ErrorCode::io_failed,
          L"合成NTFS伸長失敗");
    }
    return ytec::clonecore::Result<ytec::windowsapp::
        WindowsTsumugiShrinkNtfsExtensionEvidence>::success({
        .previous_file_system_bytes = previous_partition_size,
        .final_file_system_bytes = volume.target_size,
        .final_partition_extent_bytes = volume.target_size,
        .exact_single_extent_reverified = true,
        .ntfs_sector_count_reverified = true,
        .flushed = true,
    });
  }

  [[nodiscard]] ytec::clonecore::Status dismount_and_offline_volume(
      const ytec::windowsapp::WindowsTsumugiShrinkVolumeBinding& volume)
      override {
    state_->events.push_back(
        "dismount-" + std::to_string(volume.final_target_number));
    state_->offline = true;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status begin_owned_staged_wim(
      const std::wstring&,
      const std::uint32_t,
      const std::uint64_t) override {
    state_->events.push_back("unexpected-stage-begin");
    return ytec::clonecore::success_status();
  }
  [[nodiscard]] ytec::clonecore::Status append_owned_staged_wim(
      const std::uint64_t,
      const std::uint64_t,
      const bool,
      const std::span<const std::byte>) override {
    state_->events.push_back("unexpected-stage-append");
    return ytec::clonecore::success_status();
  }
  [[nodiscard]] ytec::clonecore::Result<
      ytec::imageformat::Sha256Digest>
  verify_and_lock_single_image_wim(const std::uint32_t) override {
    state_->events.push_back("unexpected-stage-verify");
    return ytec::clonecore::Result<
        ytec::imageformat::Sha256Digest>::success(filled<32U>(0x51U));
  }
  [[nodiscard]] ytec::clonecore::Status apply_locked_wim(
      const ytec::windowsapp::WindowsTsumugiShrinkVolumeBinding&,
      const std::wstring&,
      const ytec::imageformat::Sha256Digest&) override {
    state_->events.push_back("unexpected-stage-apply");
    return ytec::clonecore::success_status();
  }
  [[nodiscard]] ytec::clonecore::Status discard_owned_staged_wim() override {
    state_->events.push_back("unexpected-stage-discard");
    return ytec::clonecore::success_status();
  }

 private:
  std::shared_ptr<PlatformState> state_;
};

class MockWimStore final
    : public ytec::windowsapp::IWindowsDirectShrinkOwnedWimStore {
 public:
  explicit MockWimStore(std::shared_ptr<PlatformState> state)
      : state_(std::move(state)) {}

  [[nodiscard]] ytec::clonecore::Result<
      ytec::windowsapp::WindowsDirectShrinkOwnedWimEvidence>
  capture_and_seal(
      const std::uint32_t source_table_index,
      const std::wstring&,
      const std::uint64_t) override {
    state_->events.push_back("wim-capture");
    return ytec::clonecore::Result<ytec::windowsapp::
        WindowsDirectShrinkOwnedWimEvidence>::success({
        .source_table_index = source_table_index,
        .length = 4096U,
        .hash = filled<32U>(0x61U),
        .sealed_without_write_or_delete_sharing = true,
        .flushed = true,
        .complete_read_back_hash_verified = true,
    });
  }

  [[nodiscard]] ytec::clonecore::Status apply_locked_and_reverify(
      const std::uint32_t,
      const ytec::imageformat::Sha256Digest&,
      const std::wstring&) override {
    state_->events.push_back("wim-apply");
    if (state_->fail_wim_apply) {
      return ytec::clonecore::Status::failure(injected_error(
          ytec::clonecore::ErrorCode::io_failed,
          L"合成WIM apply失敗"));
    }
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status discard_exact(
      const std::uint32_t,
      const ytec::imageformat::Sha256Digest&) override {
    state_->events.push_back("wim-discard");
    return ytec::clonecore::success_status();
  }

 private:
  std::shared_ptr<PlatformState> state_;
};

ytec::diskmodel::ReidentifiedPhysicalClone observation(
    const PlatformState& state) {
  auto target = state.fixture.target;
  target.offline = state.offline;
  auto target_identity = state.fixture.target_identity;
  if (state.reidentifier_mismatch) {
    target_identity.model += L" CHANGED";
  }
  return {
      .source = state.fixture.source,
      .target = std::move(target),
      .source_identity = state.fixture.source_identity,
      .target_identity = std::move(target_identity),
  };
}

ytec::windowsapp::WindowsDirectShrinkClonePlatformDependencies dependencies(
    const std::shared_ptr<PlatformState>& state) {
  ytec::imageformat::Sha256Digest connection{};
  connection.fill(std::byte{0x71});
  return {
      .target_io = std::make_unique<MockIo>(state),
      .guid_generator = std::make_unique<SequenceGuidGenerator>(),
      .make_wim_store =
          [state](
              const std::wstring&,
              const std::uint64_t,
              const std::uint64_t,
              const ytec::clonecore::DiskOperationCallbacks&) {
            state->events.push_back("wim-store-create");
            std::unique_ptr<
                ytec::windowsapp::IWindowsDirectShrinkOwnedWimStore> store =
                std::make_unique<MockWimStore>(state);
            return ytec::clonecore::Result<std::unique_ptr<
                ytec::windowsapp::IWindowsDirectShrinkOwnedWimStore>>::success(
                    std::move(store));
          },
      .connection_instance_hash = connection,
      .reidentify_confirmed =
          [state](
              const ytec::clonecore::StableDiskIdentity&,
              const ytec::clonecore::StableDiskIdentity&,
              const ytec::clonecore::TargetConfirmation&) {
            state->events.push_back("reidentify");
            return ytec::clonecore::Result<
                 ytec::diskmodel::ReidentifiedPhysicalClone>::success(
                     observation(*state));
           },
      .finalize_boot =
          [state](const ytec::clonecore::StableDiskIdentity&,
                  const std::uint64_t) {
            state->events.push_back("finalize-boot");
            if (state->fail_boot_finalization) {
              return injected_failure<ytec::windowsapp::
                  WindowsDirectShrinkBootFinalizationEvidence>(
                  ytec::clonecore::ErrorCode::io_failed,
                  L"合成BCDBoot最終化失敗");
            }
            return ytec::clonecore::Result<ytec::windowsapp::
                WindowsDirectShrinkBootFinalizationEvidence>::success({
                .microsoft_signed_bcdboot = true,
                .fresh_bcd_store_read_back_verified = true,
                .temporary_mounts_released = true,
                .final_target_reidentified = true,
                .partition_layout_unchanged = true,
                .nvram_unchanged = true,
            });
          },
      .finalize_winre =
          [state](const ytec::windowsapp::
                      WindowsDirectShrinkWinReFinalizationRequest& request) {
            state->events.push_back("finalize-winre");
            const auto windows = std::find_if(
                state->fixture.plan.tasks().begin(),
                state->fixture.plan.tasks().end(),
                [](const auto& task) {
                  return task.role == ytec::migrationcore::
                      MigrationPartitionRole::windows;
                });
            const auto recovery = std::find_if(
                state->fixture.plan.tasks().begin(),
                state->fixture.plan.tasks().end(),
                [](const auto& task) {
                  return task.role == ytec::migrationcore::
                      MigrationPartitionRole::recovery;
                });
            state->winre_request_exact =
                windows != state->fixture.plan.tasks().end() &&
                recovery != state->fixture.plan.tasks().end() &&
                ytec::clonecore::validate_stable_identity(
                    state->fixture.source_identity,
                    request.expected_source,
                    L"合成WinREコピー元").has_value() &&
                ytec::clonecore::validate_stable_identity(
                    state->fixture.target_identity,
                    request.expected_target,
                    L"合成WinREコピー先").has_value() &&
                request.confirmation.first_step_acknowledged &&
                request.confirmation.typed_token == L"OK" &&
                request.expected_target_disk_number ==
                    state->fixture.target.disk_number &&
                request.expected_windows_partition_number ==
                    windows->target_number &&
                request.expected_windows_partition_offset ==
                    windows->target_offset_bytes &&
                request.expected_windows_partition_size ==
                    windows->construction_size_bytes &&
                request.expected_recovery_partition_number ==
                    recovery->target_number &&
                request.expected_recovery_partition_offset ==
                    recovery->target_offset_bytes &&
                request.expected_recovery_partition_size ==
                    recovery->construction_size_bytes &&
                request.windows_volume_root ==
                    L"\\\\?\\Volume{" +
                        std::to_wstring(windows->target_number) + L"}\\" &&
                request.recovery_volume_root ==
                    L"\\\\?\\Volume{" +
                        std::to_wstring(recovery->target_number) + L"}\\";
            if (!state->winre_request_exact) {
              return injected_failure<ytec::windowsapp::
                  WindowsDirectShrinkWinReFinalizationEvidence>(
                  ytec::clonecore::ErrorCode::identity_mismatch,
                  L"合成WinRE exact request不一致");
            }
            if (state->fail_winre_finalization) {
              return injected_failure<ytec::windowsapp::
                  WindowsDirectShrinkWinReFinalizationEvidence>(
                  ytec::clonecore::ErrorCode::io_failed,
                  L"合成WinRE最終化失敗");
            }
            return ytec::clonecore::Result<ytec::windowsapp::
                WindowsDirectShrinkWinReFinalizationEvidence>::success({
                .registered_partition_number =
                    request.expected_recovery_partition_number,
                .registered_image_size_bytes = 1ULL * kMiB,
                .microsoft_signed_reagentc = true,
                .cloned_source_registration_disabled = true,
                .candidate_identity_locked = true,
                .fixed_setreimage_arguments = true,
                .fixed_enable_arguments = true,
                .target_revalidated_before_each_mutation_and_diagnostic = true,
                .read_only_reinspection_completed = true,
                .registered_location_matches_expected_target = true,
                .registered_image_present = true,
                .temporary_mounts_released = true,
            });
          },
  };
}

ytec::windowsapp::WindowsDirectShrinkClonePlatformRequest platform_request() {
  return {
      .confirmation = {
          .first_step_acknowledged = true,
          .typed_token = L"OK",
      },
  };
}

std::unique_ptr<ytec::windowsapp::IWindowsDirectShrinkClonePlatform>
make_platform(const std::shared_ptr<PlatformState>& state) {
  auto made = ytec::windowsapp::
      make_windows_direct_shrink_clone_platform_with_dependencies(
          state->fixture.plan,
          observation(*state),
          platform_request(),
          dependencies(state));
  check(made.has_value(), "production platform factory must accept GPT slice");
  return made.take_value();
}

struct ReadyToCommit final {
  std::unique_ptr<ytec::windowsapp::IWindowsDirectShrinkClonePlatform>
      platform;
  ytec::windowsapp::WindowsDirectShrinkCheckpointEvidence checkpoint;
};

ReadyToCommit execute_until_commit_ready(
    const std::shared_ptr<PlatformState>& state) {
  auto platform = make_platform(state);
  check(state->events.empty(), "factory construction must perform no I/O");
  auto plan_hash = ytec::operationcore::hash_operation_plan(
      state->fixture.plan.operation_plan());
  check(plan_hash.has_value(), "operation plan hash must build");
  auto checkpoint = platform->begin_target_owned_staging(
      state->fixture.plan, plan_hash.value());
  check(checkpoint.has_value(), "target-owned staging must begin");
  auto prepared = platform->prepare_non_archive_partitions_and_verify(
      state->fixture.plan.tasks());
  check(prepared.has_value(), "non-archive preparation must pass");
  std::uint64_t completed = prepared.value().prepared_task_count;
  std::uint64_t verified = prepared.value().verified_target_bytes;
  auto aggregate = prepared.value().write_digest;
  auto current = checkpoint.take_value();
  if (completed != 0U) {
    auto persisted = platform->persist_prepared_partitions_checkpoint(
        current, completed, verified, aggregate);
    check(persisted.has_value(), "prepared checkpoint must persist");
    current = persisted.take_value();
  }
  std::uint32_t snapshot_number{};
  for (const auto& task : state->fixture.plan.tasks()) {
    if (task.kind != ytec::windowsapp::
            WindowsDirectShrinkPartitionTaskKind::apply_ntfs_wim) {
      continue;
    }
    ++snapshot_number;
    auto archive = platform->capture_ntfs_wim_to_owned_staging(
        task,
        {
            .original_volume_guid_path = task.original_volume_guid_path,
            .snapshot_id = L"{22222222-2222-3333-4444-" +
                std::to_wstring(555555555550ULL + snapshot_number) + L"}",
            .snapshot_device_path =
                L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy" +
                std::to_wstring(6U + snapshot_number),
        });
    check(archive.has_value(), "mock WIM capture must pass");
    auto applied = platform->apply_staged_ntfs_wim_and_verify(
        task, archive.value());
    check(applied.has_value(), "mock WIM apply/readback must pass");
    auto discarded = platform->discard_exact_staged_archive(archive.value());
    check(discarded.has_value(), "exact WIM discard must pass");
    ++completed;
    verified += applied.value().verified_target_bytes;
    aggregate = applied.value().target_write_digest;
    auto progress = platform->persist_progress_checkpoint(
        current, completed, verified, aggregate);
    check(progress.has_value(), "progress checkpoint must persist");
    current = progress.take_value();
  }
  auto boot = platform->finalize_boot_from_staged_layout_and_verify(
      state->fixture.plan);
  check(boot.has_value() && boot.value().completed &&
            boot.value().boot_files_read_back_verified &&
            boot.value().recovery_configuration_verified &&
            boot.value().target_offline,
        "boot and recovery finalization evidence must pass");
  auto sealed = platform->seal_commit_ready_checkpoint(
      current,
      completed,
      verified,
      aggregate);
  check(sealed.has_value(), "commit-ready checkpoint must seal");
  return {
      .platform = std::move(platform),
      .checkpoint = sealed.take_value(),
  };
}

ytec::clonecore::Result<ytec::windowsapp::WindowsDirectShrinkBootEvidence>
attempt_system_boot_finalization(
    const std::shared_ptr<PlatformState>& state) {
  auto platform = make_platform(state);
  auto plan_hash = ytec::operationcore::hash_operation_plan(
      state->fixture.plan.operation_plan());
  check(plan_hash.has_value(), "operation plan hash must build");
  auto checkpoint = platform->begin_target_owned_staging(
      state->fixture.plan, plan_hash.value());
  check(checkpoint.has_value(), "target-owned staging must begin");
  auto prepared = platform->prepare_non_archive_partitions_and_verify(
      state->fixture.plan.tasks());
  check(prepared.has_value(), "system non-archive preparation must pass");
  std::uint32_t snapshot_number{};
  for (const auto& task : state->fixture.plan.tasks()) {
    if (task.kind != ytec::windowsapp::
            WindowsDirectShrinkPartitionTaskKind::apply_ntfs_wim) {
      continue;
    }
    ++snapshot_number;
    auto archive = platform->capture_ntfs_wim_to_owned_staging(
        task,
        {
            .original_volume_guid_path = task.original_volume_guid_path,
            .snapshot_id = L"SYNTHETIC-" +
                std::to_wstring(snapshot_number),
            .snapshot_device_path =
                L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy" +
                std::to_wstring(20U + snapshot_number),
        });
    check(archive.has_value(), "system WIM capture must pass");
    auto applied = platform->apply_staged_ntfs_wim_and_verify(
        task, archive.value());
    check(applied.has_value(), "system WIM apply must pass");
    auto discarded = platform->discard_exact_staged_archive(archive.value());
    check(discarded.has_value(), "system WIM discard must pass");
  }
  return platform->finalize_boot_from_staged_layout_and_verify(
      state->fixture.plan);
}

void test_factory_rejects_mbr_before_any_io() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(ytec::diskmodel::PartitionStyle::mbr),
  });
  auto made = ytec::windowsapp::
      make_windows_direct_shrink_clone_platform_with_dependencies(
          state->fixture.plan,
          observation(*state),
          platform_request(),
          dependencies(state));
  check(!made.has_value(), "production factory must reject MBR slice");
  check(state->events.empty(), "unsupported MBR must fail before I/O");
}

void test_factory_rejects_layout_drift_before_any_io() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(ytec::diskmodel::PartitionStyle::gpt),
  });
  auto drifted = observation(*state);
  drifted.target.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  drifted.target.disk_identifier =
      L"{99999999-9999-9999-9999-999999999999}";
  drifted.target.partitions.push_back(
      data_partition(ytec::diskmodel::PartitionStyle::gpt));
  auto made = ytec::windowsapp::
      make_windows_direct_shrink_clone_platform_with_dependencies(
          state->fixture.plan,
          drifted,
          platform_request(),
          dependencies(state));
  check(!made.has_value(), "layout drift must reject production factory");
  check(state->events.empty(), "layout drift must fail before I/O");
}

void test_success_publishes_final_gpt_last_and_leaves_offline() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(ytec::diskmodel::PartitionStyle::gpt),
  });
  auto ready = execute_until_commit_ready(state);
  auto revalidated = ready.platform->revalidate_before_final_commit(
      state->fixture.plan, ready.checkpoint);
  check(revalidated.has_value(), "commit-ready state must revalidate");
  auto committed = ready.platform->commit_final_layout_last(
      state->fixture.plan, revalidated.value());
  check(committed.has_value(), "final GPT commit must pass");
  check(committed.value().primary_layout_committed_last,
        "final evidence must report primary layout committed last");
  check(committed.value().checkpoint_retired &&
            committed.value().staging_removed,
        "final evidence must retire checkpoint and staging");
  check(state->offline, "completed target must remain offline");
  check(state->writer != nullptr && state->writer->write_count() != 0U &&
            state->writer->write_count() == state->writer->flush_count(),
        "every raw metadata write must be followed by a flush");
  check(std::find(state->events.begin(), state->events.end(),
                  "wim-capture") != state->events.end() &&
            std::find(state->events.begin(), state->events.end(),
                      "wim-apply") != state->events.end() &&
            std::find(state->events.begin(), state->events.end(),
                      "wim-discard") != state->events.end(),
        "owned WIM lifecycle must be complete");
  check(std::none_of(
            state->events.begin(),
            state->events.end(),
            [](const std::string& event) {
              return event.starts_with("unexpected-");
            }),
        "direct platform must not use restore-stream staging methods");
}

void test_readback_tamper_aborts_and_withholds_final_layout() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(ytec::diskmodel::PartitionStyle::gpt),
  });
  auto ready = execute_until_commit_ready(state);
  check(state->writer != nullptr, "writer must remain owned by platform");
  state->writer->corrupt_next_read();
  auto revalidated = ready.platform->revalidate_before_final_commit(
      state->fixture.plan, ready.checkpoint);
  check(!revalidated.has_value(), "raw GPT readback tamper must fail closed");
  check(state->offline, "tamper abort must keep target offline");
  auto committed = ready.platform->commit_final_layout_last(
      state->fixture.plan, ready.checkpoint);
  check(!committed.has_value(), "aborted platform must never final-commit");
}

void test_identity_drift_at_commit_aborts_before_final_write() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(ytec::diskmodel::PartitionStyle::gpt),
  });
  auto ready = execute_until_commit_ready(state);
  const auto writes_before = state->writer->write_count();
  state->reidentifier_mismatch = true;
  auto revalidated = ready.platform->revalidate_before_final_commit(
      state->fixture.plan, ready.checkpoint);
  check(!revalidated.has_value(), "identity drift must stop final commit");
  check(state->offline, "identity drift abort must keep target offline");
  check(state->writer->write_count() > writes_before,
        "abort must visibly invalidate incomplete target metadata");
}

void test_abort_never_invalidates_until_offline_is_proven() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(ytec::diskmodel::PartitionStyle::gpt),
  });
  auto ready = execute_until_commit_ready(state);
  const auto writes_before = state->writer->write_count();
  state->fail_offline_attempts = 1U;
  ready.platform->abort_keep_offline_incomplete();
  check(state->writer->write_count() == writes_before,
        "failed offline proof must forbid raw metadata invalidation");
  ready.platform->abort_keep_offline_incomplete();
  check(state->writer->write_count() > writes_before && state->offline,
        "idempotent abort retry must withhold metadata only after offline proof");
}

void test_failed_apply_still_allows_exact_archive_discard() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(ytec::diskmodel::PartitionStyle::gpt),
  });
  auto platform = make_platform(state);
  auto plan_hash = ytec::operationcore::hash_operation_plan(
      state->fixture.plan.operation_plan());
  check(plan_hash.has_value(), "operation plan hash must build");
  auto checkpoint = platform->begin_target_owned_staging(
      state->fixture.plan, plan_hash.value());
  check(checkpoint.has_value(), "target-owned staging must begin");
  const auto task = state->fixture.plan.tasks().front();
  auto archive = platform->capture_ntfs_wim_to_owned_staging(
      task,
      {
          .original_volume_guid_path = std::wstring(kSourceVolume),
          .snapshot_id = L"{22222222-2222-2222-2222-222222222222}",
          .snapshot_device_path =
              L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy7",
      });
  check(archive.has_value(), "mock WIM capture must pass");
  state->fail_wim_apply = true;
  auto applied = platform->apply_staged_ntfs_wim_and_verify(
      task, archive.value());
  check(!applied.has_value(), "injected WIM apply failure must surface");
  auto discarded = platform->discard_exact_staged_archive(archive.value());
  check(discarded.has_value(),
        "failed apply must still permit exact owned-WIM cleanup");
  check(state->offline &&
            std::find(state->events.begin(), state->events.end(),
                      "wim-discard") != state->events.end(),
        "failed apply cleanup must discard exact WIM and leave target offline");
}

void test_gpt_system_leave_unallocated_finalizes_boot_and_winre() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = system_fixture(
          ytec::migrationcore::ShrinkSurplusAllocation::leave_unallocated),
  });
  auto ready = execute_until_commit_ready(state);
  auto revalidated = ready.platform->revalidate_before_final_commit(
      state->fixture.plan, ready.checkpoint);
  check(revalidated.has_value(), "system commit-ready state must revalidate");
  auto committed = ready.platform->commit_final_layout_last(
      state->fixture.plan, revalidated.value());
  check(committed.has_value(), "GPT system leave-unallocated must commit");
  check(
      committed.value().hidden_final_layout_published_and_read_back &&
          committed.value().extended_ntfs_partition_count == 0U &&
          committed.value().every_required_ntfs_extension_verified &&
          committed.value().primary_layout_committed_last && state->offline,
      "system leave path must publish verified final GPT last without extension");
  check(
      std::find(state->events.begin(), state->events.end(), "format-1") !=
              state->events.end() &&
          std::find(state->events.begin(), state->events.end(),
                    "finalize-boot") != state->events.end() &&
          std::find(state->events.begin(), state->events.end(),
                    "finalize-winre") != state->events.end() &&
          state->winre_request_exact &&
          std::none_of(
              state->events.begin(), state->events.end(), [](const auto& event) {
                return event.starts_with("extend-");
              }),
      "system leave path must prepare ESP and prove BCDBoot/WinRE without growth");
}

void test_gpt_system_automatic_extends_every_planned_ntfs_before_visibility() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = system_fixture(ytec::migrationcore::
          ShrinkSurplusAllocation::automatic_proportional),
  });
  check(
      state->fixture.plan.ntfs_extension_task_count() != 0U &&
          state->fixture.plan.staging().final_growth_owner_target_number
              .has_value(),
      "automatic system fixture must own staging in a planned NTFS growth extent");
  auto ready = execute_until_commit_ready(state);
  auto revalidated = ready.platform->revalidate_before_final_commit(
      state->fixture.plan, ready.checkpoint);
  check(revalidated.has_value(), "automatic commit-ready state must revalidate");
  auto committed = ready.platform->commit_final_layout_last(
      state->fixture.plan, revalidated.value());
  check(committed.has_value(), "automatic GPT system path must commit");
  const auto extension_events = static_cast<std::uint64_t>(std::count_if(
      state->events.begin(), state->events.end(), [](const auto& event) {
        return event.starts_with("extend-");
      }));
  check(
      committed.value().hidden_final_layout_published_and_read_back &&
          committed.value().extended_ntfs_partition_count ==
              state->fixture.plan.ntfs_extension_task_count() &&
          extension_events == state->fixture.plan.ntfs_extension_task_count() &&
          committed.value().every_required_ntfs_extension_verified &&
          committed.value().primary_layout_committed_last && state->offline,
      "automatic path must verify each NTFS growth while hidden, then publish visible GPT last");
}

void test_system_boot_or_winre_failure_aborts_before_commit_ready() {
  {
    auto state = std::make_shared<PlatformState>(PlatformState{
        .fixture = system_fixture(
            ytec::migrationcore::ShrinkSurplusAllocation::leave_unallocated),
        .fail_boot_finalization = true,
    });
    const auto boot = attempt_system_boot_finalization(state);
    check(
        !boot.has_value() && state->offline &&
            std::find(state->events.begin(), state->events.end(),
                      "finalize-boot") != state->events.end() &&
            std::find(state->events.begin(), state->events.end(),
                      "finalize-winre") == state->events.end(),
        "BCDBoot failure must abort offline before WinRE and final publication");
  }
  {
    auto state = std::make_shared<PlatformState>(PlatformState{
        .fixture = system_fixture(
            ytec::migrationcore::ShrinkSurplusAllocation::leave_unallocated),
        .fail_winre_finalization = true,
    });
    const auto boot = attempt_system_boot_finalization(state);
    check(
        !boot.has_value() && state->offline &&
            std::find(state->events.begin(), state->events.end(),
                      "finalize-boot") != state->events.end() &&
            std::find(state->events.begin(), state->events.end(),
                      "finalize-winre") != state->events.end(),
        "WinRE registration failure must abort offline before commit-ready");
  }
}

void test_automatic_extension_failure_invalidates_and_keeps_offline() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = system_fixture(ytec::migrationcore::
          ShrinkSurplusAllocation::automatic_proportional),
  });
  auto ready = execute_until_commit_ready(state);
  auto revalidated = ready.platform->revalidate_before_final_commit(
      state->fixture.plan, ready.checkpoint);
  check(revalidated.has_value(), "automatic commit-ready state must revalidate");
  state->fail_ntfs_extension = true;
  auto committed = ready.platform->commit_final_layout_last(
      state->fixture.plan, revalidated.value());
  check(
      !committed.has_value() && state->offline &&
          std::any_of(
              state->events.begin(), state->events.end(), [](const auto& event) {
                return event.starts_with("extend-");
              }),
      "extension failure must invalidate the incomplete target and keep it offline");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"factory_rejects_mbr_before_any_io",
       test_factory_rejects_mbr_before_any_io},
      {"factory_rejects_layout_drift_before_any_io",
       test_factory_rejects_layout_drift_before_any_io},
      {"success_publishes_final_gpt_last_and_leaves_offline",
       test_success_publishes_final_gpt_last_and_leaves_offline},
      {"readback_tamper_aborts_and_withholds_final_layout",
       test_readback_tamper_aborts_and_withholds_final_layout},
      {"identity_drift_at_commit_aborts_before_final_write",
       test_identity_drift_at_commit_aborts_before_final_write},
      {"abort_never_invalidates_until_offline_is_proven",
       test_abort_never_invalidates_until_offline_is_proven},
      {"failed_apply_still_allows_exact_archive_discard",
       test_failed_apply_still_allows_exact_archive_discard},
      {"gpt_system_leave_unallocated_finalizes_boot_and_winre",
       test_gpt_system_leave_unallocated_finalizes_boot_and_winre},
      {"gpt_system_automatic_extends_every_planned_ntfs_before_visibility",
       test_gpt_system_automatic_extends_every_planned_ntfs_before_visibility},
      {"system_boot_or_winre_failure_aborts_before_commit_ready",
       test_system_boot_or_winre_failure_aborts_before_commit_ready},
      {"automatic_extension_failure_invalidates_and_keeps_offline",
       test_automatic_extension_failure_invalidates_and_keeps_offline},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}
