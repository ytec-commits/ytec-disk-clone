#include "ytec/imageformat/backup_manifest.h"
#include "ytec/imageformat/dcimg.h"
#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/windowsapp/restore_preflight.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kSourceSize = 4ULL * kMiB;

struct TestFailure final {
  std::string message;
};

void check(bool condition, std::string message) {
  if (!condition) {
    throw TestFailure{std::move(message)};
  }
}

ytec::clonecore::Error read_error() {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_HANDLE_EOF,
      .operation = L"mock dcimg reader",
      .message = L"mock read outside image",
  };
}

std::vector<std::byte> build_manifest(
    std::uint64_t declared_source_size = kSourceSize) {
  const auto result =
      ytec::imageformat::build_backup_manifest_v1(
          ytec::imageformat::BackupImageManifest{
              .source =
                  ytec::clonecore::StableDiskIdentity{
                      .disk_number = 1,
                      .model = L"SYNTHETIC SOURCE",
                      .size_bytes = declared_source_size,
                      .logical_sector_size = 512,
                      .serial_suffix = "SOURCE01",
                      .device_instance_id =
                          L"VIRTUAL\\SOURCE\\1",
                      .is_system_disk = true,
                  },
              .physical_sector_size = 4096,
              .partition_style =
                  ytec::imageformat::BackupPartitionStyle::mbr,
              .boot_mode =
                  ytec::imageformat::BackupBootMode::legacy_bios,
              .windows_major = 10,
              .windows_minor = 0,
              .windows_build = 19045,
              .windows_architecture = "AMD64",
              .bitlocker_fully_decrypted = true,
              .compression =
                  ytec::imageformat::DcimgCompression::none,
              .compression_version = 0,
              .chunk_size =
                  ytec::imageformat::kDcimgChunkSize16MiB,
              .created_utc = "2026-07-31T12:00:00Z",
              .app_version = "0.1.0-dev",
              .partitions = {
                  ytec::imageformat::BackupManifestPartition{
                      .table_index = 0,
                      .offset_bytes = kMiB,
                      .length_bytes =
                          declared_source_size - kMiB,
                      .role =
                          ytec::imageformat::BackupPartitionRole::
                              windows_ntfs,
                      .file_system =
                          ytec::imageformat::BackupFileSystem::ntfs,
                      .cluster_size = 4096,
                      .name = L"Windows",
                  },
              },
          });
  check(result.has_value(), "Synthetic manifest should build");
  return result.value();
}

std::vector<std::byte> build_partition_snapshot() {
  ytec::imageformat::PartitionSnapshot snapshot;
  snapshot.style = ytec::imageformat::PartitionTableStyle::mbr;
  snapshot.source_disk_size = kSourceSize;
  snapshot.logical_sector_size = 512;
  ytec::imageformat::PartitionTableRegion mbr;
  mbr.disk_offset = 0;
  mbr.data.assign(512, std::byte{0});
  mbr.data[510] = std::byte{0x55};
  mbr.data[511] = std::byte{0xAA};
  snapshot.regions.push_back(std::move(mbr));
  const auto result =
      ytec::imageformat::build_partition_snapshot_v1(snapshot);
  check(result.has_value(), "Synthetic partition snapshot should build");
  return result.value();
}

std::vector<std::byte> build_image(
    std::uint64_t declared_source_size = kSourceSize) {
  ytec::imageformat::DcimgBuildRequest request;
  request.source_disk_size = kSourceSize;
  request.logical_sector_size = 512;
  request.physical_sector_size = 4096;
  request.manifest = build_manifest(declared_source_size);
  request.partition_table_snapshot = build_partition_snapshot();
  ytec::imageformat::DcimgBuildChunk data;
  data.logical_offset = kMiB;
  data.logical_length = 4096;
  data.data.assign(4096, std::byte{0xA5});
  request.chunks.push_back(std::move(data));
  const auto result =
      ytec::imageformat::build_uncompressed_dcimg_v1(request);
  check(result.has_value(), "Synthetic dcimg should build");
  return result.value();
}

ytec::imageformat::Sha256ReadCallback memory_reader(
    const std::vector<std::byte>& image) {
  return [&image](
             const std::uint64_t offset,
             const std::size_t length) {
    if (offset > image.size() || length > image.size() - offset) {
      return ytec::clonecore::Result<
          std::vector<std::byte>>::failure(read_error());
    }
    const auto begin =
        image.begin() + static_cast<std::ptrdiff_t>(offset);
    return ytec::clonecore::Result<
        std::vector<std::byte>>::success(
        std::vector<std::byte>(
            begin,
            begin + static_cast<std::ptrdiff_t>(length)));
  };
}

