#include "vm_clone_execution_service.h"

#include "ytec/clonecore/unique_handle.h"
#include "ytec/diskmodel/physical_disk.h"
#include "ytec/imageformat/backup_manifest.h"
#include "ytec/imageformat/dcimg.h"
#include "ytec/imageformat/job_file.h"
#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/restore_image_inspection.h"
#include "ytec/windowsapp/job_creation.h"

#include <Windows.h>

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#ifndef YTEC_VM_DESTRUCTIVE_TEST_ONLY
#error This executable must never be built as a product target.
#endif

namespace {

constexpr std::wstring_view kAuthorization =
    L"YTEC-VM-ONLY-PRODUCT-JOB-FIXTURE";
constexpr std::wstring_view kAutoOnceAuthorization =
    L"YTEC-VM-ONLY-PRODUCT-AUTO-ONCE-FIXTURE";
constexpr std::wstring_view kLegacyBiosAuthorization =
    L"YTEC-VM-ONLY-PRODUCT-MBR-JOB-FIXTURE";
constexpr std::wstring_view kLegacyBiosX64Authorization =
    L"YTEC-VM-ONLY-PRODUCT-MBR-X64-JOB-FIXTURE";
constexpr std::wstring_view kGptBootAuthorization =
    L"YTEC-VM-ONLY-PRODUCT-GPT-BOOT-JOB-FIXTURE";
constexpr std::wstring_view kMbr2GptAuthorization =
    L"YTEC-VM-ONLY-PRODUCT-MBR2GPT-JOB-FIXTURE";
constexpr std::wstring_view kRestoreAuthorization =
    L"YTEC-VM-ONLY-PRODUCT-RESTORE-FIXTURE";
constexpr std::wstring_view kRestoreVerifyAuthorization =
    L"YTEC-VM-ONLY-PRODUCT-RESTORE-VERIFY";
constexpr std::wstring_view kOutputDirectory = L"X:\\TsumugiValidation";
constexpr std::wstring_view kOutputPath =
    L"X:\\TsumugiValidation\\clone-job.json";
constexpr std::wstring_view kRestoreOutputPath =
    L"X:\\TsumugiValidation\\restore-job.json";
constexpr std::wstring_view kRestoreImagePath =
    L"X:\\TsumugiValidation\\synthetic-restore.dcimg";
constexpr std::uint32_t kSourceDiskNumber = 0;
constexpr std::uint64_t kRestoreSourceBytes = 4ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kRestoreTargetBytes = 8ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kRestoreDataOffset = 1ULL * 1024ULL * 1024ULL;
constexpr std::size_t kRestoreDataBytes = 4096U;
constexpr std::uint64_t kRestoreZeroOffset = 2ULL * 1024ULL * 1024ULL;
constexpr std::size_t kRestoreZeroBytes = 4096U;

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
  if (written != 20) {
    return {};
  }
  return std::string(buffer.data(), static_cast<std::size_t>(written));
}

bool prepare_fixed_output_directory(const std::wstring_view output_path) {
  if (!CreateDirectoryW(kOutputDirectory.data(), nullptr)) {
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
      return false;
    }
  }
  const DWORD attributes = GetFileAttributesW(kOutputDirectory.data());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
         GetFileAttributesW(std::wstring(output_path).c_str()) ==
             INVALID_FILE_ATTRIBUTES;
}

