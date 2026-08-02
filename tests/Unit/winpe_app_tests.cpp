#include "ytec/winpeapp/app_runner.h"
#include "ytec/winpeapp/clone_execution_readiness.h"
#include "ytec/winpeapp/job_result.h"
#include "ytec/winpeapp/product_io_policy.h"
#include "ytec/winpeapp/repair_layout.h"
#include "ytec/winpeapp/winre_diagnostic_view.h"

#include "ytec/clonecore/result.h"
#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>

#include <algorithm>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

ytec::imageformat::Sha256Digest restore_image_hash() {
  ytec::imageformat::Sha256Digest digest{};
  digest[0] = std::byte{0x52};
  return digest;
}

class MockInventoryProvider final
    : public ytec::diskmodel::IDiskInventoryProvider {
 public:
  explicit MockInventoryProvider(ytec::diskmodel::InventoryReport report)
      : report_(std::move(report)) {}

  ytec::clonecore::Result<ytec::diskmodel::InventoryReport> enumerate()
      override {
    ++call_count;
    return ytec::clonecore::Result<
        ytec::diskmodel::InventoryReport>::success(report_);
  }

  int call_count{};

 private:
  ytec::diskmodel::InventoryReport report_;
};

class MockExecutionService final
    : public ytec::winpeapp::ICloneExecutionService {
 public:
  ytec::clonecore::Result<ytec::winpeapp::CloneExecutionReport> execute(
      const ytec::winpeapp::CloneExecutionRequest& request) override {
    ++call_count;
    last_request = request;
    ytec::clonecore::DiskOperationProgress progress;
    progress.stage =
        ytec::clonecore::DiskOperationStage::copying_data;
    progress.total_verify_bytes = report.copied_data_bytes;
    progress.verified_bytes = report.copied_data_bytes;
    ytec::clonecore::report_disk_operation_progress(
        request.callbacks, progress);
    return ytec::clonecore::Result<
        ytec::winpeapp::CloneExecutionReport>::success(report);
  }

  int call_count{};
  ytec::winpeapp::CloneExecutionRequest last_request;
  ytec::winpeapp::CloneExecutionReport report{
      .partition_style = ytec::winpeapp::ClonePartitionStyle::gpt,
      .copied_data_bytes = 123456,
      .copied_partition_count = 3,
      .recreated_partition_count = 1,
      .read_back_verified = true,
      .partition_table_committed = true,
      .target_returned_online = true,
      .boot_repair =
          ytec::bootrepair::StandaloneBootRepairReport{
              .bcdboot =
                  ytec::bootrepair::BcdBootReport{
                      .executable_path =
                          L"X:\\Windows\\System32\\bcdboot.exe",
                      .exit_code = 0,
                      .microsoft_signature_verified = true,
                  },
              .boot_store_verified = true,
          },
      .windows_partition_temporarily_mounted = true,
      .system_partition_temporarily_mounted = true,
      .temporary_mounts_released = true,
      .boot_finalization_verified = true,
  };
};

class MockRestoreExecutionService final
    : public ytec::winpeapp::IRestoreExecutionService {
 public:
  ytec::clonecore::Result<ytec::winpeapp::RestoreExecutionReport> execute(
      const ytec::winpeapp::RestoreExecutionRequest& request) override {
    ++call_count;
    last_request = request;
    ytec::clonecore::DiskOperationProgress progress;
    progress.stage =
        ytec::clonecore::DiskOperationStage::copying_data;
    progress.total_write_bytes = report.restored_data_bytes;
    progress.written_bytes = report.restored_data_bytes;
    ytec::clonecore::report_disk_operation_progress(
        request.callbacks, progress);
    return ytec::clonecore::Result<
        ytec::winpeapp::RestoreExecutionReport>::success(report);
  }

  int call_count{};
  ytec::winpeapp::RestoreExecutionRequest last_request;
  ytec::winpeapp::RestoreExecutionReport report{
      .restored_data_bytes = 987654,
      .committed_partition_table_bytes = 17408,
      .restored_chunk_count = 12,
      .complete_image_verified_before_write = true,
      .backup_manifest_verified_before_write = true,
      .read_back_verified = true,
      .partition_table_committed = true,
      .target_returned_online = true,
      .boot_repair =
          ytec::bootrepair::StandaloneBootRepairReport{
              .bcdboot =
                  ytec::bootrepair::BcdBootReport{
                      .executable_path =
                          L"X:\\Windows\\System32\\bcdboot.exe",
                      .exit_code = 0,
                      .microsoft_signature_verified = true,
                  },
              .boot_store_verified = true,
          },
      .windows_partition_temporarily_mounted = true,
      .system_partition_temporarily_mounted = true,
      .temporary_mounts_released = true,
      .boot_finalization_verified = true,
  };
};

class MockMbr2GptJobExecutionService final
    : public ytec::winpeapp::IMbr2GptJobExecutionService {
 public:
  ytec::clonecore::Result<ytec::winpeapp::Mbr2GptJobExecutionReport> execute(
      const ytec::winpeapp::Mbr2GptJobExecutionRequest& request) override {
    ++call_count;
    last_request = request;
    ytec::clonecore::DiskOperationProgress progress;
    progress.stage =
        ytec::clonecore::DiskOperationStage::validating_conversion;
    progress.cancellation_allowed = false;
    ytec::clonecore::report_disk_operation_progress(
        request.callbacks, progress);
    return ytec::clonecore::Result<
        ytec::winpeapp::Mbr2GptJobExecutionReport>::success(report);
  }

  int call_count{};
  ytec::winpeapp::Mbr2GptJobExecutionRequest last_request;
  ytec::winpeapp::Mbr2GptJobExecutionReport report{
      .clone =
          ytec::winpeapp::CloneExecutionReport{
              .partition_style = ytec::winpeapp::ClonePartitionStyle::mbr,
              .copied_data_bytes = 456789,
              .copied_partition_count = 2,
              .read_back_verified = true,
              .partition_table_committed = true,
              .target_returned_online = true,
              .boot_repair =
                  ytec::bootrepair::StandaloneBootRepairReport{
                      .bcdboot =
                          ytec::bootrepair::BcdBootReport{
                              .executable_path =
                                  L"X:\\Windows\\System32\\bcdboot.exe",
                              .exit_code = 0,
                              .microsoft_signature_verified = true,
                          },
                      .boot_store_verified = true,
                  },
              .windows_partition_temporarily_mounted = true,
              .system_partition_temporarily_mounted = true,
              .temporary_mounts_released = true,
              .boot_finalization_verified = true,
          },
      .conversion =
          ytec::bootrepair::Mbr2GptConversionReport{
              .executable_path = L"X:\\Windows\\System32\\mbr2gpt.exe",
              .disk_number = 1,
              .validation = ytec::bootrepair::ProcessResult{.exit_code = 0},
              .conversion = ytec::bootrepair::ProcessResult{.exit_code = 0},
              .microsoft_signature_verified = true,
              .target_reidentified_before_conversion = true,
          },
      .boot_repair =
          ytec::bootrepair::StandaloneBootRepairReport{
              .bcdboot =
                  ytec::bootrepair::BcdBootReport{
                      .executable_path =
                          L"X:\\Windows\\System32\\bcdboot.exe",
                      .exit_code = 0,
                      .microsoft_signature_verified = true,
                  },
              .boot_store_verified = true,
              .system_partition_temporarily_mounted = true,
              .temporary_mount_released = true,
          },
      .source_reidentified_unchanged = true,
      .target_reidentified_as_gpt = true,
      .efi_system_partition_verified = true,
      .microsoft_reserved_partition_verified = true,
      .offline_windows_verified = true,
      .windows_partition_temporarily_mounted = true,
      .temporary_windows_mount_released = true,
      .final_layout_verified = true,
  };
};

class MockBootRepairService final
    : public ytec::bootrepair::IStandaloneBootRepairService {
 public:
  ytec::clonecore::Result<ytec::bootrepair::BootRepairTargetSelection>
  inspect(const ytec::bootrepair::BootRepairTargetRequest& request) override {
    ++inspect_count;
    last_inspect_request = request;
    return ytec::clonecore::Result<
        ytec::bootrepair::BootRepairTargetSelection>::success(selection);
  }

  ytec::clonecore::Result<ytec::bootrepair::StandaloneBootRepairReport>
  execute(
      const ytec::bootrepair::StandaloneBootRepairExecutionRequest& request)
      override {
    ++execute_count;
    last_execute_request = request;
    return ytec::clonecore::Result<
        ytec::bootrepair::StandaloneBootRepairReport>::success(
        ytec::bootrepair::StandaloneBootRepairReport{
            .repaired = selection,
            .bcdboot =
                ytec::bootrepair::BcdBootReport{
                    .executable_path = L"X:\\Windows\\System32\\bcdboot.exe",
                    .exit_code = 0,
                    .microsoft_signature_verified = true,
                },
            .boot_store_verified = true,
        });
  }

  int inspect_count{};
  int execute_count{};
  ytec::bootrepair::BootRepairTargetRequest last_inspect_request;
  ytec::bootrepair::StandaloneBootRepairExecutionRequest last_execute_request;
  ytec::bootrepair::BootRepairTargetSelection selection;
};

class MockWinReDiagnosticService final
    : public ytec::bootrepair::IWinReDiagnosticService {
 public:
  ytec::clonecore::Result<
      ytec::bootrepair::WinReDiagnosticReport>
  inspect(
      const std::wstring& offline_windows_directory,
      const std::uint32_t expected_target_disk_number) override {
    ++call_count;
    received_offline_windows_directory =
        offline_windows_directory;
    received_expected_disk_number =
        expected_target_disk_number;
    if (error.has_value()) {
      return ytec::clonecore::Result<
          ytec::bootrepair::WinReDiagnosticReport>::failure(
          error.value());
    }
    return ytec::clonecore::Result<
        ytec::bootrepair::WinReDiagnosticReport>::success(
        report);
  }

  int call_count{};
  std::wstring received_offline_windows_directory;
  std::uint32_t received_expected_disk_number{};
  ytec::bootrepair::WinReDiagnosticReport report{
      .exit_code = 0U,
      .source_state =
          ytec::bootrepair::WinReSourceState::
              registered_partition,
      .registered_partition_number = 4U,
      .winre_image_size_bytes = 700ULL * 1024ULL * 1024ULL,
      .microsoft_signature_verified = true,
      .read_only_command = true,
      .registered_location_reported = true,
      .registered_location_matches_expected_disk = true,
      .registered_image_present = true,
  };
  std::optional<ytec::clonecore::Error> error;
};

class MockJobManifestLoader final
    : public ytec::winpeapp::IJobManifestLoader {
 public:
  ytec::clonecore::Result<std::vector<std::byte>> load(
      const std::wstring& path) override {
    ++call_count;
    received_path = path;
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        bytes);
  }

  int call_count{};
  std::wstring received_path;
  std::vector<std::byte> bytes;
};

class MockJobManifestCandidateProvider final
    : public ytec::winpeapp::IJobManifestCandidateProvider {
 public:
  ytec::clonecore::Result<std::vector<std::wstring>> candidates() override {
    ++call_count;
    if (error.has_value()) {
      return ytec::clonecore::Result<std::vector<std::wstring>>::failure(
          error.value());
    }
    return ytec::clonecore::Result<std::vector<std::wstring>>::success(
        paths);
  }

  int call_count{};
  std::vector<std::wstring> paths;
  std::optional<ytec::clonecore::Error> error;
};

class SequencedJobManifestLoader final
    : public ytec::winpeapp::IJobManifestLoader {
 public:
  ytec::clonecore::Result<std::vector<std::byte>> load(
      const std::wstring& path) override {
    received_paths.push_back(path);
    const std::size_t index = call_count++;
    if (index >= responses.size()) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          ytec::clonecore::Error{
              .code = ytec::clonecore::ErrorCode::query_failed,
              .native_code = ERROR_FILE_NOT_FOUND,
              .operation = L"モック予約ジョブ読取り",
              .message = L"応答がありません",
          });
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        responses[index]);
  }

  std::size_t call_count{};
  std::vector<std::wstring> received_paths;
  std::vector<std::vector<std::byte>> responses;
};

class MockRestoreImageVerifier final
    : public ytec::winpeapp::IRestoreImageVerifier {
 public:
  ytec::clonecore::Result<
      ytec::imageformat::RestoreImageInspectionReport>
  verify(const std::wstring& path) override {
    ++call_count;
    received_path = path;
    if (error.has_value()) {
      return ytec::clonecore::Result<
          ytec::imageformat::RestoreImageInspectionReport>::failure(
          error.value());
    }
    return ytec::clonecore::Result<
        ytec::imageformat::RestoreImageInspectionReport>::success(
        report);
  }

  int call_count{};
  std::wstring received_path;
  ytec::imageformat::RestoreImageInspectionReport report;
  std::optional<ytec::clonecore::Error> error;
};

