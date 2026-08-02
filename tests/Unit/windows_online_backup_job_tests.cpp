#include "ytec/windowsapp/online_backup_job.h"
#include "ytec/imageformat/backup_manifest.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kSectorSize = 512;
constexpr std::uint64_t kDiskSize = 16ULL * 1024ULL * 1024ULL;

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

template <typename T>
void write_little(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

std::vector<std::byte> make_mbr_disk() {
  std::vector<std::byte> bytes(
      static_cast<std::size_t>(kDiskSize), std::byte{0});
  write_little<std::uint32_t>(bytes, 440, 0x10203040U);
  const std::size_t entry = 446;
  bytes[entry] = std::byte{0x80};
  bytes[entry + 4] = std::byte{0x07};
  constexpr std::uint32_t kFirstLba = 2048;
  constexpr std::uint32_t kSectors = 16384;
  write_little(bytes, entry + 8, kFirstLba);
  write_little(bytes, entry + 12, kSectors);
  bytes[510] = std::byte{0x55};
  bytes[511] = std::byte{0xAA};

  const std::size_t boot =
      static_cast<std::size_t>(kFirstLba) * kSectorSize;
  constexpr char kNtfsSignature[] = "NTFS    ";
  std::memcpy(bytes.data() + boot + 3, kNtfsSignature, 8);
  write_little<std::uint16_t>(bytes, boot + 11, kSectorSize);
  bytes[boot + 13] = std::byte{8};
  write_little<std::uint64_t>(bytes, boot + 40, kSectors);
  bytes[boot + 510] = std::byte{0x55};
  bytes[boot + 511] = std::byte{0xAA};
  return bytes;
}

class MemoryReader final : public ytec::clonecore::ISourceDiskReader {
 public:
  explicit MemoryReader(std::vector<std::byte> bytes)
      : bytes_(std::move(bytes)) {}

  std::uint64_t size_bytes() const noexcept override {
    return bytes_.size();
  }

  std::uint32_t logical_sector_size() const noexcept override {
    return kSectorSize;
  }

  ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > bytes_.size() || length > bytes_.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          ytec::clonecore::Error{
              .code = ytec::clonecore::ErrorCode::io_failed,
              .native_code = ERROR_READ_FAULT,
              .operation = L"合成オンラインジョブ読取り",
              .message = L"範囲外です",
          });
    }
    const auto first =
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            first, first + static_cast<std::ptrdiff_t>(length)));
  }

 private:
  std::vector<std::byte> bytes_;
};

ytec::diskmodel::DiskInfo source_disk(
    const std::uint32_t physical_sector_size = 4096U) {
  return ytec::diskmodel::DiskInfo{
      .disk_number = 0,
      .device_path = L"\\\\.\\PhysicalDrive0",
      .device_instance_id = L"VIRTUAL\\ONLINE_JOB",
      .model = L"ONLINE JOB FIXTURE",
      .size_bytes = kDiskSize,
      .sector_count = kDiskSize / kSectorSize,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = physical_sector_size,
      .bus_type = L"Virtual",
      .serial_suffix = "JOB00001",
      .partition_style = ytec::diskmodel::PartitionStyle::mbr,
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = true,
  };
}

ytec::windowsapp::OnlineBackupJobRequest request(
    const std::uint32_t physical_sector_size = 4096U) {
  return ytec::windowsapp::OnlineBackupJobRequest{
      .selected_source = source_disk(physical_sector_size),
      .final_path = L"D:\\backup\\system.dcimg",
      .administrator = true,
      .windows_major = 10,
      .windows_minor = 0,
      .windows_build = 19045,
      .windows_architecture = "AMD64",
      .created_utc = "2026-07-31T12:00:00Z",
      .app_version = "0.1.0",
  };
}

ytec::clonecore::Result<
    ytec::diskmodel::ReadOnlyPhysicalDiskHandle>