template <typename T>
void write_little(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

std::vector<std::byte> restore_mbr_sector() {
  std::vector<std::byte> sector(512U, std::byte{0});
  write_little<std::uint32_t>(sector, 440U, 0x52535431U);
  constexpr std::size_t kEntry = 446U;
  sector[kEntry] = std::byte{0x80};
  sector[kEntry + 4U] = std::byte{0x07};
  write_little<std::uint32_t>(sector, kEntry + 8U, 2048U);
  write_little<std::uint32_t>(sector, kEntry + 12U, 6144U);
  sector[510] = std::byte{0x55};
  sector[511] = std::byte{0xAA};
  return sector;
}

ytec::clonecore::Result<std::vector<std::byte>> build_restore_image() {
  const auto manifest = ytec::imageformat::build_backup_manifest_v1(
      ytec::imageformat::BackupImageManifest{
          .source =
              ytec::clonecore::StableDiskIdentity{
                  .disk_number = 7,
                  .model = L"YDC SYNTHETIC RESTORE SOURCE",
                  .size_bytes = kRestoreSourceBytes,
                  .logical_sector_size = 512,
                  .serial_suffix = "RESTORE1",
                  .device_instance_id = L"YDC\\RESTORE\\SOURCE",
                  .is_system_disk = true,
              },
          .physical_sector_size = 4096,
          .partition_style =
              ytec::imageformat::BackupPartitionStyle::mbr,
          .boot_mode = ytec::imageformat::BackupBootMode::legacy_bios,
          .windows_major = 10,
          .windows_minor = 0,
          .windows_build = 19045,
          .windows_architecture = "AMD64",
          .bitlocker_fully_decrypted = true,
          .compression = ytec::imageformat::DcimgCompression::none,
          .compression_version = 0,
          .chunk_size = ytec::imageformat::kDcimgChunkSize16MiB,
          .created_utc = "2026-07-31T00:00:00Z",
          .app_version = "0.2.0-dev-vm-restore-fixture",
          .partitions = {
              ytec::imageformat::BackupManifestPartition{
                  .table_index = 0,
                  .offset_bytes = kRestoreDataOffset,
                  .length_bytes = 3ULL * 1024ULL * 1024ULL,
                  .role =
                      ytec::imageformat::BackupPartitionRole::windows_ntfs,
                  .file_system =
                      ytec::imageformat::BackupFileSystem::ntfs,
                  .cluster_size = 4096,
                  .name = L"Windows",
              },
          },
      });
  if (!manifest) {
    return ytec::clonecore::Result<std::vector<std::byte>>::failure(
        manifest.error());
  }
  const auto snapshot = ytec::imageformat::build_partition_snapshot_v1(
      ytec::imageformat::PartitionSnapshot{
          .style = ytec::imageformat::PartitionTableStyle::mbr,
          .source_disk_size = kRestoreSourceBytes,
          .logical_sector_size = 512,
          .regions = {
              ytec::imageformat::PartitionTableRegion{
                  .disk_offset = 0,
                  .data = restore_mbr_sector(),
              },
          },
      });
  if (!snapshot) {
    return ytec::clonecore::Result<std::vector<std::byte>>::failure(
        snapshot.error());
  }
  return ytec::imageformat::build_uncompressed_dcimg_v1(
      ytec::imageformat::DcimgBuildRequest{
          .source_disk_size = kRestoreSourceBytes,
          .logical_sector_size = 512,
          .physical_sector_size = 4096,
          .chunk_size = ytec::imageformat::kDcimgChunkSize16MiB,
          .manifest = manifest.value(),
          .partition_table_snapshot = snapshot.value(),
          .chunks = {
              ytec::imageformat::DcimgBuildChunk{
                  .logical_offset = kRestoreDataOffset,
                  .logical_length = kRestoreDataBytes,
                  .zero_filled = false,
                  .data = std::vector<std::byte>(
                      kRestoreDataBytes, std::byte{0xA5}),
              },
              ytec::imageformat::DcimgBuildChunk{
                  .logical_offset = kRestoreZeroOffset,
                  .logical_length = kRestoreZeroBytes,
                  .zero_filled = true,
                  .data = {},
              },
          },
      });
}

ytec::clonecore::Status write_new_restore_image(
    const std::vector<std::byte>& bytes) {
  ytec::clonecore::UniqueHandle file(CreateFileW(
      kRestoreImagePath.data(),
      GENERIC_WRITE,
      0,
      nullptr,
      CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
      nullptr));
  if (!file) {
    return ytec::clonecore::Status::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::io_failed,
            L"VM復元fixture dcimg新規作成",
            GetLastError()));
  }
  DWORD written{};
  if (bytes.size() > (std::numeric_limits<DWORD>::max)() ||
      !WriteFile(
          file.get(),
          bytes.data(),
          static_cast<DWORD>(bytes.size()),
          &written,
          nullptr) ||
      written != bytes.size() || !FlushFileBuffers(file.get())) {
    return ytec::clonecore::Status::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::io_failed,
            L"VM復元fixture dcimg書込み",
            GetLastError()));
  }
  return ytec::clonecore::success_status();
}