class MockRestoreSafetyProbe final
    : public ytec::winpeapp::IRestoreExecutionSafetyProbe {
 public:
  ytec::clonecore::Result<
      ytec::winpeapp::RestoreExecutionSafetyObservation>
  inspect(
      const ytec::diskmodel::DiskInfo& target,
      const ytec::imageformat::RestoreImageInspectionReport& image)
      override {
    ++call_count;
    received_target_number = target.disk_number;
    received_image_length = image.image_length;
    if (error.has_value()) {
      return ytec::clonecore::Result<
          ytec::winpeapp::RestoreExecutionSafetyObservation>::failure(
          error.value());
    }
    return ytec::clonecore::Result<
        ytec::winpeapp::RestoreExecutionSafetyObservation>::success(
        observation);
  }

  int call_count{};
  std::uint32_t received_target_number{};
  std::uint64_t received_image_length{};
  ytec::winpeapp::RestoreExecutionSafetyObservation observation;
  std::optional<ytec::clonecore::Error> error;
};

class MockRestoreImageCandidateProvider final
    : public ytec::winpeapp::IRestoreImageCandidateProvider {
 public:
  ytec::clonecore::Result<std::vector<std::wstring>> candidates_for(
      const std::wstring& configured_path) override {
    ++call_count;
    received_path = configured_path;
    if (error.has_value()) {
      return ytec::clonecore::Result<
          std::vector<std::wstring>>::failure(error.value());
    }
    return ytec::clonecore::Result<
        std::vector<std::wstring>>::success(candidates);
  }

  int call_count{};
  std::wstring received_path;
  std::vector<std::wstring> candidates;
  std::optional<ytec::clonecore::Error> error;
};

class SequencedRestoreImageVerifier final
    : public ytec::winpeapp::IRestoreImageVerifier {
 public:
  ytec::clonecore::Result<
      ytec::imageformat::RestoreImageInspectionReport>
  verify(const std::wstring& path) override {
    received_paths.push_back(path);
    const std::size_t index = call_count++;
    if (index >= reports.size()) {
      return ytec::clonecore::Result<
          ytec::imageformat::RestoreImageInspectionReport>::failure(
          ytec::clonecore::Error{
              .code = ytec::clonecore::ErrorCode::query_failed,
              .native_code = ERROR_FILE_NOT_FOUND,
              .operation = L"モック復元イメージ検証",
              .message = L"候補がありません",
          });
    }
    return ytec::clonecore::Result<
        ytec::imageformat::RestoreImageInspectionReport>::success(
        reports[index]);
  }

  std::size_t call_count{};
  std::vector<std::wstring> received_paths;
  std::vector<ytec::imageformat::RestoreImageInspectionReport> reports;
};

ytec::diskmodel::DiskInfo make_disk(
    const std::uint32_t number,
    const std::uint64_t size,
    const std::string& serial,
    const ytec::diskmodel::PartitionStyle style) {
  ytec::diskmodel::DiskInfo disk;
  disk.disk_number = number;
  disk.device_path = L"\\\\.\\PhysicalDrive" + std::to_wstring(number);
  disk.device_instance_id = L"MOCK\\DISK\\" + std::to_wstring(number);
  disk.model = L"VBOX HARDDISK";
  disk.size_bytes = size;
  disk.sector_count = size / 512U;
  disk.logical_sector_size = 512;
  disk.physical_sector_size = 512;
  disk.bus_type = L"SATA";
  disk.serial_suffix = serial;
  disk.partition_style = style;
  disk.offline = false;
  disk.read_only = false;
  disk.removable = false;
  if (style == ytec::diskmodel::PartitionStyle::gpt) {
    disk.partitions.push_back(ytec::diskmodel::PartitionInfo{
        .number = 1,
        .offset_bytes = 1'048'576,
        .size_bytes = 134'217'728,
        .style = ytec::diskmodel::PartitionStyle::gpt,
        .type = L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}",
    });
  }
  return disk;
}

ytec::diskmodel::InventoryReport valid_report() {
  ytec::diskmodel::InventoryReport report;
  report.disks.push_back(make_disk(
      0, 2ULL * 1024U * 1024U * 1024U, "SOURCE01",
      ytec::diskmodel::PartitionStyle::gpt));
  report.disks.push_back(make_disk(
      1, 3ULL * 1024U * 1024U * 1024U, "TARGET01",
      ytec::diskmodel::PartitionStyle::raw));
  return report;
}

ytec::diskmodel::InventoryReport valid_mbr_to_gpt_report() {
  auto report = valid_report();
  report.disks[0].partition_style = ytec::diskmodel::PartitionStyle::mbr;
  report.disks[0].partitions = {
      ytec::diskmodel::PartitionInfo{
          .number = 1,
          .offset_bytes = 1'048'576,
          .size_bytes = 134'217'728,
          .style = ytec::diskmodel::PartitionStyle::mbr,
          .type = L"0x07",
          .bootable = true,
      },
  };
  return report;
}

std::vector<std::byte> clone_job_bytes_for_report(
    const ytec::diskmodel::InventoryReport& report) {
  check(report.disks.size() >= 2,
        "Clone job fixture requires source and target disks");
  const auto source =
      ytec::diskmodel::make_stable_disk_identity(
          report.disks[0], report.disks[0].is_system_disk);
  const auto target =
      ytec::diskmodel::make_stable_disk_identity(
          report.disks[1], report.disks[1].is_system_disk);
  check(source.has_value(), "Job source identity fixture should be valid");
  check(target.has_value(), "Job target identity fixture should be valid");
  const auto serialized =
      ytec::imageformat::serialize_hashed_job_manifest(
          ytec::imageformat::JobManifest{
              .schema_version =
                  ytec::imageformat::kJobManifestSchemaVersion,
              .job_type = ytec::imageformat::JobType::clone,
              .source = source.value(),
              .target = target.value(),
              .image_path = {},
              .requested_conversion =
                  ytec::imageformat::RequestedConversion::preserve,
              .created_utc = "2026-07-31T03:20:00Z",
              .app_version = "0.1.0-dev",
              .destructive_target_confirmed = true,
          });
  check(serialized.has_value(), "Job fixture should serialize");
  return serialized.value();
}

std::vector<std::byte> valid_clone_job_bytes() {
  return clone_job_bytes_for_report(valid_report());
}

std::vector<std::byte> valid_mbr_to_gpt_job_bytes() {
  const auto report = valid_mbr_to_gpt_report();
  const auto source = ytec::diskmodel::make_stable_disk_identity(
      report.disks[0], report.disks[0].is_system_disk);
  const auto target = ytec::diskmodel::make_stable_disk_identity(
      report.disks[1], report.disks[1].is_system_disk);
  check(source.has_value() && target.has_value(),
        "MBR-to-GPT job identities should be valid");
  const auto serialized = ytec::imageformat::serialize_hashed_job_manifest(
      ytec::imageformat::JobManifest{
          .schema_version = ytec::imageformat::kJobManifestSchemaVersion,
          .job_type = ytec::imageformat::JobType::mbr_to_gpt,
          .source = source.value(),
          .target = target.value(),
          .requested_conversion =
              ytec::imageformat::RequestedConversion::mbr_to_gpt,
          .created_utc = "2026-08-01T12:00:00Z",
          .app_version = "0.2.0-dev",
          .execution_mode =
              ytec::imageformat::JobExecutionMode::review_required,
          .destructive_target_confirmed = true,
      });
  check(serialized.has_value(), "MBR-to-GPT job fixture should serialize");
  return serialized.value();
}

std::vector<std::byte> restore_job_bytes_for_report(
    const ytec::diskmodel::InventoryReport& report) {
  check(report.disks.size() >= 2,
        "Restore job fixture requires a target disk");
  const auto target =
      ytec::diskmodel::make_stable_disk_identity(
          report.disks[1], report.disks[1].is_system_disk);
  check(target.has_value(), "Restore target identity fixture should be valid");
  const auto serialized =
      ytec::imageformat::serialize_hashed_job_manifest(
          ytec::imageformat::JobManifest{
              .schema_version =
                  ytec::imageformat::kJobManifestSchemaVersion,
              .job_type = ytec::imageformat::JobType::restore_image,
              .source = std::nullopt,
              .target = target.value(),
              .image_path = L"E:\\Tsumugi\\system.dcimg",
              .restore_image_identity =
                  ytec::imageformat::RestoreImageIdentity{
                      .length_bytes = 4096,
                      .global_hash = restore_image_hash(),
                  },
              .requested_conversion =
                  ytec::imageformat::RequestedConversion::preserve,
              .created_utc = "2026-07-31T03:20:00Z",
              .app_version = "0.1.0-dev",
              .destructive_target_confirmed = true,
          });
  check(serialized.has_value(), "Restore job fixture should serialize");
  return serialized.value();
}

std::vector<std::byte> valid_restore_job_bytes() {
  return restore_job_bytes_for_report(valid_report());
}

void test_job_candidate_paths_are_fixed_and_exclude_winpe_ram_drive() {
  constexpr std::uint32_t kDriveC = 1U << (L'C' - L'A');
  constexpr std::uint32_t kDriveE = 1U << (L'E' - L'A');
  constexpr std::uint32_t kDriveX = 1U << (L'X' - L'A');
  const auto paths = ytec::winpeapp::build_job_manifest_candidate_paths(
      kDriveC | kDriveE | kDriveX);

  check(paths.size() == 8U,
        "Each eligible drive should produce exactly four fixed candidates");
  check(paths[0] == L"C:\\Tsumugi\\Tsumugi-clone-job.json" &&
            paths[3] == L"C:\\Tsumugi-restore-job.json" &&
            paths[4] == L"E:\\Tsumugi\\Tsumugi-clone-job.json" &&
            paths[7] == L"E:\\Tsumugi-restore-job.json",
        "Job discovery must use only the documented fixed names");
  check(std::none_of(
            paths.begin(),
            paths.end(),
            [](const std::wstring& path) { return path.starts_with(L"X:"); }),
        "WinPE X: RAM drive must never be searched for a job");
}

void test_job_discovery_accepts_one_verified_candidate() {
  MockJobManifestCandidateProvider provider;
  provider.paths = {L"E:\\Tsumugi\\Tsumugi-clone-job.json"};
  MockJobManifestLoader loader;
  loader.bytes = valid_clone_job_bytes();

  const auto result = ytec::winpeapp::discover_unique_job_manifest(
      provider, loader);
  check(result.has_value() && result.value().has_value(),
        "One canonical hash-verified job should be discovered");
  check(result.value()->path == provider.paths.front() &&
            result.value()->verified_job.manifest.job_type ==
                ytec::imageformat::JobType::clone,
        "Discovery should retain the verified job and exact path");
  check(provider.call_count == 1 && loader.call_count == 1,
        "One discovered candidate should be enumerated and loaded once");
}

void test_job_discovery_allows_no_candidate_without_loading() {
  MockJobManifestCandidateProvider provider;
  MockJobManifestLoader loader;

  const auto result = ytec::winpeapp::discover_unique_job_manifest(
      provider, loader);
  check(result.has_value() && !result.value().has_value(),
        "No fixed-name job should be a normal empty discovery result");
  check(loader.call_count == 0,
        "No manifest should be opened when no candidate exists");
}

void test_job_discovery_rejects_multiple_verified_candidates() {
  MockJobManifestCandidateProvider provider;
  provider.paths = {
      L"E:\\Tsumugi\\Tsumugi-clone-job.json",
      L"F:\\Tsumugi\\Tsumugi-clone-job.json",
  };
  MockJobManifestLoader loader;
  loader.bytes = valid_clone_job_bytes();

  const auto result = ytec::winpeapp::discover_unique_job_manifest(
      provider, loader);
  check(!result.has_value() &&
            result.error().code == ytec::clonecore::ErrorCode::invalid_data,
        "Multiple valid jobs must fail instead of selecting by drive order");
  check(loader.call_count == 2,
        "Every bounded candidate should be verified before ambiguity is reported");
}

void test_job_discovery_rejects_tampered_candidate_alongside_valid_job() {
  MockJobManifestCandidateProvider provider;
  provider.paths = {
      L"E:\\Tsumugi\\Tsumugi-clone-job.json",
      L"F:\\Tsumugi\\Tsumugi-restore-job.json",
  };
  SequencedJobManifestLoader loader;
  loader.responses = {
      valid_clone_job_bytes(),
      std::vector<std::byte>{std::byte{'{'}, std::byte{'}'}},
  };

  const auto result = ytec::winpeapp::discover_unique_job_manifest(
      provider, loader);
  check(!result.has_value(),
        "A tampered fixed-name candidate must block automatic selection");
  check(loader.call_count == 2,
        "Discovery should not silently ignore a tampered candidate");
}

void test_job_discovery_rejects_case_insensitive_duplicate_paths() {
  MockJobManifestCandidateProvider provider;
  provider.paths = {
      L"E:\\Tsumugi\\Tsumugi-clone-job.json",
      L"e:\\tsumugi\\tsumugi-clone-job.json",
  };
  MockJobManifestLoader loader;
  loader.bytes = valid_clone_job_bytes();

  const auto result = ytec::winpeapp::discover_unique_job_manifest(
      provider, loader);
  check(!result.has_value() &&
            result.error().code == ytec::clonecore::ErrorCode::invalid_data,
        "Windows-equivalent duplicate paths must fail closed");
  check(loader.call_count == 0,
        "Duplicate paths should be rejected before opening a manifest");
}