open_fixture(
    const ytec::clonecore::StableDiskIdentity& expected,
    const std::uint32_t physical_sector_size = 4096U) {
  return ytec::clonecore::Result<
      ytec::diskmodel::ReadOnlyPhysicalDiskHandle>::success(
      ytec::diskmodel::ReadOnlyPhysicalDiskHandle{
          .observed =
              ytec::diskmodel::ReidentifiedReadOnlyDisk{
                  .observed = source_disk(physical_sector_size),
                  .identity = expected,
              },
          .reader = std::make_unique<MemoryReader>(make_mbr_disk()),
      });
}

ytec::windowsapp::OnlineBackupJobDependencies dependencies(
    bool& opened,
    bool& bindings_queried,
    bool& executed,
    const bool expect_callbacks = false,
    const std::uint32_t physical_sector_size = 4096U) {
  return ytec::windowsapp::OnlineBackupJobDependencies{
      .open_read_only_disk =
          [&](const ytec::clonecore::StableDiskIdentity& expected) {
            opened = true;
            return open_fixture(expected, physical_sector_size);
          },
      .query_gpt_bindings =
          [&](const ytec::diskmodel::DiskInfo&,
              const ytec::clonecore::GptDisk&) {
            return ytec::clonecore::Result<std::vector<
                ytec::clonecore::VolumeBitmapBinding>>::failure(
                ytec::clonecore::Error{
                    .code =
                        ytec::clonecore::ErrorCode::unsupported_layout,
                    .native_code = ERROR_NOT_SUPPORTED,
                    .operation = L"予期しないGPT",
                    .message = L"MBRテストでGPT照会が呼ばれました",
                });
          },
      .query_mbr_bindings =
          [&](const ytec::diskmodel::DiskInfo&,
              const ytec::clonecore::MbrDisk& layout) {
            bindings_queried = true;
            check(
                layout.partitions.size() == 1,
                "Current MBR should reach binding query");
            return ytec::clonecore::Result<std::vector<
                ytec::clonecore::VolumeBitmapBinding>>::success({
                ytec::clonecore::VolumeBitmapBinding{
                    .partition_entry_index = 0,
                    .volume_device_path =
                        L"\\\\?\\Volume{11111111-1111-1111-1111-111111111111}\\",
                },
            });
          },
      .execute_backup =
          [&](const ytec::vssrequester::WindowsOnlineImageBackupRequest&
                  execution) {
            executed = true;
            check(
                execution.plan.snapshot_partition_count == 1 &&
                    execution.plan.raw_partition_count == 0 &&
                    execution.plan.workflow.administrator &&
                    execution.plan.image_copy.physical_sector_size ==
                        physical_sector_size &&
                    execution.plan.image_copy.compression ==
                        ytec::imageformat::DcimgCompression::zstandard &&
                    execution.plan.image_copy
                            .verification_block_bytes ==
                        4U * 1024U * 1024U,
                "Executor should receive a fully prepared MBR/VSS plan");
            const auto manifest =
                ytec::imageformat::inspect_backup_manifest_v1(
                    execution.plan.image_copy.manifest);
            check(
                manifest.has_value() &&
                    manifest.value().physical_sector_size ==
                        physical_sector_size &&
                    manifest.value().compression ==
                        ytec::imageformat::DcimgCompression::zstandard &&
                    manifest.value().compression_version ==
                        ytec::imageformat::kDcimgZstandardProfileVersion,
                "Product backup metadata must declare Zstandard profile 1");
            if (expect_callbacks) {
              check(
                  static_cast<bool>(
                      execution.plan.image_copy.callbacks.progress) &&
                      static_cast<bool>(
                          execution.plan.image_copy.callbacks
                              .cancellation_requested),
                  "Product progress and cancellation callbacks should reach the image writer");
            }
            check(
                execution.staging.final_path ==
                        L"D:\\backup\\system.dcimg" &&
                    !execution.staging.expected_clone_target_disk
                         .has_value(),
                "Executor should receive the guarded final destination");
            return ytec::clonecore::Result<
                ytec::vssrequester::OnlineImageBackupReport>::success(
                ytec::vssrequester::OnlineImageBackupReport{
                    .workflow =
                        ytec::vssrequester::WorkflowReport{
                            .snapshot_set_id = L"{fixture}",
                            .volume_count = 1,
                            .writer_count = 1,
                            .snapshot_data_copied = true,
                            .backup_completed = true,
                            .snapshots_deleted = true,
                        },
                    .image =
                        ytec::imageformat::DcimgStreamBuildReport{
                            .image_length = 4096,
                            .stored_data_bytes = 1024,
                            .chunk_count = 1,
                            .all_chunks_read_back_verified = true,
                            .global_hash_read_back_verified = true,
                            .committed = true,
                        },
                    .final_file_committed_after_vss = true,
                });
          },
  };
}