ytec::clonecore::Result<ytec::diskmodel::DiskInfo>
select_restore_target(const bool restored) {
  auto inventory = ytec::diskmodel::make_windows_disk_inventory_provider();
  auto report = inventory->enumerate();
  if (!report) {
    return ytec::clonecore::Result<ytec::diskmodel::DiskInfo>::failure(
        report.error());
  }
  if (!report.value().issues.empty() || report.value().disks.size() != 1U) {
    return ytec::clonecore::Result<ytec::diskmodel::DiskInfo>::failure(
        ytec::clonecore::Error{
            .code = ytec::clonecore::ErrorCode::verification_failed,
            .native_code = ERROR_INVALID_DATA,
            .operation = L"VM製品復元 固定ディスク構成",
            .message = L"列挙問題があるか、物理ディスクが固定1台構成ではありません",
        });
  }
  const auto& target = report.value().disks.front();
  if (target.size_bytes != kRestoreTargetBytes ||
      target.logical_sector_size != 512U ||
      target.partition_style !=
          (restored ? ytec::diskmodel::PartitionStyle::mbr
                    : ytec::diskmodel::PartitionStyle::raw) ||
      (restored ? target.partitions.size() != 1U
                : !target.partitions.empty()) ||
      target.is_system_disk ||
      !target.offline.has_value() || target.offline.value() ||
      !target.read_only.has_value() || target.read_only.value() ||
      !target.removable.has_value() || target.removable.value() ||
      _wcsicmp(target.model.c_str(), L"VBOX HARDDISK") != 0) {
    return ytec::clonecore::Result<ytec::diskmodel::DiskInfo>::failure(
        ytec::clonecore::Error{
            .code = ytec::clonecore::ErrorCode::unsupported_layout,
            .native_code = ERROR_NOT_SUPPORTED,
            .operation = L"VM製品復元 固定RAWターゲット",
            .message = restored
                ? L"8MiBの復元済みMBR VirtualBoxディスクではありません"
                : L"8MiBのオンラインRAW VirtualBoxディスクではありません",
        });
  }
  return ytec::clonecore::Result<ytec::diskmodel::DiskInfo>::success(target);
}