void test_hash_locked_job_loader_rejects_post_review_replacement() {
  const auto approved_bytes = valid_clone_job_bytes();
  const auto approved =
      ytec::imageformat::parse_and_verify_hashed_job_manifest(approved_bytes);
  check(approved.has_value(), "Approved job fixture should verify");

  auto matching_inner = std::make_unique<MockJobManifestLoader>();
  matching_inner->bytes = approved_bytes;
  auto matching = ytec::winpeapp::make_hash_locked_job_manifest_loader(
      std::move(matching_inner), approved.value().payload_hash);
  check(matching->load(L"E:\\Tsumugi\\Tsumugi-clone-job.json").has_value(),
        "The exact preflight-approved job should remain loadable");

  auto replaced_inner = std::make_unique<MockJobManifestLoader>();
  replaced_inner->bytes = valid_restore_job_bytes();
  auto replaced = ytec::winpeapp::make_hash_locked_job_manifest_loader(
      std::move(replaced_inner), approved.value().payload_hash);
  const auto result =
      replaced->load(L"E:\\Tsumugi\\Tsumugi-clone-job.json");
  check(!result.has_value() &&
            result.error().code ==
                ytec::clonecore::ErrorCode::identity_mismatch,
        "A canonical but replaced job must stop before disk enumeration");
}

void test_job_result_log_is_bounded_and_correlated_to_job() {
  ytec::imageformat::Sha256Digest job_hash{};
  job_hash[0] = std::byte{0xA5};
  job_hash[31] = std::byte{0x5A};
  const auto serialized = ytec::winpeapp::serialize_job_result_log(
      ytec::winpeapp::JobResultRecord{
          .job_payload_hash = job_hash,
          .job_type = ytec::imageformat::JobType::restore_image,
          .outcome = ytec::winpeapp::JobResultOutcome::passed,
          .completed_utc = "2026-08-01T12:34:56Z",
          .app_version = "0.2.0",
          .details_utf8 = "result details\n",
      });
  check(serialized.has_value() &&
            serialized.value().size() <=
                ytec::winpeapp::kMaximumJobResultLogBytes,
        "A valid result log should serialize within the fixed bound");
  const std::string text(
      reinterpret_cast<const char*>(serialized.value().data()),
      serialized.value().size());
  check(
      text.find(
          "jobHashSha256=A50000000000000000000000000000000000000000000000000000000000005A") !=
              std::string::npos &&
          text.find("jobType=restore-image") != std::string::npos &&
          text.find("result=PASS") != std::string::npos &&
          text.find("detailsSha256=") != std::string::npos,
      "Result log should correlate to the verified job and hash its details");
  const auto parsed = ytec::imageformat::parse_and_verify_job_result_log(
      serialized.value());
  check(parsed.has_value() &&
            parsed.value().job_payload_hash == job_hash &&
            parsed.value().job_type ==
                ytec::imageformat::JobType::restore_image &&
            parsed.value().outcome ==
                ytec::imageformat::JobResultOutcome::passed &&
            parsed.value().details_utf8 == "result details\n",
        "Windows-side parsing should strictly verify the canonical result log");

  auto tampered = serialized.value();
  tampered.back() = std::byte{'X'};
  check(!ytec::imageformat::parse_and_verify_job_result_log(tampered)
             .has_value(),
        "A tampered result detail must fail its recorded SHA-256");
  auto trailing = serialized.value();
  trailing.push_back(std::byte{'\n'});
  check(!ytec::imageformat::parse_and_verify_job_result_log(trailing)
             .has_value(),
        "Trailing bytes must make a result log noncanonical");

  auto oversized = ytec::winpeapp::serialize_job_result_log(
      ytec::winpeapp::JobResultRecord{
          .job_payload_hash = job_hash,
          .job_type = ytec::imageformat::JobType::clone,
          .outcome = ytec::winpeapp::JobResultOutcome::failed,
          .completed_utc = "2026-08-01T12:34:56Z",
          .app_version = "0.2.0",
          .details_utf8 = std::string(49U * 1024U, 'x'),
      });
  check(!oversized.has_value(),
        "Oversized result details must fail before file creation");

  auto invalid_utf8 = ytec::winpeapp::serialize_job_result_log(
      ytec::winpeapp::JobResultRecord{
          .job_payload_hash = job_hash,
          .job_type = ytec::imageformat::JobType::clone,
          .outcome = ytec::winpeapp::JobResultOutcome::failed,
          .completed_utc = "2026-08-01T12:34:56Z",
          .app_version = "0.2.0",
          .details_utf8 = std::string(1U, static_cast<char>(0xFF)),
      });
  check(!invalid_utf8.has_value(),
        "Invalid UTF-8 result details must fail before file creation");
}

void test_job_result_path_is_timestamped_beside_job() {
  const auto path = ytec::winpeapp::build_job_result_log_path(
      L"E:\\Tsumugi\\Tsumugi-clone-job.json",
      L"20260801-123456Z");
  check(path.has_value() &&
            path.value() ==
                L"E:\\Tsumugi\\Tsumugi-clone-job.result-20260801-123456Z.log",
        "Result path should be timestamped beside the job without replacing it");
  check(
      !ytec::winpeapp::build_job_result_log_path(
           L"\\\\server\\share\\job.json", L"20260801-123456Z")
           .has_value() &&
          !ytec::winpeapp::build_job_result_log_path(
           L"E:\\job.json", L"2026-08-01T12:34:56Z")
           .has_value(),
      "UNC paths and noncompact timestamps must be rejected");
}

void test_job_result_file_is_create_new_and_read_back_verified() {
  std::array<wchar_t, MAX_PATH + 1U> temporary_root{};
  const DWORD root_length = GetTempPathW(
      static_cast<DWORD>(temporary_root.size()), temporary_root.data());
  check(root_length > 0U && root_length < temporary_root.size(),
        "A local temporary path should be available");
  const std::wstring path =
      std::wstring(temporary_root.data(), root_length) +
      L"ytec-tsumugi-job-result-" + std::to_wstring(GetCurrentProcessId()) +
      L"-" + std::to_wstring(GetTickCount64()) + L".log";
  const std::vector<std::byte> bytes{
      std::byte{'P'}, std::byte{'A'}, std::byte{'S'}, std::byte{'S'},
      std::byte{'\n'}};

  const auto first = ytec::winpeapp::write_new_job_result_log(path, bytes);
  check(first.has_value(),
        "A new local result log should be written and read back");
  const auto second = ytec::winpeapp::write_new_job_result_log(
      path, std::vector<std::byte>{std::byte{'X'}});
  check(!second.has_value(),
        "An existing result log must never be replaced");

  ytec::clonecore::UniqueHandle file(CreateFileW(
      path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  check(static_cast<bool>(file),
        "The original result log should remain after replacement rejection");
  std::array<std::byte, 5> observed{};
  DWORD read{};
  check(ReadFile(
            file.get(),
            observed.data(),
            static_cast<DWORD>(observed.size()),
            &read,
            nullptr) != FALSE &&
            static_cast<std::size_t>(read) == observed.size() &&
            std::equal(observed.begin(), observed.end(), bytes.begin()),
        "Replacement rejection must preserve the first result bytes");
  file.reset();
  check(DeleteFileW(path.c_str()) != FALSE,
        "The test-owned temporary result log should be removable");
}

std::wstring valid_restore_confirmation_token() {
  const auto report = valid_report();
  const auto target =
      ytec::diskmodel::make_stable_disk_identity(
          report.disks[1], report.disks[1].is_system_disk);
  check(target.has_value(),
        "Restore confirmation fixture identity should be valid");
  return ytec::clonecore::make_target_confirmation_token(target.value());
}

std::wstring valid_clone_confirmation_token() {
  return valid_restore_confirmation_token();
}

ytec::imageformat::RestoreImageInspectionReport
valid_restore_image_report() {
  ytec::imageformat::RestoreImageInspectionReport report;
  report.canonical_path = L"E:\\Tsumugi\\system.dcimg";
  report.image_length = 4096;
  report.global_hash = restore_image_hash();
  report.header.source_disk_size =
      2ULL * 1024U * 1024U * 1024U;
  report.header.logical_sector_size = 512;
  report.manifest.source =
      ytec::clonecore::StableDiskIdentity{
          .disk_number = 9,
          .model = L"IMAGE SOURCE",
          .size_bytes = report.header.source_disk_size,
          .logical_sector_size = 512,
          .serial_suffix = "IMAGE001",
          .device_instance_id = L"MOCK\\IMAGE\\9",
          .is_system_disk = false,
      };
  report.complete_container_verified = true;
  report.metadata_verified = true;
  report.restore_layout_verified = true;
  report.restore_execution_enabled = false;
  return report;
}

ytec::winpeapp::RestoreExecutionSafetyObservation
all_restore_safety_checks_passed() {
  using ytec::winpeapp::RestoreSafetyState;
  return ytec::winpeapp::RestoreExecutionSafetyObservation{
      .bitlocker_fully_decrypted = RestoreSafetyState::passed,
      .dynamic_disk_absent = RestoreSafetyState::passed,
      .storage_spaces_absent = RestoreSafetyState::passed,
      .file_system_layout_supported = RestoreSafetyState::passed,
      .stable_power = RestoreSafetyState::passed,
      .pending_restart_absent = RestoreSafetyState::passed,
  };
}

ytec::bootrepair::BootRepairTargetSelection boot_repair_selection() {
  auto report = valid_report();
  auto disk = report.disks[0];
  disk.partitions.clear();
  disk.partitions.push_back(ytec::diskmodel::PartitionInfo{
      .number = 1,
      .offset_bytes = 1'048'576,
      .size_bytes = 272'629'760,
      .style = ytec::diskmodel::PartitionStyle::gpt,
      .type = L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}",
  });
  disk.partitions.push_back(ytec::diskmodel::PartitionInfo{
      .number = 3,
      .offset_bytes = 290'455'552,
      .size_bytes = 1ULL * 1024U * 1024U * 1024U,
      .style = ytec::diskmodel::PartitionStyle::gpt,
      .type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}",
  });
  auto identity = ytec::diskmodel::make_stable_disk_identity(disk, false);
  check(identity.has_value(), "Boot repair fixture identity should be valid");
  return ytec::bootrepair::BootRepairTargetSelection{
      .disk = disk,
      .identity = identity.take_value(),
      .windows_partition = disk.partitions[1],
      .system_partition = disk.partitions[0],
  };
}

void test_help_is_read_only_and_does_not_enumerate() {
  MockInventoryProvider provider(valid_report());
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--help"}, provider, output, errors);

  check(exit_code == 0, "Help should succeed");
  check(provider.call_count == 0, "Help must not enumerate disks");
  check(output.str().find("--clone-preflight") != std::string::npos,
        "Help should describe the read-only preflight");
  check(output.str().find("書き込み・クローン・復元機能はありません") !=
            std::string::npos,
        "Help must keep the no-write boundary explicit");
  check(errors.str().empty(), "Help should not write stderr");
}

void test_job_preflight_verifies_hash_and_re_resolves_disks() {
  MockInventoryProvider provider(valid_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_clone_job_bytes();
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-preflight", L"--job-path", L"E:\\Tsumugi\\job.json",
       L"--json"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader);

  check(exit_code == 0, "Valid hashed job should pass preflight");
  check(loader.call_count == 1, "Job file should be loaded exactly once");
  check(
      loader.received_path == L"E:\\Tsumugi\\job.json",
      "Loader must receive the explicit absolute path");
  check(provider.call_count == 1, "Disks should be enumerated once");
  check(
      output.str().find("\"mode\":\"job-preflight\"") !=
          std::string::npos,
      "JSON should identify job preflight");
  check(
      output.str().find("\"jobHashVerified\":true") !=
          std::string::npos,
      "JSON should report successful hash verification");
  check(
      output.str().find("\"executionMode\":\"review-required\"") !=
          std::string::npos,
      "JSON should expose that the default job still needs manual review");
  check(
      output.str().find("\"stableIdentityResolved\":true") !=
          std::string::npos,
      "JSON should report stable identity resolution");
  check(
      output.str().find("\"executionEnabled\":false") !=
          std::string::npos,
      "Job preflight must remain read-only");
  check(errors.str().empty(), "Valid job should not use stderr");
}

void test_clone_readiness_fails_closed_on_unknown_or_non_raw_layout() {
  auto report = valid_report();
  auto status =
      ytec::winpeapp::validate_clone_execution_observation(
          report.disks[0], report.disks[1]);
  check(status.has_value(),
        "A fixed basic GPT source and empty RAW target should be eligible");

  report.disks[0].partition_style =
      ytec::diskmodel::PartitionStyle::mbr;
  report.disks[0].partitions.clear();
  report.disks[0].partitions.push_back(
      ytec::diskmodel::PartitionInfo{
          .number = 1,
          .style = ytec::diskmodel::PartitionStyle::mbr,
          .type = L"0x07",
          .bootable = true,
      });
  status = ytec::winpeapp::validate_clone_execution_observation(
      report.disks[0], report.disks[1]);
  check(status.has_value(),
        "A fixed basic MBR source and empty RAW target should be eligible");

  report = valid_report();
  report.disks[0].bus_type = L"Unknown";
  status = ytec::winpeapp::validate_clone_execution_observation(
      report.disks[0], report.disks[1]);
  check(!status.has_value(),
        "Unknown source bus must fail closed before target state changes");

  report = valid_report();
  report.disks[1].partition_style =
      ytec::diskmodel::PartitionStyle::gpt;
  report.disks[1].partitions.push_back(
      ytec::diskmodel::PartitionInfo{
          .number = 1,
          .style = ytec::diskmodel::PartitionStyle::gpt,
          .type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}",
      });
  status = ytec::winpeapp::validate_clone_execution_observation(
      report.disks[0], report.disks[1]);
  check(!status.has_value(),
        "A non-RAW target must fail before the destructive service");

  report = valid_report();
  report.disks[0].partitions[0].type =
      L"{AF9B60A0-1431-4F62-BC68-3311714A69AD}";
  status = ytec::winpeapp::validate_clone_execution_observation(
      report.disks[0], report.disks[1]);
  check(!status.has_value(),
        "LDM metadata must fail closed before target state changes");
}