void test_standard_user_stops_before_open() {
  bool opened = false;
  bool queried = false;
  bool executed = false;
  auto value = request();
  value.administrator = false;
  const auto result = ytec::windowsapp::execute_online_backup_job(
      value, dependencies(opened, queried, executed));
  check(!result.has_value(), "Standard-user backup must fail");
  check(
      result.error().code ==
              ytec::clonecore::ErrorCode::access_denied &&
          !opened && !queried && !executed,
      "Standard-user gate must precede physical-disk and VSS access");
}

void test_non_system_disk_stops_before_open() {
  bool opened = false;
  bool queried = false;
  bool executed = false;
  auto value = request();
  value.selected_source.is_system_disk = false;
  const auto result = ytec::windowsapp::execute_online_backup_job(
      value, dependencies(opened, queried, executed));
  check(!result.has_value(), "Non-system online backup must fail");
  check(
      !opened && !queried && !executed,
      "Unsupported source gate must precede physical-disk access");
}

void test_verified_mbr_job_reaches_executor() {
  bool opened = false;
  bool queried = false;
  bool executed = false;
  const auto result = ytec::windowsapp::execute_online_backup_job(
      request(), dependencies(opened, queried, executed));
  check(result.has_value(), "Verified MBR online job should succeed");
  check(
      opened && queried && executed &&
          result.value().final_file_committed_after_vss,
      "The complete product orchestration should reach its executor");
}

void test_stale_reidentification_stops_before_binding_query() {
  bool opened = false;
  bool queried = false;
  bool executed = false;
  auto deps = dependencies(opened, queried, executed);
  deps.open_read_only_disk =
      [&](const ytec::clonecore::StableDiskIdentity& expected) {
        opened = true;
        auto result = open_fixture(expected);
        result.value().observed.identity.serial_suffix = "STALE999";
        return result;
      };
  const auto result =
      ytec::windowsapp::execute_online_backup_job(request(), deps);
  check(!result.has_value(), "Stale source identity must fail");
  check(
      opened && !queried && !executed,
      "Identity mismatch must stop before Volume or VSS access");
}

void test_progress_callbacks_reach_image_writer_plan() {
  bool opened = false;
  bool queried = false;
  bool executed = false;
  auto value = request();
  value.callbacks = ytec::clonecore::DiskOperationCallbacks{
      .progress =
          [](const ytec::clonecore::DiskOperationProgress&) {},
      .cancellation_requested = []() { return false; },
  };
  const auto result = ytec::windowsapp::execute_online_backup_job(
      value, dependencies(opened, queried, executed, true));
  check(
      result.has_value() && opened && queried && executed,
      "Verified callbacks should follow the product orchestration path");
}

void test_16k_physical_sector_reaches_product_executor() {
  bool opened = false;
  bool queried = false;
  bool executed = false;
  const auto result = ytec::windowsapp::execute_online_backup_job(
      request(16U * 1024U),
      dependencies(
          opened, queried, executed, false, 16U * 1024U));
  check(
      result.has_value() && opened && queried && executed,
      "A system disk reporting 16 KiB physical sectors should reach the product VSS executor");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"standard_user_stops_before_open",
       test_standard_user_stops_before_open},
      {"non_system_disk_stops_before_open",
       test_non_system_disk_stops_before_open},
      {"verified_mbr_job_reaches_executor",
       test_verified_mbr_job_reaches_executor},
      {"stale_reidentification_stops_before_binding_query",
       test_stale_reidentification_stops_before_binding_query},
      {"progress_callbacks_reach_image_writer_plan",
       test_progress_callbacks_reach_image_writer_plan},
      {"physical_16k_reaches_product_executor",
       test_16k_physical_sector_reaches_product_executor},
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