int create_restore_job() {
  const auto target = select_restore_target(false);
  if (!target) {
    std::wcerr << target.error().operation << L": "
               << target.error().message << L"\n";
    return 4;
  }
  if (!prepare_fixed_output_directory(kRestoreImagePath)) {
    std::wcerr << L"固定RAM上のdcimg出力先が新規ではありません。\n";
    return 5;
  }
  const auto built_image = build_restore_image();
  if (!built_image) {
    std::wcerr << built_image.error().operation << L": "
               << built_image.error().message << L"\n";
    return 6;
  }
  const auto written = write_new_restore_image(built_image.value());
  if (!written) {
    std::wcerr << written.error().operation << L": "
               << written.error().message << L"\n";
    return 7;
  }
  const auto image =
      ytec::imageformat::inspect_restore_image_file_v1(
          std::wstring(kRestoreImagePath));
  if (!image || !image.value().complete_container_verified ||
      !image.value().metadata_verified ||
      !image.value().restore_layout_verified) {
    if (!image) {
      std::wcerr << image.error().operation << L": "
                 << image.error().message << L"\n";
    } else {
      std::wcerr << L"合成dcimgの完全検証が不十分です。\n";
    }
    return 8;
  }
  const std::wstring confirmation =
      ytec::windowsapp::restore_job_confirmation_token(target.value());
  const std::string created_utc = current_utc_timestamp();
  if (confirmation.empty() || created_utc.empty()) {
    std::wcerr << L"復元確認語または作成日時を生成できませんでした。\n";
    return 9;
  }
  const auto bytes = ytec::windowsapp::create_confirmed_restore_job(
      ytec::windowsapp::RestoreJobCreationRequest{
          .target = target.value(),
          .verified_image_path = image.value().canonical_path,
          .verified_image_length = image.value().image_length,
          .verified_image_global_hash = image.value().global_hash,
          .first_step_acknowledged = true,
          .typed_confirmation = confirmation,
          .created_utc = created_utc,
          .app_version = "0.2.0-dev-vm-restore-fixture",
      });
  if (!bytes) {
    std::wcerr << bytes.error().operation << L": "
               << bytes.error().message << L"\n";
    return 10;
  }
  if (!prepare_fixed_output_directory(kRestoreOutputPath)) {
    std::wcerr << L"固定RAM出力先が新規・通常ディレクトリではありません。\n";
    return 11;
  }
  const auto saved = ytec::imageformat::write_new_verified_job_file(
      std::wstring(kRestoreOutputPath), bytes.value());
  if (!saved) {
    std::wcerr << saved.error().operation << L": "
               << saved.error().message << L"\n";
    return 12;
  }
  std::wcout << L"YDC_PRODUCT_RESTORE_JOB_FIXTURE_PASS="
             << kRestoreOutputPath << L"\nCONFIRMATION=" << confirmation
             << L"\nIMAGE=" << image.value().canonical_path << L"\n";
  return 0;
}

int verify_restored_target() {
  const auto target = select_restore_target(true);
  if (!target) {
    std::wcerr << target.error().operation << L": "
               << target.error().message << L"\n";
    return 4;
  }
  const auto identity = ytec::diskmodel::make_stable_disk_identity(
      target.value(), false);
  if (!identity) {
    std::wcerr << identity.error().operation << L": "
               << identity.error().message << L"\n";
    return 5;
  }
  auto opened =
      ytec::diskmodel::open_verified_read_only_physical_disk_with_windows_apis(
          identity.value());
  if (!opened || !opened.value().reader) {
    if (!opened) {
      std::wcerr << opened.error().operation << L": "
                 << opened.error().message << L"\n";
    }
    return 6;
  }
  const auto table = opened.value().reader->read(0, 512U);
  const auto data = opened.value().reader->read(
      kRestoreDataOffset, kRestoreDataBytes);
  const auto zero = opened.value().reader->read(
      kRestoreZeroOffset, kRestoreZeroBytes);
  if (!table || !data || !zero ||
      table.value() != restore_mbr_sector()) {
    std::wcerr << L"復元後パーティション表領域を読取り検証できません。\n";
    return 7;
  }
  if (!std::all_of(
          data.value().begin(),
          data.value().end(),
          [](const std::byte value) { return value == std::byte{0xA5}; }) ||
      !std::all_of(
          zero.value().begin(),
          zero.value().end(),
          [](const std::byte value) { return value == std::byte{0}; })) {
    std::wcerr << L"復元後データまたはゼロ領域が固定パターンと一致しません。\n";
    return 8;
  }
  std::wcout << L"YDC_PRODUCT_RESTORE_VERIFY_PASS disk="
             << target.value().disk_number << L" bytes="
             << data.value().size() << L"\n";
  return 0;
}

}  // namespace