void test_clone_job_execution_passes_verified_contract_and_progress() {
  MockInventoryProvider provider(valid_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_clone_job_bytes();
  MockExecutionService service;
  int progress_count = 0;
  std::uint64_t verified_bytes = 0;
  const ytec::clonecore::DiskOperationCallbacks callbacks{
      .progress =
          [&](const ytec::clonecore::DiskOperationProgress& progress) {
            ++progress_count;
            verified_bytes = progress.verified_bytes;
          },
  };
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-execute", L"--job-path", L"E:\\Tsumugi\\clone-job.json",
       L"--acknowledge-target-erasure", L"--confirmation",
       valid_clone_confirmation_token(), L"--json"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      nullptr,
      nullptr,
      nullptr,
      &callbacks,
      nullptr,
      &service);

  check(exit_code == 0, "Verified mock clone job execution should succeed");
  check(loader.call_count == 1 && provider.call_count == 1,
        "Clone job must be hash-checked and resolved once before service use");
  check(service.call_count == 1,
        "Verified clone job should reach only the product job service");
  check(service.last_request.expected_source.serial_suffix == "SOURCE01" &&
            service.last_request.expected_target.serial_suffix == "TARGET01",
        "Clone service must receive freshly resolved stable identities");
  check(service.last_request.authorization.empty(),
        "Product clone jobs must never receive a VM authorization token");
  check(service.last_request.confirmation.first_step_acknowledged &&
            service.last_request.confirmation.typed_token ==
                valid_clone_confirmation_token(),
        "Clone service must receive the exact two-stage confirmation");
  check(progress_count == 1 &&
            verified_bytes == service.report.copied_data_bytes,
        "Clone progress callbacks must cross the job coordinator boundary");
  check(output.str().find("\"mode\":\"clone-execution\"") !=
            std::string::npos &&
            output.str().find("\"partitionStyle\":\"GPT\"") !=
                std::string::npos &&
            output.str().find("\"targetReturnedOnline\":true") !=
                std::string::npos,
        "Successful JSON must contain the verified clone result");
  check(errors.str().empty(), "Successful mock clone should be quiet");
}

void test_clone_job_execution_rejects_incomplete_service_report() {
  MockInventoryProvider provider(valid_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_clone_job_bytes();
  MockExecutionService service;
  service.report.target_returned_online = false;
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-execute", L"--job-path", L"E:\\Tsumugi\\clone-job.json",
       L"--acknowledge-target-erasure", L"--confirmation",
       valid_clone_confirmation_token()},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &service);

  check(exit_code == 1,
        "A clone report without target online recovery must fail closed");
  check(service.call_count == 1,
        "Coordinator should validate exactly one clone service report");
  check(output.str().empty(),
        "Incomplete clone service report must not emit PASS");
  check(errors.str().find("オンライン復帰") != std::string::npos,
        "Incomplete clone report should explain its failed completion gate");
}

void test_clone_job_execution_rejects_wrong_confirmation_before_service() {
  MockInventoryProvider provider(valid_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_clone_job_bytes();
  MockExecutionService service;
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-execute", L"--job-path", L"E:\\Tsumugi\\clone-job.json",
       L"--acknowledge-target-erasure", L"--confirmation", L"WRONG"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &service);

  check(exit_code == 1, "Wrong clone confirmation must fail closed");
  check(service.call_count == 0,
        "Wrong confirmation must never reach the destructive service");
  check(output.str().empty(), "Wrong confirmation must not emit PASS");
}

void test_mbr_to_gpt_preflight_requires_mbr_source_and_raw_target() {
  MockInventoryProvider provider(valid_mbr_to_gpt_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_mbr_to_gpt_job_bytes();
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-preflight", L"--job-path",
       L"E:\\Tsumugi\\Tsumugi-clone-job.json", L"--json"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader);

  check(exit_code == 0,
        "A canonical MBR-to-GPT job should pass read-only preflight");
  check(provider.call_count == 1 && loader.call_count == 1,
        "Migration preflight should verify one job and one fresh inventory");
  check(output.str().find("\"jobType\":\"mbr-to-gpt\"") !=
            std::string::npos &&
            output.str().find("\"executionEnabled\":false") !=
                std::string::npos,
        "Migration preflight must remain read-only and identify its job type");
  check(errors.str().empty(), "Valid migration preflight should be quiet");
}

void test_mbr_to_gpt_job_execution_passes_complete_verified_contract() {
  MockInventoryProvider provider(valid_mbr_to_gpt_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_mbr_to_gpt_job_bytes();
  MockMbr2GptJobExecutionService service;
  int progress_count = 0;
  const ytec::clonecore::DiskOperationCallbacks callbacks{
      .progress =
          [&](const ytec::clonecore::DiskOperationProgress& progress) {
            ++progress_count;
            check(
                progress.stage ==
                    ytec::clonecore::DiskOperationStage::
                        validating_conversion,
                "Migration progress should cross the coordinator boundary");
          },
  };
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-execute", L"--job-path",
       L"E:\\Tsumugi\\Tsumugi-clone-job.json",
       L"--acknowledge-target-erasure", L"--confirmation",
       valid_clone_confirmation_token(), L"--json"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      nullptr,
      nullptr,
      nullptr,
      &callbacks,
      nullptr,
      nullptr,
      nullptr,
      &service);

  check(exit_code == 0,
        "A fully verified mock MBR-to-GPT execution should succeed");
  check(service.call_count == 1 && progress_count == 1,
        "The product migration service and progress callback should run once");
  check(service.last_request.expected_source.serial_suffix == "SOURCE01" &&
            service.last_request.expected_target.serial_suffix == "TARGET01" &&
            service.last_request.confirmation.typed_token ==
                valid_clone_confirmation_token(),
        "Migration service must receive the hash-bound identities and confirmation");
  check(output.str().find("\"mode\":\"mbr-to-gpt-execution\"") !=
            std::string::npos &&
            output.str().find("\"efiSystemPartitionVerified\":true") !=
                std::string::npos &&
            output.str().find(
                "disconnect-source-and-boot-target-in-uefi") !=
                std::string::npos,
        "Migration PASS must report conversion, boot verification, and next action");
  check(errors.str().empty(), "Successful mock migration should be quiet");
}

void test_mbr_to_gpt_job_execution_rejects_incomplete_report() {
  MockInventoryProvider provider(valid_mbr_to_gpt_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_mbr_to_gpt_job_bytes();
  MockMbr2GptJobExecutionService service;
  service.report.microsoft_reserved_partition_verified = false;
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-execute", L"--job-path",
       L"E:\\Tsumugi\\Tsumugi-clone-job.json",
       L"--acknowledge-target-erasure", L"--confirmation",
       valid_clone_confirmation_token()},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &service);

  check(exit_code == 1 && service.call_count == 1,
        "An incomplete migration report must fail after one service call");
  check(output.str().empty(), "Incomplete migration must never emit PASS");
  check(errors.str().find("完全確認できません") != std::string::npos,
        "Incomplete migration should explain its final verification gate");
}

void test_restore_job_reverifies_image_before_disk_scan() {
  MockInventoryProvider provider(valid_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_restore_job_bytes();
  MockRestoreImageVerifier verifier;
  verifier.report = valid_restore_image_report();
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-preflight", L"--job-path",
       L"E:\\Tsumugi\\restore-job.json", L"--json"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      &verifier);

  check(exit_code == 0, "Verified restore job should pass preflight");
  check(loader.call_count == 1, "Restore job should be loaded exactly once");
  check(verifier.call_count == 1,
        "Restore image should be verified exactly once");
  check(verifier.received_path == L"E:\\Tsumugi\\system.dcimg",
        "Verifier must receive the hashed job image path");
  check(provider.call_count == 1,
        "Disk scan should run only after image verification succeeds");
  check(output.str().find("\"imageVerified\":true") !=
            std::string::npos,
        "JSON should report complete image verification");
  check(output.str().find("\"imageLengthBytes\":4096") !=
            std::string::npos,
        "JSON should report the verified image length");
  check(output.str().find("\"imageSourceDiskBytes\":2147483648") !=
            std::string::npos,
        "JSON should report the original source disk capacity");
  check(output.str().find("\"allChecksPassed\":false") !=
            std::string::npos,
        "Missing runtime probes must fail closed");
  check(output.str().find(
            "\"id\":\"storageSpacesAbsent\",\"required\":true,"
            "\"state\":\"unknown\"") !=
            std::string::npos,
        "JSON should report Storage Spaces as an unknown safety gate");
  check(output.str().find("\"executionEnabled\":false") !=
            std::string::npos,
        "Restore job preflight must remain read-only");
  check(errors.str().empty(), "Valid restore job should not use stderr");
}

void test_restore_readiness_model_fails_closed_on_unknown_or_blocked() {
  using ytec::winpeapp::RestoreSafetyState;
  const auto unknown =
      ytec::winpeapp::evaluate_restore_execution_readiness({});
  check(!unknown.required_checks_passed,
        "Unknown required observations must not satisfy preconditions");
  check(!unknown.all_checks_passed,
        "Default unknown observations must not be ready");
  check(unknown.checks.size() ==
            ytec::winpeapp::kRestoreSafetyCheckCount,
        "Readiness report must retain every required check");
  for (const auto& finding : unknown.checks) {
    check(finding.state == RestoreSafetyState::unknown,
          "Every default finding must remain unknown");
  }

  auto observation = all_restore_safety_checks_passed();
  observation.dynamic_disk_absent = RestoreSafetyState::blocked;
  const auto blocked =
      ytec::winpeapp::evaluate_restore_execution_readiness(observation);
  check(!blocked.required_checks_passed,
        "One explicitly blocked required check must stop preconditions");
  check(!blocked.all_checks_passed,
        "One explicitly blocked safety check must stop readiness");

  observation = all_restore_safety_checks_passed();
  observation.pending_restart_absent = RestoreSafetyState::unknown;
  const auto advisory =
      ytec::winpeapp::evaluate_restore_execution_readiness(observation);
  check(advisory.required_checks_passed,
        "Pending restart warning must remain advisory per specification");
  check(!advisory.all_checks_passed,
        "An advisory unknown must remain visible in the full check state");
}

void test_restore_safety_derivation_rejects_unsupported_disk_types() {
  using ytec::winpeapp::RestoreSafetyState;
  auto target = valid_report().disks[1];
  target.bus_type = L"SATA";
  auto image = valid_restore_image_report();
  image.manifest.bitlocker_fully_decrypted = true;
  image.manifest.partitions.push_back(
      ytec::imageformat::BackupManifestPartition{
          .table_index = 1,
          .offset_bytes = 1'048'576,
          .length_bytes = 1'073'741'824,
          .role =
              ytec::imageformat::BackupPartitionRole::windows_ntfs,
          .file_system = ytec::imageformat::BackupFileSystem::ntfs,
          .cluster_size = 4096,
          .name = L"Windows",
      });

  const auto normal =
      ytec::winpeapp::derive_restore_execution_safety_observation(
          target, image, RestoreSafetyState::passed);
  check(normal.bitlocker_fully_decrypted == RestoreSafetyState::passed &&
            normal.dynamic_disk_absent == RestoreSafetyState::passed &&
            normal.storage_spaces_absent == RestoreSafetyState::passed &&
            normal.file_system_layout_supported ==
                RestoreSafetyState::passed &&
            normal.stable_power == RestoreSafetyState::passed,
        "Verified basic-disk restore evidence should pass derivable gates");
  check(normal.pending_restart_absent == RestoreSafetyState::unknown,
        "Read-only evidence must not invent pending-restart state");

  target.partition_style = ytec::diskmodel::PartitionStyle::mbr;
  target.partitions.push_back(ytec::diskmodel::PartitionInfo{
      .number = 1,
      .style = ytec::diskmodel::PartitionStyle::mbr,
      .type = L"0x42",
  });
  const auto dynamic =
      ytec::winpeapp::derive_restore_execution_safety_observation(
          target, image, RestoreSafetyState::passed);
  check(dynamic.dynamic_disk_absent == RestoreSafetyState::blocked,
        "MBR LDM partition type must block restore readiness");

  target.partitions.clear();
  target.bus_type = L"Storage Spaces";
  const auto storage_spaces =
      ytec::winpeapp::derive_restore_execution_safety_observation(
          target, image, RestoreSafetyState::passed);
  check(storage_spaces.storage_spaces_absent ==
            RestoreSafetyState::blocked,
        "Storage Spaces bus type must block restore readiness");

  target.bus_type = L"Unknown";
  const auto unknown_bus =
      ytec::winpeapp::derive_restore_execution_safety_observation(
          target, image, RestoreSafetyState::passed);
  check(unknown_bus.storage_spaces_absent ==
            RestoreSafetyState::unknown,
        "Unknown bus type must not be assumed to be non-Storage-Spaces");
}