void test_complete_reader_preflight_is_non_executable() {
  const auto image = build_image();
  const auto result =
      ytec::windowsapp::inspect_restore_image_reader(
          L"C:\\Mock\\valid.dcimg",
          image.size(),
          memory_reader(image));
  check(result.has_value(), "Valid image should pass complete preflight");
  check(
      result.value().complete_container_verified &&
          result.value().metadata_verified &&
          result.value().restore_layout_verified,
      "Every read-only verification gate should be reported");
  check(
      !result.value().restore_execution_enabled,
      "Read-only preflight must never authorize restoration");
  check(
      result.value().manifest.windows_build == 19045 &&
          result.value().manifest.partitions.size() == 1,
      "Verified source details should be returned");
}

void test_corrupt_image_and_cross_metadata_mismatch_fail() {
  auto corrupt = build_image();
  corrupt[corrupt.size() / 2U] ^= std::byte{0x01};
  check(
      !ytec::windowsapp::inspect_restore_image_reader(
           L"C:\\Mock\\corrupt.dcimg",
           corrupt.size(),
           memory_reader(corrupt))
           .has_value(),
      "A corrupt image must fail complete verification");

  const auto mismatch = build_image(8ULL * kMiB);
  const auto mismatch_result =
      ytec::windowsapp::inspect_restore_image_reader(
          L"C:\\Mock\\mismatch.dcimg",
          mismatch.size(),
          memory_reader(mismatch));
  check(
      !mismatch_result.has_value(),
      "Container and manifest source-size mismatch must fail");
  check(
      mismatch_result.error().operation ==
          L"dcimgメタデータ整合",
      "Cross-metadata mismatch should identify the failed gate");
}

void test_cancellation_stops_before_read() {
  const auto image = build_image();
  std::size_t reads{};
  const ytec::imageformat::Sha256ReadCallback reader =
      [&image, &reads](
          const std::uint64_t offset,
          const std::size_t length) {
        ++reads;
        return memory_reader(image)(offset, length);
      };
  const auto result =
      ytec::windowsapp::inspect_restore_image_reader(
          L"C:\\Mock\\cancelled.dcimg",
          image.size(),
          reader,
          ytec::windowsapp::RestoreImagePreflightOptions{
              .cancellation_requested = []() { return true; },
          });
  check(!result.has_value(), "Cancellation must stop verification");
  check(reads == 0, "Cancellation must be checked before image reads");
  check(
      result.error().native_code == ERROR_CANCELLED &&
          result.error().operation ==
              L"dcimg読取り専用検証キャンセル",
      "Cancellation should return the dedicated safe-stop error");
}

void test_restore_target_selection_is_read_only_and_fail_closed() {
  const auto image = build_image();
  const auto preflight =
      ytec::windowsapp::inspect_restore_image_reader(
          L"C:\\Mock\\valid.dcimg",
          image.size(),
          memory_reader(image));
  check(preflight.has_value(), "Image preflight should pass");

  ytec::diskmodel::DiskInfo target;
  target.disk_number = 2;
  target.model = L"SYNTHETIC TARGET";
  target.device_instance_id = L"VIRTUAL\\TARGET\\2";
  target.size_bytes = 8ULL * kMiB;
  target.logical_sector_size = 512;
  target.physical_sector_size = 4096;
  target.serial_suffix = "TARGET02";
  target.partition_style =
      ytec::diskmodel::PartitionStyle::raw;
  target.read_only = false;
  target.removable = false;

  ytec::diskmodel::InventoryReport inventory;
  inventory.disks.push_back(target);
  const auto ready =
      ytec::windowsapp::evaluate_restore_target_selection(
          &preflight.value(), &inventory, 0, false);
  check(
      ready.ready_for_confirmation &&
          !ready.restore_execution_enabled &&
          ready.target_identity.has_value(),
      "A compatible candidate should be ready only for later confirmation");

  inventory.disks[0].is_system_disk = true;
  check(
      ytec::windowsapp::evaluate_restore_target_selection(
          &preflight.value(), &inventory, 0, false)
              .issue ==
          ytec::windowsapp::RestoreTargetSelectionIssue::
              target_is_system,
      "Running Windows disk must be rejected");

  inventory.disks[0] = target;
  inventory.disks[0].size_bytes = 2ULL * kMiB;
  check(
      ytec::windowsapp::evaluate_restore_target_selection(
          &preflight.value(), &inventory, 0, false)
              .issue ==
          ytec::windowsapp::RestoreTargetSelectionIssue::
              target_too_small,
      "A target smaller than the image source must be rejected");

  inventory.disks[0] = target;
  inventory.disks[0].logical_sector_size = 4096;
  check(
      ytec::windowsapp::evaluate_restore_target_selection(
          &preflight.value(), &inventory, 0, false)
              .issue ==
          ytec::windowsapp::RestoreTargetSelectionIssue::
              logical_sector_mismatch,
      "Logical sector mismatch must be rejected");

  inventory.disks[0] = target;
  inventory.disks[0].read_only.reset();
  check(
      ytec::windowsapp::evaluate_restore_target_selection(
          &preflight.value(), &inventory, 0, false)
              .issue ==
          ytec::windowsapp::RestoreTargetSelectionIssue::
              target_state_unknown,
      "Unknown target state must be rejected");

  inventory.disks[0] = target;
  inventory.disks[0].removable = true;
  check(
      ytec::windowsapp::evaluate_restore_target_selection(
          &preflight.value(), &inventory, 0, false)
              .issue ==
          ytec::windowsapp::RestoreTargetSelectionIssue::
              target_is_removable,
      "Removable media must be rejected");

  inventory.disks[0] = target;
  inventory.disks[0].model = L"SYNTHETIC SOURCE";
  inventory.disks[0].serial_suffix = "SOURCE01";
  check(
      ytec::windowsapp::evaluate_restore_target_selection(
          &preflight.value(), &inventory, 0, false)
              .issue ==
          ytec::windowsapp::RestoreTargetSelectionIssue::
              target_is_original_source,
      "The original source disk must be rejected");

  inventory.disks[0] = target;
  inventory.issues.push_back(ytec::diskmodel::InventoryIssue{
      .device = L"mock",
      .error = read_error(),
  });
  check(
      ytec::windowsapp::evaluate_restore_target_selection(
          &preflight.value(), &inventory, 0, false)
              .issue ==
          ytec::windowsapp::RestoreTargetSelectionIssue::
              inventory_has_issues,
      "Unresolved inventory diagnostics must fail closed");
}