int wmain(const int argc, wchar_t* argv[]) {
  SetConsoleOutputCP(CP_UTF8);

  if (argc != 5 || std::wstring_view(argv[1]) != L"--profile" ||
      std::wstring_view(argv[3]) != L"--authorization") {
    std::wcerr
        << L"VM専用の固定プロファイルと許可語が必要です。ジョブは作成していません。\n";
    return 2;
  }
  const std::wstring_view profile_argument(argv[2]);
  const std::wstring_view authorization_argument(argv[4]);
  const bool restore_profile =
      profile_argument == L"restore-synthetic" &&
      authorization_argument == kRestoreAuthorization;
  const bool restore_verify_profile =
      profile_argument == L"restore-verify" &&
      authorization_argument == kRestoreVerifyAuthorization;
  const bool auto_once_profile =
      profile_argument == L"synthetic-auto-once" &&
      authorization_argument == kAutoOnceAuthorization;
  const bool mbr2gpt_profile =
      profile_argument == L"legacy-bios-x64-mbr-to-gpt" &&
      authorization_argument == kMbr2GptAuthorization;
  const bool gpt_boot_profile =
      profile_argument == L"gpt-boot-x64" &&
      authorization_argument == kGptBootAuthorization;
  const ytec::vmtest::VmCloneProfile profile =
      gpt_boot_profile
          ? ytec::vmtest::VmCloneProfile::boot
          : (profile_argument == L"synthetic" || auto_once_profile)
          ? ytec::vmtest::VmCloneProfile::synthetic
          : profile_argument == L"legacy-bios"
              ? ytec::vmtest::VmCloneProfile::legacy_bios
              : ytec::vmtest::VmCloneProfile::legacy_bios_x64;
  const bool profile_valid = restore_profile || restore_verify_profile ||
      auto_once_profile || gpt_boot_profile ||
      mbr2gpt_profile ||
      (profile_argument == L"synthetic" &&
       authorization_argument == kAuthorization) ||
      (profile_argument == L"legacy-bios" &&
       authorization_argument == kLegacyBiosAuthorization) ||
      (profile_argument == L"legacy-bios-x64" &&
       authorization_argument == kLegacyBiosX64Authorization);
  if (!profile_valid) {
    std::wcerr
        << L"VM専用プロファイルまたは固定許可語が一致しません。ジョブは作成していません。\n";
    return 2;
  }
  if (!ytec::vmtest::is_virtualbox_guest() ||
      !ytec::vmtest::is_administrator()) {
    std::wcerr << L"VirtualBox上の管理者WinPEだけで実行できます。\n";
    return 3;
  }

  if (restore_profile) {
    return create_restore_job();
  }
  if (restore_verify_profile) {
    return verify_restored_target();
  }

  const auto selection = ytec::vmtest::select_unique_vm_clone_target(
      kSourceDiskNumber, profile);
  if (!selection) {
    std::wcerr << selection.error().operation << L": "
               << selection.error().message << L"\n";
    return 4;
  }

  const std::wstring confirmation =
      ytec::windowsapp::clone_job_confirmation_token(
          selection.value().target_disk);
  const std::string created_utc = current_utc_timestamp();
  if (confirmation.empty() || created_utc.empty()) {
    std::wcerr << L"確認語または作成日時を生成できませんでした。\n";
    return 5;
  }

  const auto bytes = ytec::windowsapp::create_confirmed_clone_job(
      ytec::windowsapp::CloneJobCreationRequest{
          .source = selection.value().source_disk,
          .target = selection.value().target_disk,
          .first_step_acknowledged = true,
          .typed_confirmation = confirmation,
          .requested_conversion =
              mbr2gpt_profile
                  ? ytec::imageformat::RequestedConversion::mbr_to_gpt
                  : ytec::imageformat::RequestedConversion::preserve,
          .auto_execute_once = auto_once_profile,
          .created_utc = created_utc,
          .app_version = "0.2.0-dev-vm-fixture",
      });
  if (!bytes) {
    std::wcerr << bytes.error().operation << L": "
               << bytes.error().message << L"\n";
    return 6;
  }
  if (!prepare_fixed_output_directory(kOutputPath)) {
    std::wcerr
        << L"固定RAM出力先が新規・通常ディレクトリではありません。\n";
    return 7;
  }

  const auto saved = ytec::imageformat::write_new_verified_job_file(
      std::wstring(kOutputPath), bytes.value());
  if (!saved) {
    std::wcerr << saved.error().operation << L": "
               << saved.error().message << L"\n";
    return 8;
  }

  std::wcout << L"YDC_PRODUCT_JOB_FIXTURE_PASS=" << kOutputPath << L"\n"
             << L"CONFIRMATION=" << confirmation << L"\n";
  return 0;
}