void test_restore_readiness_all_passed_still_does_not_enable_execution() {
  MockInventoryProvider provider(valid_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_restore_job_bytes();
  MockRestoreImageVerifier verifier;
  verifier.report = valid_restore_image_report();
  MockRestoreSafetyProbe safety_probe;
  safety_probe.observation = all_restore_safety_checks_passed();
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-preflight", L"--job-path",
       L"E:\\Tsumugi\\restore-job.json", L"--json"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      &verifier,
      &safety_probe);

  check(exit_code == 0, "All passed read-only probes should be reportable");
  check(safety_probe.call_count == 1,
        "Safety probe should inspect exactly once after base preflight");
  check(safety_probe.received_target_number == 1,
        "Safety probe must receive the stable-identity resolved target");
  check(safety_probe.received_image_length == 4096,
        "Safety probe must receive the fully verified image report");
  check(output.str().find("\"allChecksPassed\":true") !=
            std::string::npos,
        "JSON should distinguish all passed runtime checks");
  check(output.str().find("\"requiredChecksPassed\":true") !=
            std::string::npos,
        "JSON should report that every required check passed");
  check(output.str().find("\"state\":\"blocked\"") ==
            std::string::npos &&
            output.str().find("\"state\":\"unknown\"") ==
                std::string::npos,
        "All passed report must contain no blocked or unknown finding");
  check(output.str().find("\"executionEnabled\":false") !=
            std::string::npos,
        "Passing read-only checks must not connect physical restore");
  check(errors.str().empty(), "Successful safety inspection should be quiet");
}

void test_restore_execution_requires_injected_service_before_access() {
  MockInventoryProvider provider(valid_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_restore_job_bytes();
  MockRestoreImageVerifier verifier;
  verifier.report = valid_restore_image_report();
  MockRestoreSafetyProbe safety_probe;
  safety_probe.observation = all_restore_safety_checks_passed();
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-execute", L"--job-path",
       L"E:\\Tsumugi\\restore-job.json",
       L"--acknowledge-target-erasure", L"--confirmation",
       valid_restore_confirmation_token()},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      &verifier,
      &safety_probe);

  check(exit_code == 64,
        "Restore execution without an injected service must be unsupported");
  check(loader.call_count == 0 && verifier.call_count == 0 &&
            safety_probe.call_count == 0 && provider.call_count == 0,
        "Missing restore service must stop before all file and disk access");
  check(output.str().empty(),
        "Missing restore service must not emit an execution report");
  check(errors.str().find("ジョブ実行サービスが無効") !=
            std::string::npos,
        "Missing job execution services should be explained explicitly");
}

void test_restore_execution_requires_exact_fresh_confirmation() {
  MockInventoryProvider provider(valid_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_restore_job_bytes();
  MockRestoreImageVerifier verifier;
  verifier.report = valid_restore_image_report();
  MockRestoreSafetyProbe safety_probe;
  safety_probe.observation = all_restore_safety_checks_passed();
  MockRestoreExecutionService service;
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-execute", L"--job-path",
       L"E:\\Tsumugi\\restore-job.json",
       L"--acknowledge-target-erasure", L"--confirmation", L"WRONG"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      &verifier,
      &safety_probe,
      nullptr,
      nullptr,
      &service);

  check(exit_code == 1, "Wrong restore confirmation must fail closed");
  check(loader.call_count == 1 && verifier.call_count == 1 &&
            safety_probe.call_count == 1 && provider.call_count == 1,
        "Confirmation must be checked against the same fresh preflight");
  check(service.call_count == 0,
        "Wrong confirmation must not reach the restore service");
  check(output.str().empty(),
        "Wrong confirmation must not emit a PASS report");
  check(errors.str().find("確認語が一致しません") !=
            std::string::npos,
        "Wrong confirmation should preserve the safety diagnostic");
}

void test_restore_execution_blocks_unknown_or_dangerous_safety_gate() {
  MockInventoryProvider provider(valid_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_restore_job_bytes();
  MockRestoreImageVerifier verifier;
  verifier.report = valid_restore_image_report();
  MockRestoreSafetyProbe safety_probe;
  safety_probe.observation = all_restore_safety_checks_passed();
  safety_probe.observation.storage_spaces_absent =
      ytec::winpeapp::RestoreSafetyState::blocked;
  MockRestoreExecutionService service;
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-execute", L"--job-path",
       L"E:\\Tsumugi\\restore-job.json",
       L"--acknowledge-target-erasure", L"--confirmation",
       valid_restore_confirmation_token()},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      &verifier,
      &safety_probe,
      nullptr,
      nullptr,
      &service);

  check(exit_code == 1, "A blocked required safety gate must stop restore");
  check(service.call_count == 0,
        "A blocked gate must stop before the restore service");
  check(output.str().empty(),
        "A blocked gate must not emit a PASS report");
  check(errors.str().find("必須安全検査") != std::string::npos,
        "Blocked readiness should explain why execution was denied");
}

void test_restore_execution_passes_verified_contract_and_progress() {
  MockInventoryProvider provider(valid_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_restore_job_bytes();
  MockRestoreImageVerifier verifier;
  verifier.report = valid_restore_image_report();
  MockRestoreSafetyProbe safety_probe;
  safety_probe.observation = all_restore_safety_checks_passed();
  MockRestoreExecutionService service;
  int progress_count = 0;
  std::uint64_t reported_written_bytes = 0;
  const ytec::clonecore::DiskOperationCallbacks callbacks{
      .progress =
          [&](const ytec::clonecore::DiskOperationProgress& progress) {
            ++progress_count;
            reported_written_bytes = progress.written_bytes;
          },
  };
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-execute", L"--job-path",
       L"E:\\Tsumugi\\restore-job.json",
       L"--acknowledge-target-erasure", L"--confirmation",
       valid_restore_confirmation_token(), L"--json"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      &verifier,
      &safety_probe,
      nullptr,
      &callbacks,
      &service);

  check(exit_code == 0, "Verified mock restore execution should succeed");
  check(service.call_count == 1,
        "Verified restore should reach the service exactly once");
  check(service.last_request.expected_target.disk_number == 1 &&
            service.last_request.expected_target.serial_suffix == "TARGET01",
        "Restore service must receive the freshly resolved stable target");
  check(service.last_request.expected_image.length_bytes == 4096 &&
            service.last_request.expected_image.global_hash ==
                restore_image_hash(),
        "Restore service must receive the job-recorded image fingerprint");
  check(service.last_request.verified_image_path ==
            L"E:\\Tsumugi\\system.dcimg",
        "Restore service must receive the verifier's canonical image path");
  check(service.last_request.confirmation.first_step_acknowledged &&
            service.last_request.confirmation.typed_token ==
                valid_restore_confirmation_token(),
        "Restore service must receive the exact two-step confirmation");
  check(progress_count == 1 &&
            reported_written_bytes == service.report.restored_data_bytes,
        "Restore progress callbacks must cross the coordinator boundary");
  check(output.str().find("\"mode\":\"restore-execution\"") !=
            std::string::npos &&
            output.str().find("\"result\":\"PASS\"") !=
                std::string::npos &&
            output.str().find("\"readBackVerified\":true") !=
                std::string::npos,
        "Successful JSON must report the verified restore result");
  check(output.str().find("E:\\") == std::string::npos,
        "Execution output must not disclose the image path");
  check(errors.str().empty(), "Successful mock restore should be quiet");
}

void test_restore_execution_rejects_incomplete_service_report() {
  MockInventoryProvider provider(valid_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_restore_job_bytes();
  MockRestoreImageVerifier verifier;
  verifier.report = valid_restore_image_report();
  MockRestoreSafetyProbe safety_probe;
  safety_probe.observation = all_restore_safety_checks_passed();
  MockRestoreExecutionService service;
  service.report.read_back_verified = false;
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-execute", L"--job-path",
       L"E:\\Tsumugi\\restore-job.json",
       L"--acknowledge-target-erasure", L"--confirmation",
       valid_restore_confirmation_token()},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      &verifier,
      &safety_probe,
      nullptr,
      nullptr,
      &service);

  check(exit_code == 1,
        "Incomplete restore report must not be treated as success");
  check(service.call_count == 1,
        "Coordinator should validate the single service result");
  check(output.str().empty(),
        "Incomplete service report must not emit PASS");
  check(errors.str().find("読戻し検証") != std::string::npos,
        "Incomplete report should explain the missing verification");
}

void test_restore_safety_probe_failure_stops_without_ready_report() {
  MockInventoryProvider provider(valid_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_restore_job_bytes();
  MockRestoreImageVerifier verifier;
  verifier.report = valid_restore_image_report();
  MockRestoreSafetyProbe safety_probe;
  safety_probe.error = ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::query_failed,
      .native_code = ERROR_ACCESS_DENIED,
      .operation = L"復元実行直前の読取り専用検査",
      .message = L"検査状態を確定できません",
  };
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-preflight", L"--job-path",
       L"E:\\Tsumugi\\restore-job.json", L"--json"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      &verifier,
      &safety_probe);

  check(exit_code == 1, "Safety probe I/O failure must fail closed");
  check(safety_probe.call_count == 1,
        "Failing safety probe must not be retried implicitly");
  check(output.str().empty(),
        "Safety probe failure must not emit a ready report");
  check(errors.str().find("検査状態を確定できません") !=
            std::string::npos,
        "Probe failure should preserve its diagnostic");
}

void test_restore_job_accepts_reselected_identical_image() {
  MockInventoryProvider provider(valid_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_restore_job_bytes();
  MockRestoreImageVerifier verifier;
  verifier.report = valid_restore_image_report();
  verifier.report.canonical_path = L"F:\\Recovered\\system.dcimg";
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-preflight", L"--job-path",
       L"E:\\Tsumugi\\restore-job.json", L"--image-path",
       L"F:\\Recovered\\system.dcimg", L"--json"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      &verifier);

  check(exit_code == 0,
        "Reselected image with the recorded fingerprint should pass");
  check(verifier.received_path == L"F:\\Recovered\\system.dcimg",
        "Verifier must receive the explicitly reselected image path");
  check(provider.call_count == 1,
        "Matching reselected image should proceed to disk enumeration");
  check(output.str().find("\"imageVerified\":true") !=
            std::string::npos,
        "Matching reselected image should be reported as verified");
  check(errors.str().empty(),
        "Matching reselected image should not use stderr");
}

void test_restore_candidate_paths_change_only_drive_letter() {
  const std::uint32_t mask =
      (1U << 2U) | (1U << 4U) | (1U << 23U) | (1U << 25U);
  const auto candidates =
      ytec::winpeapp::build_restore_image_candidate_paths(
          L"E:\\Backups\\Tsumugi\\system.dcimg", mask);
  check(candidates.size() == 3,
        "Candidate builder should include C, E, and Z but skip WinPE X");
  check(candidates[0] == L"C:\\Backups\\Tsumugi\\system.dcimg" &&
            candidates[1] == L"E:\\Backups\\Tsumugi\\system.dcimg" &&
            candidates[2] == L"Z:\\Backups\\Tsumugi\\system.dcimg",
        "Candidate builder must preserve the exact relative path");
  check(
      ytec::winpeapp::build_restore_image_candidate_paths(
          L"\\\\server\\share\\system.dcimg", mask)
          .empty(),
      "Candidate builder must reject a non drive-letter source path");
}

void test_restore_job_auto_locates_matching_image_before_disk_scan() {
  MockInventoryProvider provider(valid_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_restore_job_bytes();
  MockRestoreImageCandidateProvider candidates;
  candidates.candidates = {
      L"D:\\Tsumugi\\system.dcimg",
      L"F:\\Tsumugi\\system.dcimg",
  };
  SequencedRestoreImageVerifier verifier;
  auto different = valid_restore_image_report();
  different.global_hash[0] = std::byte{0x7F};
  verifier.reports = {different, valid_restore_image_report()};
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-preflight", L"--job-path",
       L"E:\\Tsumugi\\restore-job.json", L"--search-image", L"--json"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      &verifier,
      nullptr,
      &candidates);

  check(exit_code == 0,
        "Automatic resolution should accept the first exact image identity");
  check(candidates.call_count == 1 &&
            candidates.received_path ==
                L"E:\\Tsumugi\\system.dcimg",
        "Candidate provider must receive the configured path exactly once");
  check(verifier.call_count == 2,
        "Each candidate should be fully verified until an exact match");
  check(provider.call_count == 1,
        "Disk enumeration must start only after an image match");
  check(output.str().find("\"imageAutoLocated\":true") !=
            std::string::npos,
        "JSON should identify automatic image resolution");
  check(output.str().find("D:\\") == std::string::npos &&
            output.str().find("F:\\") == std::string::npos,
        "Preflight output must not disclose resolved image paths");
  check(output.str().find("\"executionEnabled\":false") !=
            std::string::npos,
        "Automatic resolution must not enable physical restore");
  check(errors.str().empty(), "Successful automatic resolution is quiet");
}