void test_shrink_restore_accepts_a_sufficient_smaller_raw_target() {
  constexpr std::uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;
  const ytec::clonecore::StableDiskIdentity source{
      .disk_number = 1,
      .model = L"SHRINK SOURCE",
      .size_bytes = 100ULL * kGiB,
      .logical_sector_size = 512,
      .serial_suffix = "SHRINK01",
      .device_instance_id = L"VIRTUAL\\SHRINK\\1",
  };
  ytec::imageformat::RestoreImageInspectionReport image{
      .header = ytec::imageformat::DcimgHeader{
          .source_disk_size = source.size_bytes,
          .logical_sector_size = 512,
          .physical_sector_size = 4096,
      },
      .manifest = ytec::imageformat::BackupImageManifest{
          .source = source,
          .physical_sector_size = 4096,
          .partition_style =
              ytec::imageformat::BackupPartitionStyle::gpt,
          .boot_mode = ytec::imageformat::BackupBootMode::none,
          .bitlocker_fully_decrypted = true,
          .created_utc = "2026-08-03T00:00:00Z",
          .app_version = "0.2.0-dev",
      },
      .complete_container_verified = true,
      .metadata_verified = true,
      .restore_layout_verified = true,
      .shrink_manifest = ytec::imageformat::ShrinkImageManifest{
          .source = source,
          .physical_sector_size = 4096,
          .partition_style =
              ytec::migrationcore::MigrationPartitionStyle::gpt,
          .bitlocker_fully_decrypted = true,
          .created_utc = "2026-08-03T00:00:00Z",
          .app_version = "0.2.0-dev",
          .partitions = {
              ytec::imageformat::ShrinkImagePartition{
                  .source_table_index = 0,
                  .role = ytec::migrationcore::MigrationPartitionRole::data,
                  .file_system =
                      ytec::migrationcore::MigrationFileSystem::ntfs,
                  .source_size_bytes = 99ULL * kGiB,
                  .used_bytes = 20ULL * kGiB,
                  .cluster_size = 4096,
                  .label = L"Data",
                  .payload_file_name = "volume-000.wim",
                  .payload_length_bytes = 10ULL * kGiB,
              },
          },
      },
  };
  image.manifest.partitions.push_back(
      ytec::imageformat::BackupManifestPartition{
          .table_index = 0,
          .offset_bytes = kMiB,
          .length_bytes = 99ULL * kGiB,
          .role = ytec::imageformat::BackupPartitionRole::ntfs_data,
          .file_system = ytec::imageformat::BackupFileSystem::ntfs,
          .cluster_size = 4096,
          .name = L"Data",
      });

  ytec::diskmodel::DiskInfo target;
  target.disk_number = 2;
  target.model = L"SMALL TARGET";
  target.device_instance_id = L"VIRTUAL\\TARGET\\2";
  target.size_bytes = 40ULL * kGiB;
  target.logical_sector_size = 512;
  target.physical_sector_size = 4096;
  target.serial_suffix = "TARGET02";
  target.partition_style = ytec::diskmodel::PartitionStyle::raw;
  target.read_only = false;
  target.removable = false;
  target.offline = false;
  ytec::diskmodel::InventoryReport inventory;
  inventory.disks.push_back(target);

  const auto ready = ytec::windowsapp::evaluate_restore_target_selection(
      &image, &inventory, 0, false);
  check(
      ready.ready_for_confirmation,
      "A sufficient smaller RAW target should pass shrink planning");

  inventory.disks[0].partition_style =
      ytec::diskmodel::PartitionStyle::gpt;
  check(
      ytec::windowsapp::evaluate_restore_target_selection(
          &image, &inventory, 0, false)
              .issue ==
          ytec::windowsapp::RestoreTargetSelectionIssue::target_style_unknown,
      "Shrink restore should require an empty RAW target");

  inventory.disks[0] = target;
  inventory.disks[0].size_bytes = 10ULL * kGiB;
  check(
      ytec::windowsapp::evaluate_restore_target_selection(
          &image, &inventory, 0, false)
              .issue ==
          ytec::windowsapp::RestoreTargetSelectionIssue::target_too_small,
      "Shrink restore should reject less than used data plus reserve");
}

