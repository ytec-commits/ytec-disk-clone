#include "vm_clone_execution_service.h"

#include "ytec/clonecore/operation_progress.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/imageformat/job_file.h"
#include "ytec/imageformat/restore_image_inspection.h"
#include "ytec/windowsapp/job_creation.h"
#include "ytec/winpeapp/app_runner.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef YTEC_VM_DESTRUCTIVE_TEST_ONLY
#error This executable must never be built as a product target.
#endif

namespace {

constexpr std::wstring_view kAuthorization =
    L"YTEC-VM-ONLY-PRODUCT-VSS-RESTORE";
constexpr std::wstring_view kInspectionAuthorization =
    L"YTEC-READ-ONLY-VSS-IMAGE-INSPECTION";
constexpr std::wstring_view kJobDirectory = L"X:\\TsumugiValidation";
constexpr std::wstring_view kJobPath =
    L"X:\\TsumugiValidation\\restore-job.json";
constexpr std::wstring_view kRelativeImagePath =
    L"YDC-Product-VSS\\system.dcimg";
constexpr std::uint64_t kGibibyte = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kImageDiskBytes = 32ULL * kGibibyte;
constexpr std::uint64_t kTargetDiskBytes = 96ULL * kGibibyte;

ytec::clonecore::Error vm_error(
    const ytec::clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return ytec::clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

std::string current_utc_timestamp() {
  SYSTEMTIME time{};
  GetSystemTime(&time);
  std::array<char, 21> buffer{};
  const int written = std::snprintf(
      buffer.data(),
      buffer.size(),
      "%04u-%02u-%02uT%02u:%02u:%02uZ",
      time.wYear,
      time.wMonth,
      time.wDay,
      time.wHour,
      time.wMinute,
      time.wSecond);
  return written == 20
      ? std::string(buffer.data(), static_cast<std::size_t>(written))
      : std::string{};
}

bool regular_non_reparse_file(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
      (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U &&
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
}

ytec::clonecore::Result<std::wstring> find_unique_image() {
  const DWORD drives = GetLogicalDrives();
  if (drives == 0U) {
    return ytec::clonecore::Result<std::wstring>::failure(vm_error(
        ytec::clonecore::ErrorCode::enumeration_failed,
        GetLastError(),
        L"VM VSS復元イメージのドライブ列挙",
        L"ローカルドライブを列挙できません"));
  }
  std::vector<std::wstring> matches;
  for (std::uint32_t index = 2U; index < 26U; ++index) {
    const wchar_t letter = static_cast<wchar_t>(L'A' + index);
    if (letter == L'X' || (drives & (1UL << index)) == 0U) {
      continue;
    }
    const std::wstring root = std::wstring(1U, letter) + L":\\";
    if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) {
      continue;
    }
    const std::wstring candidate = root + std::wstring(kRelativeImagePath);
    if (regular_non_reparse_file(candidate)) {
      matches.push_back(candidate);
    }
  }
  if (matches.size() != 1U) {
    return ytec::clonecore::Result<std::wstring>::failure(vm_error(
        ytec::clonecore::ErrorCode::identity_mismatch,
        matches.empty() ? ERROR_NOT_FOUND : ERROR_DUP_NAME,
        L"VM VSS復元イメージの一意選択",
        L"固定相対パスのdcimgを一つだけ確認できません"));
  }
  return ytec::clonecore::Result<std::wstring>::success(matches.front());
}

struct SafeVmLayout final {
  ytec::diskmodel::DiskInfo image_disk;
  ytec::diskmodel::DiskInfo target_disk;
};

ytec::clonecore::Result<SafeVmLayout> select_safe_layout(
    ytec::diskmodel::IDiskInventoryProvider& provider,
    const bool restored) {
  auto inventory = provider.enumerate();
  if (!inventory) {
    return ytec::clonecore::Result<SafeVmLayout>::failure(inventory.error());
  }
  if (!inventory.value().issues.empty() ||
      inventory.value().disks.size() != 2U) {
    return ytec::clonecore::Result<SafeVmLayout>::failure(vm_error(
        ytec::clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"VM VSS復元の固定ディスク構成",
        L"列挙問題のない固定2ディスク構成だけを許可します"));
  }
  std::optional<ytec::diskmodel::DiskInfo> image_disk;
  std::optional<ytec::diskmodel::DiskInfo> target_disk;
  for (const auto& disk : inventory.value().disks) {
    if (_wcsicmp(disk.model.c_str(), L"VBOX HARDDISK") != 0 ||
        disk.logical_sector_size != 512U || disk.is_system_disk ||
        !disk.offline.has_value() || disk.offline.value() ||
        !disk.read_only.has_value() || disk.read_only.value() ||
        !disk.removable.has_value() || disk.removable.value()) {
      continue;
    }
    if (disk.size_bytes == kImageDiskBytes &&
        disk.partition_style != ytec::diskmodel::PartitionStyle::raw &&
        !disk.partitions.empty()) {
      image_disk = disk;
    } else if (disk.size_bytes == kTargetDiskBytes &&
               disk.partition_style ==
                   (restored ? ytec::diskmodel::PartitionStyle::gpt
                             : ytec::diskmodel::PartitionStyle::raw) &&
               (restored ? !disk.partitions.empty()
                         : disk.partitions.empty())) {
      target_disk = disk;
    }
  }
  if (!image_disk.has_value() || !target_disk.has_value() ||
      image_disk->disk_number == target_disk->disk_number) {
    return ytec::clonecore::Result<SafeVmLayout>::failure(vm_error(
        ytec::clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"VM VSS復元の固定仮想ディスク選択",
        restored
            ? L"32GiBイメージ媒体と復元済み96GiB GPT対象を確認できません"
            : L"32GiBイメージ媒体と空の96GiB RAW対象を確認できません"));
  }
  return ytec::clonecore::Result<SafeVmLayout>::success(SafeVmLayout{
      .image_disk = image_disk.value(),
      .target_disk = target_disk.value(),
  });
}

bool valid_system_image(
    const ytec::imageformat::RestoreImageInspectionReport& image) {
  std::size_t esp_count = 0U;
  std::size_t msr_count = 0U;
  std::size_t windows_count = 0U;
  for (const auto& partition : image.manifest.partitions) {
    esp_count += partition.role ==
        ytec::imageformat::BackupPartitionRole::efi_system;
    msr_count += partition.role ==
        ytec::imageformat::BackupPartitionRole::microsoft_reserved;
    windows_count += partition.role ==
        ytec::imageformat::BackupPartitionRole::windows_ntfs;
  }
  return image.complete_container_verified && image.metadata_verified &&
      image.restore_layout_verified &&
      image.header.source_disk_size == kTargetDiskBytes &&
      image.manifest.source.size_bytes == kTargetDiskBytes &&
      image.manifest.source.logical_sector_size == 512U &&
      image.manifest.partition_style ==
          ytec::imageformat::BackupPartitionStyle::gpt &&
      image.manifest.boot_mode == ytec::imageformat::BackupBootMode::uefi &&
      image.manifest.windows_major == 10U &&
      image.manifest.windows_build >= 10240U &&
      image.manifest.windows_architecture == "AMD64" &&
      image.manifest.bitlocker_fully_decrypted &&
      image.manifest.compression ==
          ytec::imageformat::DcimgCompression::zstandard &&
      esp_count == 1U && msr_count == 1U && windows_count == 1U;
}

bool prepare_job_directory() {
  if (!CreateDirectoryW(kJobDirectory.data(), nullptr) &&
      GetLastError() != ERROR_ALREADY_EXISTS) {
    return false;
  }
  const DWORD attributes = GetFileAttributesW(kJobDirectory.data());
  return attributes != INVALID_FILE_ATTRIBUTES &&
      (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U &&
      GetFileAttributesW(kJobPath.data()) == INVALID_FILE_ATTRIBUTES;
}

}  // namespace

int wmain(const int argc, wchar_t* argv[]) {
  SetConsoleOutputCP(CP_UTF8);
  if (argc == 5 && std::wstring_view(argv[1]) == L"--inspect-image" &&
      std::wstring_view(argv[3]) == L"--authorization" &&
      std::wstring_view(argv[4]) == kInspectionAuthorization) {
    const auto image = ytec::imageformat::inspect_restore_image_file_v1(
        std::wstring(argv[2]));
    if (!image) {
      std::wcerr << L"READ_ONLY_IMAGE_INSPECTION_FAIL code="
                 << static_cast<int>(image.error().code)
                 << L" nativeCode=" << image.error().native_code
                 << L" operation=" << image.error().operation
                 << L" message=" << image.error().message << L"\n";
      return 20;
    }
    std::size_t esp_count = 0U;
    std::size_t msr_count = 0U;
    std::size_t windows_count = 0U;
    std::size_t recovery_count = 0U;
    for (const auto& partition : image.value().manifest.partitions) {
      esp_count += partition.role ==
          ytec::imageformat::BackupPartitionRole::efi_system;
      msr_count += partition.role ==
          ytec::imageformat::BackupPartitionRole::microsoft_reserved;
      windows_count += partition.role ==
          ytec::imageformat::BackupPartitionRole::windows_ntfs;
      recovery_count += partition.role ==
          ytec::imageformat::BackupPartitionRole::recovery_ntfs;
    }
    std::cout
        << "READ_ONLY_IMAGE_INSPECTION_PASS"
        << " imageLength=" << image.value().image_length
        << " containerVerified=" << image.value().complete_container_verified
        << " metadataVerified=" << image.value().metadata_verified
        << " restoreLayoutVerified=" << image.value().restore_layout_verified
        << " restoreExecutionEnabled=" << image.value().restore_execution_enabled
        << " headerSourceBytes=" << image.value().header.source_disk_size
        << " manifestSourceBytes=" << image.value().manifest.source.size_bytes
        << " logicalSector=" << image.value().manifest.source.logical_sector_size
        << " physicalSector=" << image.value().manifest.physical_sector_size
        << " partitionStyle="
        << static_cast<int>(image.value().manifest.partition_style)
        << " bootMode=" << static_cast<int>(image.value().manifest.boot_mode)
        << " windowsMajor=" << image.value().manifest.windows_major
        << " windowsMinor=" << image.value().manifest.windows_minor
        << " windowsBuild=" << image.value().manifest.windows_build
        << " windowsArchitecture="
        << image.value().manifest.windows_architecture
        << " bitlockerDecrypted="
        << image.value().manifest.bitlocker_fully_decrypted
        << " compression="
        << static_cast<int>(image.value().manifest.compression)
        << " compressionVersion="
        << image.value().manifest.compression_version
        << " partitionCount=" << image.value().manifest.partitions.size()
        << " espCount=" << esp_count
        << " msrCount=" << msr_count
        << " windowsCount=" << windows_count
        << " recoveryCount=" << recovery_count << "\n";
    return 0;
  }
  if (argc != 3 || std::wstring_view(argv[1]) != L"--authorization" ||
      std::wstring_view(argv[2]) != kAuthorization) {
    std::wcerr << L"VM専用の固定許可語が必要です。復元は開始していません。\n";
    return 2;
  }
  if (!ytec::vmtest::is_virtualbox_guest() ||
      !ytec::vmtest::is_administrator()) {
    std::wcerr << L"VirtualBox上の管理者WinPEだけで実行できます。\n";
    return 3;
  }

  auto provider = ytec::diskmodel::make_windows_disk_inventory_provider();
  auto layout = select_safe_layout(*provider, false);
  if (!layout) {
    std::wcerr << layout.error().operation << L": "
               << layout.error().message << L"\n";
    return 4;
  }
  auto image_path = find_unique_image();
  if (!image_path) {
    std::wcerr << image_path.error().operation << L": "
               << image_path.error().message << L"\n";
    return 5;
  }
  auto image = ytec::imageformat::inspect_restore_image_file_v1(
      image_path.value());
  if (!image || !valid_system_image(image.value())) {
    if (!image) {
      std::wcerr << image.error().operation << L": "
                 << image.error().message << L"\n";
    } else {
      std::wcerr << L"VSS生成dcimgが固定GPT/UEFI Windows要件を満たしません。\n";
    }
    return 6;
  }
  const std::wstring confirmation =
      ytec::windowsapp::restore_job_confirmation_token(
          layout.value().target_disk);
  const std::string created_utc = current_utc_timestamp();
  if (confirmation.empty() || created_utc.empty() ||
      !prepare_job_directory()) {
    std::wcerr << L"固定RAM上の新規ジョブ出力を準備できません。\n";
    return 7;
  }
  auto job = ytec::windowsapp::create_confirmed_restore_job(
      ytec::windowsapp::RestoreJobCreationRequest{
          .target = layout.value().target_disk,
          .verified_image_path = image.value().canonical_path,
          .verified_image_length = image.value().image_length,
          .verified_image_global_hash = image.value().global_hash,
          .first_step_acknowledged = true,
          .typed_confirmation = confirmation,
          .created_utc = created_utc,
          .app_version = "0.2.0-dev-vm-vss-restore",
      });
  if (!job) {
    std::wcerr << job.error().operation << L": "
               << job.error().message << L"\n";
    return 8;
  }
  auto saved = ytec::imageformat::write_new_verified_job_file(
      std::wstring(kJobPath), job.value());
  if (!saved) {
    std::wcerr << saved.error().operation << L": "
               << saved.error().message << L"\n";
    return 9;
  }

  std::uint32_t progress_events = 0U;
  ytec::clonecore::DiskOperationStage final_stage =
      ytec::clonecore::DiskOperationStage::planning;
  std::uint64_t maximum_written = 0U;
  std::uint64_t maximum_verified = 0U;
  const ytec::clonecore::DiskOperationCallbacks callbacks{
      .progress =
          [&](const ytec::clonecore::DiskOperationProgress& progress) {
            ++progress_events;
            final_stage = progress.stage;
            maximum_written =
                (std::max)(maximum_written, progress.written_bytes);
            maximum_verified =
                (std::max)(maximum_verified, progress.verified_bytes);
          },
  };
  auto loader = ytec::winpeapp::make_windows_job_manifest_loader();
  auto verifier = ytec::winpeapp::make_windows_restore_image_verifier();
  auto safety =
      ytec::winpeapp::make_windows_restore_execution_safety_probe();
  auto candidates =
      ytec::winpeapp::make_windows_restore_image_candidate_provider();
  auto restore = ytec::winpeapp::make_windows_restore_execution_service();
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-execute",
       L"--job-path",
       std::wstring(kJobPath),
       L"--acknowledge-target-erasure",
       L"--confirmation",
       confirmation,
       L"--json"},
      *provider,
      output,
      errors,
      nullptr,
      nullptr,
      loader.get(),
      verifier.get(),
      safety.get(),
      candidates.get(),
      &callbacks,
      restore.get());
  auto restored_layout = select_safe_layout(*provider, true);
  const std::string rendered = output.str();
  const bool passed = exit_code == 0 && errors.str().empty() &&
      progress_events > 0U &&
      final_stage == ytec::clonecore::DiskOperationStage::completed &&
      maximum_written > 0U && maximum_verified > 0U && restored_layout &&
      rendered.find("\"mode\":\"restore-execution\"") !=
          std::string::npos &&
      rendered.find("\"completeImageVerifiedBeforeWrite\":true") !=
          std::string::npos &&
      rendered.find("\"readBackVerified\":true") != std::string::npos &&
      rendered.find("\"bcdbootSignatureVerified\":true") !=
          std::string::npos &&
      rendered.find("\"bootStoreVerified\":true") != std::string::npos &&
      rendered.find("\"temporaryMountsReleased\":true") !=
          std::string::npos &&
      rendered.find("\"bootFinalizationVerified\":true") !=
          std::string::npos;
  if (!passed) {
    std::cerr << "Product VSS restore failed. exit=" << exit_code
              << " progressEvents=" << progress_events
              << " finalStage=" << static_cast<int>(final_stage)
              << " maximumWritten=" << maximum_written
              << " maximumVerified=" << maximum_verified
              << "\nstdout:\n" << rendered
              << "\nstderr:\n" << errors.str();
    return 10;
  }
  std::cout << "YDC_PRODUCT_VSS_RESTORE_PASS targetDisk="
            << restored_layout.value().target_disk.disk_number
            << " imageBytes=" << image.value().image_length
             << " writtenBytes=" << maximum_written
             << " verifiedBytes=" << maximum_verified
            << " partitionCount="
            << restored_layout.value().target_disk.partitions.size()
             << " bootFinalization=true\n" << std::flush;
  return 0;
}