void test_restore_job_auto_search_no_match_stops_before_disk_scan() {
  MockInventoryProvider provider(valid_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_restore_job_bytes();
  MockRestoreImageCandidateProvider candidates;
  candidates.candidates = {L"F:\\Tsumugi\\system.dcimg"};
  SequencedRestoreImageVerifier verifier;
  auto different = valid_restore_image_report();
  different.image_length = 8192;
  verifier.reports = {different};
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-preflight", L"--job-path",
       L"E:\\Tsumugi\\restore-job.json", L"--search-image"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      &verifier,
      nullptr,
      &candidates);

  check(exit_code == 1, "No exact image identity must fail closed");
  check(verifier.call_count == 1,
        "The available candidate should be fully verified once");
  check(provider.call_count == 0,
        "No-match search must stop before disk enumeration");
  check(output.str().empty(),
        "No-match search must not emit a ready report");
  check(errors.str().find("一致するdcimgが見つかりません") !=
            std::string::npos,
        "No-match failure should explain the identity requirement");
}

void test_restore_job_auto_search_rejects_excess_candidates() {
  MockInventoryProvider provider(valid_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_restore_job_bytes();
  MockRestoreImageCandidateProvider candidates;
  for (std::size_t index = 0; index < 65; ++index) {
    candidates.candidates.push_back(
        L"F:\\Tsumugi\\candidate-" + std::to_wstring(index) +
        L".dcimg");
  }
  SequencedRestoreImageVerifier verifier;
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-preflight", L"--job-path",
       L"E:\\Tsumugi\\restore-job.json", L"--search-image"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      &verifier,
      nullptr,
      &candidates);

  check(exit_code == 1, "Excess candidate count must fail closed");
  check(verifier.call_count == 0,
        "Candidate bound must be checked before opening any image");
  check(provider.call_count == 0,
        "Excess candidates must stop before disk enumeration");
  check(errors.str().find("安全上限") != std::string::npos,
        "Candidate-bound failure should be explicit");
}

void test_restore_job_rejects_ambiguous_image_selection_arguments() {
  MockInventoryProvider provider(valid_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_restore_job_bytes();
  MockRestoreImageVerifier verifier;
  MockRestoreImageCandidateProvider candidates;
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-preflight", L"--job-path",
       L"E:\\Tsumugi\\restore-job.json", L"--image-path",
       L"F:\\Tsumugi\\system.dcimg", L"--search-image"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      &verifier,
      nullptr,
      &candidates);

  check(exit_code == 64,
        "Manual and automatic image selection must be mutually exclusive");
  check(loader.call_count == 0 && verifier.call_count == 0 &&
            candidates.call_count == 0 && provider.call_count == 0,
        "Ambiguous arguments must stop before all file and disk access");
  check(output.str().empty(),
        "Ambiguous arguments must not emit a preflight report");
}

void test_restore_job_rejects_reselected_different_image_before_disk_scan() {
  MockInventoryProvider provider(valid_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_restore_job_bytes();
  MockRestoreImageVerifier verifier;
  verifier.report = valid_restore_image_report();
  verifier.report.canonical_path = L"F:\\Recovered\\system.dcimg";
  verifier.report.global_hash[1] = std::byte{0x01};
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-preflight", L"--job-path",
       L"E:\\Tsumugi\\restore-job.json", L"--image-path",
       L"F:\\Recovered\\system.dcimg"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      &verifier);

  check(exit_code == 1,
        "Reselected image with a different fingerprint must fail closed");
  check(verifier.call_count == 1,
        "Different reselected image should be verified exactly once");
  check(provider.call_count == 0,
        "Image identity mismatch must stop before disk enumeration");
  check(output.str().empty(),
        "Image identity mismatch must not emit a ready report");
  check(errors.str().find(
            "ジョブ作成時に検証したイメージと一致しません") !=
            std::string::npos,
        "Image identity mismatch should preserve the safety diagnostic");
}

void test_restore_image_failure_stops_before_disk_scan() {
  MockInventoryProvider provider(valid_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_restore_job_bytes();
  MockRestoreImageVerifier verifier;
  verifier.error = ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::verification_failed,
      .native_code = ERROR_CRC,
      .operation = L"モックdcimg完全検証",
      .message = L"イメージ全体ハッシュが一致しません",
  };
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-preflight", L"--job-path",
       L"E:\\Tsumugi\\restore-job.json"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      &verifier);

  check(exit_code == 1, "Invalid restore image must fail closed");
  check(verifier.call_count == 1,
        "Invalid image should be inspected exactly once");
  check(provider.call_count == 0,
        "Image verification failure must stop before disk enumeration");
  check(output.str().empty(),
        "Invalid image must not emit a ready report");
  check(errors.str().find("イメージ全体ハッシュが一致しません") !=
            std::string::npos,
        "Failure should preserve the verifier diagnostic");
}

void test_restore_job_requires_image_verifier_before_disk_scan() {
  MockInventoryProvider provider(valid_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_restore_job_bytes();
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-preflight", L"--job-path",
       L"E:\\Tsumugi\\restore-job.json"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader);

  check(exit_code == 1,
        "Restore job without image verifier must fail closed");
  check(provider.call_count == 0,
        "Missing verifier must stop before disk enumeration");
  check(output.str().empty(),
        "Missing verifier must not emit a ready report");
  check(errors.str().find("完全検証サービスが無効") !=
            std::string::npos,
        "Failure should explain the missing image verifier");
}

void test_restore_job_rejects_target_smaller_than_image_source() {
  auto report = valid_report();
  report.disks[1].size_bytes = 1ULL * 1024U * 1024U * 1024U;
  report.disks[1].sector_count =
      report.disks[1].size_bytes / report.disks[1].logical_sector_size;
  MockInventoryProvider provider(report);
  MockJobManifestLoader loader;
  loader.bytes = restore_job_bytes_for_report(report);
  MockRestoreImageVerifier verifier;
  verifier.report = valid_restore_image_report();
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-preflight", L"--job-path",
       L"E:\\Tsumugi\\restore-job.json"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      &verifier);

  check(exit_code == 1,
        "Restore target smaller than the image source must fail closed");
  check(verifier.call_count == 1,
        "Capacity rejection must still verify the image exactly once");
  check(provider.call_count == 1,
        "Capacity rejection requires exactly one disk enumeration");
  check(output.str().empty(),
        "Rejected capacity must not emit a ready report");
  check(errors.str().find("復元先ディスクがイメージ元ディスクより小さい") !=
            std::string::npos,
        "Capacity rejection should preserve the safety diagnostic");
}

void test_restore_job_rejects_logical_sector_mismatch() {
  const auto report = valid_report();
  MockInventoryProvider provider(report);
  MockJobManifestLoader loader;
  loader.bytes = restore_job_bytes_for_report(report);
  MockRestoreImageVerifier verifier;
  verifier.report = valid_restore_image_report();
  verifier.report.header.logical_sector_size = 4096;
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-preflight", L"--job-path",
       L"E:\\Tsumugi\\restore-job.json"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      &verifier);

  check(exit_code == 1,
        "Restore target with a different logical sector size must fail closed");
  check(verifier.call_count == 1,
        "Sector-size rejection must still verify the image exactly once");
  check(provider.call_count == 1,
        "Sector-size rejection requires exactly one disk enumeration");
  check(output.str().empty(),
        "Rejected sector size must not emit a ready report");
  check(errors.str().find("論理セクターサイズが一致しません") !=
            std::string::npos,
        "Sector-size rejection should preserve the safety diagnostic");
}

void test_restore_job_rejects_original_source_as_target() {
  const auto report = valid_report();
  MockInventoryProvider provider(report);
  MockJobManifestLoader loader;
  loader.bytes = restore_job_bytes_for_report(report);
  MockRestoreImageVerifier verifier;
  verifier.report = valid_restore_image_report();
  const auto target_identity =
      ytec::diskmodel::make_stable_disk_identity(
          report.disks[1], report.disks[1].is_system_disk);
  check(target_identity.has_value(),
        "Original-source rejection fixture identity should be valid");
  verifier.report.manifest.source = target_identity.value();
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-preflight", L"--job-path",
       L"E:\\Tsumugi\\restore-job.json"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader,
      &verifier);

  check(exit_code == 1,
        "Original image source disk must not be accepted as its restore target");
  check(verifier.call_count == 1,
        "Same-device rejection must verify the image exactly once");
  check(provider.call_count == 1,
        "Same-device rejection requires exactly one disk enumeration");
  check(output.str().empty(),
        "Same-device rejection must not emit a ready report");
  check(errors.str().find(
            "イメージに記録された元ディスクと同じディスク") !=
            std::string::npos,
        "Same-device rejection should preserve the safety diagnostic");
}

void test_tampered_job_stops_before_disk_enumeration() {
  MockInventoryProvider provider(valid_report());
  MockJobManifestLoader loader;
  loader.bytes = valid_clone_job_bytes();
  check(loader.bytes.size() > 3, "Job fixture must contain a hash");
  std::byte& last_hash_digit = loader.bytes[loader.bytes.size() - 3];
  last_hash_digit =
      last_hash_digit == std::byte{'A'} ? std::byte{'B'} : std::byte{'A'};
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-preflight", L"--job-path", L"E:\\Tsumugi\\job.json"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader);

  check(exit_code == 1, "Tampered job must fail");
  check(provider.call_count == 0,
        "Hash failure must stop before disk enumeration");
  check(output.str().empty(), "Tampered job must not emit a ready report");
  check(
      errors.str().find("変更されています") != std::string::npos,
      "Failure should explain the job integrity mismatch");
}

void test_job_identity_change_fails_closed() {
  auto report = valid_report();
  report.disks[1].serial_suffix = "CHANGED1";
  MockInventoryProvider provider(std::move(report));
  MockJobManifestLoader loader;
  loader.bytes = valid_clone_job_bytes();
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-preflight", L"--job-path", L"E:\\Tsumugi\\job.json"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      &loader);

  check(exit_code == 1, "Changed target identity must fail");
  check(provider.call_count == 1, "Identity failure follows one fresh scan");
  check(
      errors.str().find("一致するディスクがありません") !=
          std::string::npos,
      "Failure should explain the missing stable identity");
}

void test_job_preflight_requires_loader_before_enumeration() {
  MockInventoryProvider provider(valid_report());
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-preflight", L"--job-path", L"E:\\Tsumugi\\job.json"},
      provider,
      output,
      errors);

  check(exit_code == 64, "Missing job loader must be unsupported");
  check(provider.call_count == 0,
        "Missing loader must stop before disk enumeration");
  check(
      errors.str().find("ジョブ読取りサービスが無効") !=
          std::string::npos,
      "Failure should explain the missing job loader");
}

void test_text_preflight_reports_confirmation_without_execution() {
  MockInventoryProvider provider(valid_report());
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--clone-preflight", L"--source", L"0", L"--target", L"1"},
      provider,
      output,
      errors);

  check(exit_code == 0, "Valid preflight should succeed");
  check(provider.call_count == 1, "Preflight should enumerate exactly once");
  check(output.str().find("ERASE VBOX HARDDISK TARGET01 3221225472") !=
            std::string::npos,
        "Preflight should emit the target-specific confirmation token");
  check(output.str().find("クローン実行: 無効") != std::string::npos,
        "Preflight must not expose execution");
  check(errors.str().empty(), "Successful preflight should not use stderr");
}

void test_json_preflight_is_machine_readable() {
  MockInventoryProvider provider(valid_report());
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--clone-preflight", L"--source", L"0", L"--target", L"1",
       L"--json"},
      provider,
      output,
      errors);

  check(exit_code == 0, "JSON preflight should succeed");
  check(output.str().find("\"mode\":\"clone-preflight\"") !=
            std::string::npos,
        "JSON should identify its mode");
  check(output.str().find("\"executionEnabled\":false") !=
            std::string::npos,
        "JSON must declare execution disabled");
  check(output.str().find("\"targetPartitionsToErase\":0") !=
            std::string::npos,
        "JSON should report the destructive target surface");
}

void test_non_raw_target_fails_closed() {
  auto report = valid_report();
  report.disks[1].partition_style = ytec::diskmodel::PartitionStyle::gpt;
  report.disks[1].partitions.push_back(
      ytec::diskmodel::PartitionInfo{.number = 1});
  MockInventoryProvider provider(std::move(report));
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--clone-preflight", L"--source", L"0", L"--target", L"1"},
      provider,
      output,
      errors);

  check(exit_code == 1, "Non-RAW target must fail");
  check(output.str().empty(), "Failed preflight must not emit a ready plan");
  check(errors.str().find("RAW") != std::string::npos,
        "Failure should explain the RAW-only gate");
}

void test_inventory_issues_fail_closed() {
  auto report = valid_report();
  report.issues.push_back(ytec::diskmodel::InventoryIssue{
      .device = L"mock",
      .error = ytec::clonecore::Error{
          .code = ytec::clonecore::ErrorCode::query_failed,
          .native_code = ERROR_ACCESS_DENIED,
          .operation = L"mock",
          .message = L"mock",
      },
  });
  MockInventoryProvider provider(std::move(report));
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--clone-preflight", L"--source", L"0", L"--target", L"1"},
      provider,
      output,
      errors);

  check(exit_code == 1, "Inventory issues must stop preflight");
  check(errors.str().find("未解決") != std::string::npos,
        "Failure should explain unresolved inventory diagnostics");
}

void test_execute_argument_is_rejected_without_enumeration() {
  MockInventoryProvider provider(valid_report());
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--execute", L"--source", L"0", L"--target", L"1"},
      provider,
      output,
      errors);

  check(exit_code == 64, "Execution argument must be rejected");
  check(provider.call_count == 0,
        "Rejected execution must not even enumerate disks");
}