class TemporaryImageFile final {
 public:
  explicit TemporaryImageFile(
      const std::span<const std::byte> bytes) {
    std::vector<wchar_t> temp_path(MAX_PATH + 1U, L'\0');
    const DWORD length = GetTempPathW(
        static_cast<DWORD>(temp_path.size()), temp_path.data());
    check(
        length != 0 &&
            static_cast<std::size_t>(length) < temp_path.size(),
        "Temporary path should be available");
    path_.assign(temp_path.data(), length);
    path_ += L"Tsumugi-restore-";
    path_ += std::to_wstring(GetCurrentProcessId());
    path_ += L"-";
    path_ += std::to_wstring(GetTickCount64());
    path_ += L".dcimg";

    HANDLE file = CreateFileW(
        path_.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    check(file != INVALID_HANDLE_VALUE, "Temporary image should be created");
    DWORD written{};
    const BOOL succeeded = WriteFile(
        file,
        bytes.data(),
        static_cast<DWORD>(bytes.size()),
        &written,
        nullptr);
    CloseHandle(file);
    check(
        succeeded != FALSE &&
            static_cast<std::size_t>(written) == bytes.size(),
        "Temporary image should be written completely");
  }

  ~TemporaryImageFile() {
    if (!path_.empty()) {
      DeleteFileW(path_.c_str());
    }
  }

  TemporaryImageFile(const TemporaryImageFile&) = delete;
  TemporaryImageFile& operator=(const TemporaryImageFile&) = delete;

  const std::wstring& path() const noexcept { return path_; }

 private:
  std::wstring path_;
};

void test_windows_file_backend_is_read_only_and_complete() {
  const auto image = build_image();
  const TemporaryImageFile file(image);
  const auto result =
      ytec::windowsapp::inspect_restore_image_file(file.path());
  check(result.has_value(), "A regular local dcimg file should verify");
  check(
      result.value().canonical_path == file.path(),
      "Product backend should retain the canonical local path");

  std::wstring wrong_extension = file.path();
  wrong_extension += L".bin";
  const auto rejected =
      ytec::windowsapp::inspect_restore_image_file(wrong_extension);
  check(
      !rejected.has_value() &&
          rejected.error().operation == L"dcimg入力パス",
      "A non-dcimg path must stop before file open");
}

}  // namespace

int main() {
  const std::vector<
      std::pair<std::string, std::function<void()>>> tests{
      {"complete_reader_preflight_is_non_executable",
       test_complete_reader_preflight_is_non_executable},
      {"corrupt_image_and_cross_metadata_mismatch_fail",
       test_corrupt_image_and_cross_metadata_mismatch_fail},
      {"cancellation_stops_before_read",
       test_cancellation_stops_before_read},
      {"restore_target_selection_is_read_only_and_fail_closed",
       test_restore_target_selection_is_read_only_and_fail_closed},
      {"shrink_restore_accepts_a_sufficient_smaller_raw_target",
       test_shrink_restore_accepts_a_sufficient_smaller_raw_target},
      {"windows_file_backend_is_read_only_and_complete",
       test_windows_file_backend_is_read_only_and_complete},
  };

  int failures{};
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL " << name << ": "
                << failure.message << '\n';
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "FAIL " << name
                << ": unexpected exception: "
                << exception.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}