void test_clone_execute_is_rejected_when_service_is_not_injected() {
  MockInventoryProvider provider(valid_report());
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--clone-execute", L"--source", L"0", L"--target", L"1",
       L"--acknowledge-target-erasure", L"--confirmation",
       L"ERASE VBOX HARDDISK TARGET01 3221225472", L"--authorization",
       L"VM-ONLY"},
      provider,
      output,
      errors);

  check(exit_code == 64, "Product build must reject clone execution");
  check(provider.call_count == 0,
        "Disabled clone execution must be rejected before enumeration");
  check(errors.str().find("実行サービスが無効") != std::string::npos,
        "Failure should explain that no execution service is available");
}

void test_injected_execution_requires_exact_confirmation() {
  MockInventoryProvider provider(valid_report());
  MockExecutionService service;
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--clone-execute", L"--source", L"0", L"--target", L"1",
       L"--acknowledge-target-erasure", L"--confirmation", L"WRONG",
       L"--authorization", L"VM-ONLY"},
      provider,
      output,
      errors,
      &service);

  check(exit_code == 1, "Wrong confirmation must fail");
  check(provider.call_count == 1,
        "Execution confirmation should be checked against fresh inventory");
  check(service.call_count == 0,
        "Wrong confirmation must not reach the execution service");
}

void test_injected_execution_receives_stable_identities() {
  MockInventoryProvider provider(valid_report());
  MockExecutionService service;
  std::ostringstream output;
  std::ostringstream errors;
  std::uint32_t progress_event_count = 0;
  ytec::clonecore::DiskOperationCallbacks callbacks;
  callbacks.progress =
      [&](const ytec::clonecore::DiskOperationProgress& progress) {
        ++progress_event_count;
        check(
            progress.verified_bytes == service.report.copied_data_bytes,
            "Execution progress should preserve engine counters");
      };
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--clone-execute", L"--source", L"0", L"--target", L"1",
       L"--acknowledge-target-erasure", L"--confirmation",
       L"ERASE VBOX HARDDISK TARGET01 3221225472", L"--authorization",
       L"VM-ONLY", L"--json"},
      provider,
      output,
      errors,
      &service,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &callbacks);

  check(exit_code == 0, "Injected VM execution should succeed");
  check(service.call_count == 1, "Execution service should be called once");
  check(service.last_request.expected_source.serial_suffix == "SOURCE01",
        "Execution service must receive the stable source identity");
  check(service.last_request.expected_target.serial_suffix == "TARGET01",
        "Execution service must receive the stable target identity");
  check(service.last_request.confirmation.first_step_acknowledged,
        "First erasure acknowledgement must be preserved");
  check(progress_event_count == 1,
        "Runner callbacks should reach the injected execution service");
  check(output.str().find("\"mode\":\"clone-execution\"") !=
            std::string::npos,
         "Execution JSON should identify its mode");
  check(output.str().find("\"partitionStyle\":\"GPT\"") !=
            std::string::npos,
        "Execution JSON should identify the cloned partition style");
  check(output.str().find("\"partitionTableCommitted\":true") !=
            std::string::npos,
        "Execution JSON should report partition-table commit state");
  check(output.str().find("\"targetReturnedOnline\":true") !=
            std::string::npos,
        "Execution JSON should report target recovery state");
  check(errors.str().empty(), "Successful execution should not use stderr");
}

void test_boot_repair_preflight_is_read_only_and_machine_readable() {
  MockInventoryProvider provider(valid_report());
  MockBootRepairService service;
  service.selection = boot_repair_selection();
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--boot-repair-preflight", L"--disk", L"0",
       L"--windows-root", L"W:\\", L"--system-root", L"S:\\",
       L"--firmware", L"uefi", L"--json"},
      provider,
      output,
      errors,
      nullptr,
      &service);

  check(exit_code == 0, "Valid boot repair preflight should succeed");
  check(provider.call_count == 0,
        "Boot repair service owns its fresh inventory pass");
  check(service.inspect_count == 1 && service.execute_count == 0,
        "Preflight must inspect once without executing BCDBoot");
  check(output.str().find("\"mode\":\"boot-repair-preflight\"") !=
            std::string::npos,
        "Preflight JSON should identify its mode");
  check(output.str().find("\"readOnly\":true") != std::string::npos,
        "Preflight JSON must declare read-only behavior");
  check(output.str().find("REPAIR BOOT UEFI VBOX HARDDISK SOURCE01") !=
            std::string::npos,
        "Preflight must emit a target-specific repair confirmation");
  check(errors.str().empty(), "Successful preflight should not use stderr");
}

void test_boot_repair_preflight_accepts_unassigned_system_partition_mode() {
  MockInventoryProvider provider(valid_report());
  MockBootRepairService service;
  service.selection = boot_repair_selection();
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--boot-repair-preflight", L"--disk", L"0",
       L"--windows-root", L"W:\\", L"--auto-system-partition",
       L"--firmware", L"uefi", L"--json"},
      provider,
      output,
      errors,
      nullptr,
      &service);

  check(exit_code == 0, "Auto system partition preflight should succeed");
  check(
      service.inspect_count == 1 &&
          service.last_inspect_request.system_root.empty() &&
          service.last_inspect_request.auto_mount_system_partition,
      "Auto mode must reach inspection without a fabricated drive root");
  check(output.str().find("\"systemRootMode\":\"auto-unassigned\"") !=
            std::string::npos,
        "JSON must distinguish an unassigned read-only system selection");
  check(errors.str().empty(), "Successful auto preflight should not use stderr");

  std::ostringstream conflicting_output;
  std::ostringstream conflicting_errors;
  const int conflicting_exit = ytec::winpeapp::run_winpe_app(
      {L"--boot-repair-preflight", L"--disk", L"0",
       L"--windows-root", L"W:\\", L"--system-root", L"S:\\",
       L"--auto-system-partition", L"--firmware", L"uefi"},
      provider,
      conflicting_output,
      conflicting_errors,
      nullptr,
      &service);
  check(conflicting_exit == 64 && service.inspect_count == 1,
        "Manual and automatic system roots together must stop before inspection");
}

void test_boot_repair_is_rejected_when_service_is_not_injected() {
  MockInventoryProvider provider(valid_report());
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--boot-repair-preflight", L"--disk", L"0",
       L"--windows-root", L"W:\\", L"--system-root", L"S:\\",
       L"--firmware", L"uefi"},
      provider,
      output,
      errors);

  check(exit_code == 64, "Missing repair service must fail as unsupported");
  check(provider.call_count == 0,
        "Disabled repair must stop before any disk enumeration");
  check(errors.str().find("単独起動修復サービスが無効") !=
            std::string::npos,
        "Failure should explain the disabled repair boundary");
}

void test_boot_repair_execution_requires_exact_confirmation() {
  MockInventoryProvider provider(valid_report());
  MockBootRepairService service;
  service.selection = boot_repair_selection();
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--boot-repair-execute", L"--disk", L"0",
       L"--windows-root", L"W:\\", L"--system-root", L"S:\\",
       L"--firmware", L"uefi", L"--acknowledge-boot-files-change",
       L"--confirmation", L"WRONG"},
      provider,
      output,
      errors,
      nullptr,
      &service);

  check(exit_code == 1, "Wrong boot repair confirmation must fail");
  check(service.inspect_count == 1 && service.execute_count == 0,
        "Wrong confirmation must not reach the execution service");
}

void test_boot_repair_execution_reports_verified_bcd_store() {
  MockInventoryProvider provider(valid_report());
  MockBootRepairService service;
  service.selection = boot_repair_selection();
  const std::wstring token =
      ytec::bootrepair::make_boot_repair_confirmation_token(
          service.selection.identity,
          ytec::bootrepair::BcdBootFirmware::uefi);
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--boot-repair-execute", L"--disk", L"0",
       L"--windows-root", L"W:\\", L"--system-root", L"S:\\",
       L"--firmware", L"uefi", L"--acknowledge-boot-files-change",
       L"--confirmation", token, L"--json"},
      provider,
      output,
      errors,
      nullptr,
      &service);

  check(exit_code == 0, "Confirmed boot repair should succeed");
  check(service.inspect_count == 1 && service.execute_count == 1,
        "Execution should inspect then call the service once");
  check(service.last_execute_request.confirmation.first_step_acknowledged,
        "First confirmation step must reach the service");
  check(output.str().find("\"mode\":\"boot-repair-execution\"") !=
            std::string::npos,
        "Execution JSON should identify its mode");
  check(output.str().find("\"microsoftSignatureVerified\":true") !=
            std::string::npos,
        "Execution JSON must report the Microsoft trust gate");
  check(output.str().find("\"bootStoreVerified\":true") !=
            std::string::npos,
        "Execution JSON must report post-write BCD verification");
  check(errors.str().empty(), "Successful repair should not use stderr");
}

void test_winre_diagnostic_is_read_only_and_machine_readable() {
  MockInventoryProvider provider(valid_report());
  MockWinReDiagnosticService service;
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--winre-diagnostic", L"--disk", L"0",
       L"--windows-root", L"w:/", L"--json"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &service);

  check(exit_code == 0, "Verified WinRE diagnostics should succeed");
  check(provider.call_count == 1,
        "WinRE diagnostics must re-enumerate disks once");
  check(service.call_count == 1,
        "WinRE diagnostic service should be called once");
  check(
      service.received_offline_windows_directory ==
              L"W:\\Windows" &&
          service.received_expected_disk_number == 0U,
      "CLI must pass a normalized offline Windows path and disk number");
  check(
      output.str().find("\"mode\":\"winre-diagnostic\"") !=
              std::string::npos &&
          output.str().find("\"readOnly\":true") !=
              std::string::npos &&
          output.str().find(
              "\"sourceState\":\"registered-partition\"") !=
              std::string::npos &&
          output.str().find("\"executionEnabled\":false") !=
              std::string::npos,
      "JSON must identify the read-only non-executable diagnostic");
  check(errors.str().empty(),
        "A completed WinRE diagnostic should not use stderr");
}

void test_winre_diagnostic_unknown_fails_closed_after_reporting() {
  MockInventoryProvider provider(valid_report());
  MockWinReDiagnosticService service;
  service.report.exit_code = 5U;
  service.report.source_state =
      ytec::bootrepair::WinReSourceState::unknown;
  service.report.registered_partition_number = 0U;
  service.report.registered_location_reported = false;
  service.report.registered_location_matches_expected_disk = false;
  service.report.registered_image_present = false;
  service.report.winre_image_size_bytes = 0U;
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--winre-diagnostic", L"--disk", L"0",
       L"--windows-root", L"W:\\", L"--json"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &service);

  check(exit_code == 1,
        "Unknown WinRE diagnostics must return failure");
  check(
      output.str().find("\"diagnosticComplete\":false") !=
              std::string::npos &&
          output.str().find("\"sourceState\":\"unknown\"") !=
              std::string::npos,
      "Unknown facts should remain visible in machine-readable output");
  check(
      errors.str().find("再構築計画へ進めません") !=
          std::string::npos,
      "Unknown diagnostics must explain the fail-closed result");
}

void test_winre_diagnostic_rejects_invalid_input_before_access() {
  MockInventoryProvider provider(valid_report());
  MockWinReDiagnosticService service;
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--winre-diagnostic", L"--disk", L"0",
       L"--windows-root", L"W:\\Windows\\..\\"},
      provider,
      output,
      errors,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &service);

  check(exit_code == 64,
        "Invalid WinRE paths must be rejected as arguments");
  check(provider.call_count == 0 && service.call_count == 0,
        "Invalid input must stop before disk or REAgentC access");
  check(output.str().empty(),
        "Invalid input should not produce a diagnostic report");
}

void test_winre_diagnostic_requires_injected_service() {
  MockInventoryProvider provider(valid_report());
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--winre-diagnostic", L"--disk", L"0",
       L"--windows-root", L"W:\\"},
      provider,
      output,
      errors);

  check(exit_code == 64,
        "A missing WinRE diagnostic service must be unsupported");
  check(provider.call_count == 0,
        "A disabled WinRE diagnostic must not enumerate disks");
  check(
      errors.str().find("WinRE読み取り専用診断サービスが無効") !=
          std::string::npos,
      "Disabled diagnostics should explain the product boundary");
}

void test_winre_view_accepts_verified_registered_partition() {
  ytec::bootrepair::WinReDiagnosticReport report{
      .exit_code = 0U,
      .source_state =
          ytec::bootrepair::WinReSourceState::registered_partition,
      .registered_partition_number = 4U,
      .winre_image_size_bytes = 768ULL * 1024ULL * 1024ULL,
      .microsoft_signature_verified = true,
      .read_only_command = true,
      .registered_location_reported = true,
      .registered_location_matches_expected_disk = true,
      .registered_image_present = true,
  };

  const auto view =
      ytec::winpeapp::build_winre_diagnostic_view(report);

  check(view.conclusive && view.recovery_source_available,
        "A fully matched registered WinRE image should be reusable evidence");
  check(
      view.details.find(L"パーティション 4") != std::wstring::npos &&
          view.details.find(L"変更なし") != std::wstring::npos,
      "The registered WinRE view should identify the partition and read-only boundary");
}

void test_winre_view_accepts_verified_windows_fallback_for_rebuild() {
  ytec::bootrepair::WinReDiagnosticReport report{
      .exit_code = 0U,
      .source_state =
          ytec::bootrepair::WinReSourceState::image_available_in_windows,
      .winre_image_size_bytes = 640ULL * 1024ULL * 1024ULL,
      .microsoft_signature_verified = true,
      .read_only_command = true,
      .fallback_image_present = true,
  };

  const auto view =
      ytec::winpeapp::build_winre_diagnostic_view(report);

  check(view.conclusive && view.recovery_source_available,
        "A verified Windows fallback image should support recovery partition creation");
  check(
      view.headline.find(L"補完") != std::wstring::npos &&
          view.details.find(L"書込みを開始しません") != std::wstring::npos,
      "The fallback view should explain both reconstruction use and no-write behavior");
}

void test_winre_view_fails_closed_on_incomplete_evidence() {
  ytec::bootrepair::WinReDiagnosticReport report{
      .exit_code = 0U,
      .source_state =
          ytec::bootrepair::WinReSourceState::registered_partition,
      .registered_partition_number = 4U,
      .winre_image_size_bytes = 768ULL * 1024ULL * 1024ULL,
      .microsoft_signature_verified = true,
      .read_only_command = true,
      .registered_location_reported = true,
      .registered_location_matches_expected_disk = false,
      .registered_image_present = true,
  };

  const auto view =
      ytec::winpeapp::build_winre_diagnostic_view(report);

  check(!view.conclusive && !view.recovery_source_available,
        "A registered WinRE location on another disk must fail closed");
  check(
      view.details.find(L"停止") != std::wstring::npos,
      "Incomplete WinRE evidence should explicitly stop MBR-to-GPT planning");
}

void test_winpe_repair_actions_never_overlap_at_supported_widths() {
  for (const int width : {1024, 1040, 1280, 1600}) {
    const auto layout =
        ytec::winpeapp::build_winpe_repair_action_layout(width);
    check(
        layout.note.width() >= 300 && layout.note.height() == 42,
        "The WinRE explanation should retain a readable text area");
    check(
        layout.winre_diagnostic_button.width() == 164 &&
            layout.boot_inspect_button.width() == 164,
        "Both repair diagnostic actions should keep their designed width");
    check(
        layout.note.right + 28 ==
                layout.winre_diagnostic_button.left &&
            layout.winre_diagnostic_button.right + 14 ==
                layout.boot_inspect_button.left,
        "Repair explanatory text and action buttons must not overlap");
    check(
        layout.boot_inspect_button.right <= width - 44,
        "The rightmost repair action must retain the client margin");
  }
}

void test_product_io_policy_uses_validated_16_mib_boundary() {
  const auto policy = ytec::winpeapp::select_product_io_policy(512U);
  check(policy.has_value(),
        "The current 512-byte logical sector product boundary should have an I/O policy");
  check(
      policy->transfer_chunk_bytes == 16U * 1024U * 1024U &&
          policy->maximum_bytes_between_cancellation_checks ==
              policy->transfer_chunk_bytes,
      "Product transfers should use the validated 16 MiB ceiling without widening cancellation intervals");
  check(
      !ytec::winpeapp::select_product_io_policy(4096U).has_value() &&
          !ytec::winpeapp::select_product_io_policy(0U).has_value(),
      "Performance policy must not silently enable unverified 4Kn or unknown sectors");
}

void test_product_io_policy_reduces_deterministic_request_count() {
  constexpr std::uint64_t kLogicalBytes =
      64ULL * 1024ULL * 1024ULL * 1024ULL;
  const ytec::winpeapp::ProductIoPolicy old_policy{
      .transfer_chunk_bytes = 4U * 1024U * 1024U,
      .maximum_bytes_between_cancellation_checks =
          4U * 1024U * 1024U,
  };
  const auto new_policy =
      ytec::winpeapp::select_product_io_policy(512U).value();
  const auto old_estimate =
      ytec::winpeapp::estimate_product_io_requests(
          kLogicalBytes, old_policy);
  const auto new_estimate =
      ytec::winpeapp::estimate_product_io_requests(
          kLogicalBytes, new_policy);

  check(
      old_estimate.logical_chunk_count == 16384U &&
          new_estimate.logical_chunk_count == 4096U,
      "A 64 GiB logical copy should need 75 percent fewer transfer chunks");
  check(
      new_estimate.source_read_count * 4U ==
              old_estimate.source_read_count &&
          new_estimate.target_write_count * 4U ==
              old_estimate.target_write_count &&
          new_estimate.target_read_back_count * 4U ==
              old_estimate.target_read_back_count,
      "Read, write, and mandatory read-back request counts should all fall by 75 percent");
  const auto partial = ytec::winpeapp::estimate_product_io_requests(
      new_policy.transfer_chunk_bytes + 512U, new_policy);
  check(partial.logical_chunk_count == 2U,
        "A partial final sector-aligned transfer must round up to one final chunk");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"help_is_read_only_and_does_not_enumerate",
       test_help_is_read_only_and_does_not_enumerate},
      {"job_candidate_paths_are_fixed_and_exclude_winpe_ram_drive",
       test_job_candidate_paths_are_fixed_and_exclude_winpe_ram_drive},
      {"job_discovery_accepts_one_verified_candidate",
       test_job_discovery_accepts_one_verified_candidate},
      {"job_discovery_allows_no_candidate_without_loading",
       test_job_discovery_allows_no_candidate_without_loading},
      {"job_discovery_rejects_multiple_verified_candidates",
       test_job_discovery_rejects_multiple_verified_candidates},
      {"job_discovery_rejects_tampered_candidate_alongside_valid_job",
       test_job_discovery_rejects_tampered_candidate_alongside_valid_job},
      {"job_discovery_rejects_case_insensitive_duplicate_paths",
       test_job_discovery_rejects_case_insensitive_duplicate_paths},
      {"hash_locked_job_loader_rejects_post_review_replacement",
       test_hash_locked_job_loader_rejects_post_review_replacement},
      {"job_result_log_is_bounded_and_correlated_to_job",
       test_job_result_log_is_bounded_and_correlated_to_job},
      {"job_result_path_is_timestamped_beside_job",
       test_job_result_path_is_timestamped_beside_job},
      {"job_result_file_is_create_new_and_read_back_verified",
       test_job_result_file_is_create_new_and_read_back_verified},
      {"job_preflight_verifies_hash_and_re_resolves_disks",
       test_job_preflight_verifies_hash_and_re_resolves_disks},
      {"clone_readiness_fails_closed_on_unknown_or_non_raw_layout",
       test_clone_readiness_fails_closed_on_unknown_or_non_raw_layout},
      {"clone_job_execution_passes_verified_contract_and_progress",
       test_clone_job_execution_passes_verified_contract_and_progress},
      {"clone_job_execution_rejects_incomplete_service_report",
       test_clone_job_execution_rejects_incomplete_service_report},
      {"clone_job_execution_rejects_wrong_confirmation_before_service",
       test_clone_job_execution_rejects_wrong_confirmation_before_service},
      {"mbr_to_gpt_preflight_requires_mbr_source_and_raw_target",
       test_mbr_to_gpt_preflight_requires_mbr_source_and_raw_target},
      {"mbr_to_gpt_job_execution_passes_complete_verified_contract",
       test_mbr_to_gpt_job_execution_passes_complete_verified_contract},
      {"mbr_to_gpt_job_execution_rejects_incomplete_report",
       test_mbr_to_gpt_job_execution_rejects_incomplete_report},
      {"restore_job_reverifies_image_before_disk_scan",
       test_restore_job_reverifies_image_before_disk_scan},
      {"restore_readiness_model_fails_closed_on_unknown_or_blocked",
       test_restore_readiness_model_fails_closed_on_unknown_or_blocked},
      {"restore_safety_derivation_rejects_unsupported_disk_types",
       test_restore_safety_derivation_rejects_unsupported_disk_types},
      {"restore_readiness_all_passed_still_does_not_enable_execution",
       test_restore_readiness_all_passed_still_does_not_enable_execution},
      {"restore_execution_requires_injected_service_before_access",
       test_restore_execution_requires_injected_service_before_access},
      {"restore_execution_requires_exact_fresh_confirmation",
       test_restore_execution_requires_exact_fresh_confirmation},
      {"restore_execution_blocks_unknown_or_dangerous_safety_gate",
       test_restore_execution_blocks_unknown_or_dangerous_safety_gate},
      {"restore_execution_passes_verified_contract_and_progress",
       test_restore_execution_passes_verified_contract_and_progress},
      {"restore_execution_rejects_incomplete_service_report",
       test_restore_execution_rejects_incomplete_service_report},
      {"restore_safety_probe_failure_stops_without_ready_report",
       test_restore_safety_probe_failure_stops_without_ready_report},
      {"restore_job_accepts_reselected_identical_image",
       test_restore_job_accepts_reselected_identical_image},
      {"restore_candidate_paths_change_only_drive_letter",
       test_restore_candidate_paths_change_only_drive_letter},
      {"restore_job_auto_locates_matching_image_before_disk_scan",
       test_restore_job_auto_locates_matching_image_before_disk_scan},
      {"restore_job_auto_search_no_match_stops_before_disk_scan",
       test_restore_job_auto_search_no_match_stops_before_disk_scan},
      {"restore_job_auto_search_rejects_excess_candidates",
       test_restore_job_auto_search_rejects_excess_candidates},
      {"restore_job_rejects_ambiguous_image_selection_arguments",
       test_restore_job_rejects_ambiguous_image_selection_arguments},
      {"restore_job_rejects_reselected_different_image_before_disk_scan",
       test_restore_job_rejects_reselected_different_image_before_disk_scan},
      {"restore_image_failure_stops_before_disk_scan",
       test_restore_image_failure_stops_before_disk_scan},
      {"restore_job_requires_image_verifier_before_disk_scan",
       test_restore_job_requires_image_verifier_before_disk_scan},
      {"restore_job_rejects_target_smaller_than_image_source",
       test_restore_job_rejects_target_smaller_than_image_source},
      {"restore_job_rejects_logical_sector_mismatch",
       test_restore_job_rejects_logical_sector_mismatch},
      {"restore_job_rejects_original_source_as_target",
       test_restore_job_rejects_original_source_as_target},
      {"tampered_job_stops_before_disk_enumeration",
       test_tampered_job_stops_before_disk_enumeration},
      {"job_identity_change_fails_closed",
       test_job_identity_change_fails_closed},
      {"job_preflight_requires_loader_before_enumeration",
       test_job_preflight_requires_loader_before_enumeration},
      {"text_preflight_reports_confirmation_without_execution",
       test_text_preflight_reports_confirmation_without_execution},
      {"json_preflight_is_machine_readable",
       test_json_preflight_is_machine_readable},
      {"non_raw_target_fails_closed", test_non_raw_target_fails_closed},
      {"inventory_issues_fail_closed", test_inventory_issues_fail_closed},
      {"execute_argument_is_rejected_without_enumeration",
       test_execute_argument_is_rejected_without_enumeration},
      {"clone_execute_is_rejected_when_service_is_not_injected",
       test_clone_execute_is_rejected_when_service_is_not_injected},
      {"injected_execution_requires_exact_confirmation",
       test_injected_execution_requires_exact_confirmation},
      {"injected_execution_receives_stable_identities",
       test_injected_execution_receives_stable_identities},
      {"boot_repair_preflight_is_read_only_and_machine_readable",
       test_boot_repair_preflight_is_read_only_and_machine_readable},
      {"boot_repair_preflight_accepts_unassigned_system_partition_mode",
       test_boot_repair_preflight_accepts_unassigned_system_partition_mode},
      {"boot_repair_is_rejected_when_service_is_not_injected",
       test_boot_repair_is_rejected_when_service_is_not_injected},
      {"boot_repair_execution_requires_exact_confirmation",
       test_boot_repair_execution_requires_exact_confirmation},
      {"boot_repair_execution_reports_verified_bcd_store",
       test_boot_repair_execution_reports_verified_bcd_store},
      {"winre_diagnostic_is_read_only_and_machine_readable",
       test_winre_diagnostic_is_read_only_and_machine_readable},
      {"winre_diagnostic_unknown_fails_closed_after_reporting",
       test_winre_diagnostic_unknown_fails_closed_after_reporting},
      {"winre_diagnostic_rejects_invalid_input_before_access",
       test_winre_diagnostic_rejects_invalid_input_before_access},
      {"winre_diagnostic_requires_injected_service",
       test_winre_diagnostic_requires_injected_service},
      {"winre_view_accepts_verified_registered_partition",
       test_winre_view_accepts_verified_registered_partition},
      {"winre_view_accepts_verified_windows_fallback_for_rebuild",
       test_winre_view_accepts_verified_windows_fallback_for_rebuild},
      {"winre_view_fails_closed_on_incomplete_evidence",
       test_winre_view_fails_closed_on_incomplete_evidence},
      {"winpe_repair_actions_never_overlap_at_supported_widths",
       test_winpe_repair_actions_never_overlap_at_supported_widths},
      {"product_io_policy_uses_validated_16_mib_boundary",
       test_product_io_policy_uses_validated_16_mib_boundary},
      {"product_io_policy_reduces_deterministic_request_count",
       test_product_io_policy_reduces_deterministic_request_count},
  };

  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << failure.message << '\n';
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "FAIL " << name << ": unexpected exception: "
                << exception.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}
