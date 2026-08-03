#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/error.h"
#include "ytec/clonecore/log.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/diskmodel/physical_disk.h"
#include "ytec/migrationengine/shrink_bundle.h"
#include "ytec/windowsapp/online_shrink_backup_job.h"
#include "ytec/winpeapp/app_runner.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <io.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef YTEC_VM_DESTRUCTIVE_TEST_ONLY
#error This validation harness must never be built as a product target.
#endif

namespace {

constexpr std::wstring_view kAuthorization =
    L"YTEC-VM-ONLY-PRODUCT-DATA-SHRINK";
constexpr std::uint64_t kSystemDiskBytes = 96ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kSourceDiskBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kTargetDiskBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::wstring_view kFixtureDirectory = L"YDC-Shrink-Fixture";
constexpr std::wstring_view kFixtureFile = L"sentinel.bin";
constexpr std::size_t kMaximumFixtureBytes = 16U * 1024U * 1024U;

bool contains_case_insensitive(
    const std::wstring_view text,
    const std::wstring_view needle) {
  if (needle.empty() || text.size() < needle.size()) {
    return false;
  }
  return std::search(
             text.begin(),
             text.end(),
             needle.begin(),
             needle.end(),
             [](const wchar_t left, const wchar_t right) {
               return std::towlower(left) == std::towlower(right);
             }) != text.end();
}

bool is_administrator() {
  SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
  PSID administrators = nullptr;
  if (!AllocateAndInitializeSid(
          &authority,
          2,
          SECURITY_BUILTIN_DOMAIN_RID,
          DOMAIN_ALIAS_RID_ADMINS,
          0,
          0,
          0,
          0,
          0,
          0,
          &administrators)) {
    return false;
  }
  BOOL member = FALSE;
  const BOOL checked = CheckTokenMembership(nullptr, administrators, &member);
  FreeSid(administrators);
  return checked != FALSE && member != FALSE;
}

std::optional<std::wstring> read_bios_registry_value(
    const wchar_t* value_name) {
  std::array<wchar_t, 256> value{};
  DWORD bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
  const LSTATUS status = RegGetValueW(
      HKEY_LOCAL_MACHINE,
      L"HARDWARE\\DESCRIPTION\\System\\BIOS",
      value_name,
      RRF_RT_REG_SZ,
      nullptr,
      value.data(),
      &bytes);
  if (status != ERROR_SUCCESS || bytes < sizeof(wchar_t)) {
    return std::nullopt;
  }
  return std::wstring(value.data());
}

bool is_virtualbox_guest() {
  const auto manufacturer = read_bios_registry_value(L"SystemManufacturer");
  const auto product = read_bios_registry_value(L"SystemProductName");
  if ((manufacturer &&
       (contains_case_insensitive(*manufacturer, L"VirtualBox") ||
        contains_case_insensitive(*manufacturer, L"innotek"))) ||
      (product &&
       (contains_case_insensitive(*product, L"VirtualBox") ||
        contains_case_insensitive(*product, L"innotek")))) {
    return true;
  }
  constexpr DWORD kRawSmbiosProvider = 'RSMB';
  const UINT required =
      GetSystemFirmwareTable(kRawSmbiosProvider, 0, nullptr, 0);
  if (required == 0U || required > 1024U * 1024U) {
    return false;
  }
  std::vector<std::byte> firmware(required);
  if (GetSystemFirmwareTable(
          kRawSmbiosProvider,
          0,
          firmware.data(),
          static_cast<DWORD>(firmware.size())) != required) {
    return false;
  }
  const std::string_view virtualbox = "VIRTUALBOX";
  const std::string_view innotek = "INNOTEK";
  const auto contains_ascii = [&](const std::string_view needle) {
    return std::search(
               firmware.begin(),
               firmware.end(),
               needle.begin(),
               needle.end(),
               [](const std::byte observed, const char expected) {
                 unsigned char value = std::to_integer<unsigned char>(observed);
                 if (value >= 'a' && value <= 'z') {
                   value = static_cast<unsigned char>(value - 'a' + 'A');
                 }
                 return value == static_cast<unsigned char>(expected);
               }) != firmware.end();
  };
  return contains_ascii(virtualbox) || contains_ascii(innotek);
}

std::optional<std::uint32_t> disk_number_for_volume(
    const std::wstring_view drive) {
  if (drive.size() != 2U || drive[1] != L':') {
    return std::nullopt;
  }
  const std::wstring path = L"\\\\.\\" + std::wstring(drive);
  HANDLE raw = CreateFileW(
      path.c_str(),
      0,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (raw == INVALID_HANDLE_VALUE) {
    return std::nullopt;
  }
  std::array<std::byte,
             sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT) * 8U>
      buffer{};
  DWORD returned = 0;
  const BOOL queried = DeviceIoControl(
      raw,
      IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
      nullptr,
      0,
      buffer.data(),
      static_cast<DWORD>(buffer.size()),
      &returned,
      nullptr);
  CloseHandle(raw);
  if (queried == FALSE || returned < sizeof(VOLUME_DISK_EXTENTS)) {
    return std::nullopt;
  }
  const auto* extents =
      reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
  if (extents->NumberOfDiskExtents != 1U) {
    return std::nullopt;
  }
  return extents->Extents[0].DiskNumber;
}

std::string current_utc_timestamp() {
  SYSTEMTIME value{};
  GetSystemTime(&value);
  std::array<char, 32> buffer{};
  const int length = std::snprintf(
      buffer.data(),
      buffer.size(),
      "%04u-%02u-%02uT%02u:%02u:%02uZ",
      static_cast<unsigned>(value.wYear),
      static_cast<unsigned>(value.wMonth),
      static_cast<unsigned>(value.wDay),
      static_cast<unsigned>(value.wHour),
      static_cast<unsigned>(value.wMinute),
      static_cast<unsigned>(value.wSecond));
  return length > 0 && static_cast<std::size_t>(length) < buffer.size()
      ? std::string(buffer.data(), static_cast<std::size_t>(length))
      : std::string{};
}

std::optional<std::filesystem::path> executable_directory() {
  std::vector<wchar_t> path(32U * 1024U, L'\0');
  const DWORD length = GetModuleFileNameW(
      nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0U || length >= path.size()) {
    return std::nullopt;
  }
  return std::filesystem::path(std::wstring(path.data(), length)).parent_path();
}

std::optional<std::vector<std::byte>> read_regular_file(
    const std::wstring& path) {
  HANDLE raw = CreateFileW(
      path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr);
  if (raw == INVALID_HANDLE_VALUE) {
    return std::nullopt;
  }
  BY_HANDLE_FILE_INFORMATION information{};
  LARGE_INTEGER length{};
  const bool valid = GetFileInformationByHandle(raw, &information) != FALSE &&
      GetFileSizeEx(raw, &length) != FALSE && length.QuadPart > 0 &&
      static_cast<std::uint64_t>(length.QuadPart) <= kMaximumFixtureBytes &&
      (information.dwFileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0U;
  if (!valid) {
    CloseHandle(raw);
    return std::nullopt;
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(length.QuadPart));
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const DWORD requested = static_cast<DWORD>((std::min)(
        bytes.size() - offset,
        static_cast<std::size_t>(1024U * 1024U)));
    DWORD read = 0;
    if (!ReadFile(raw, bytes.data() + offset, requested, &read, nullptr) ||
        read == 0U) {
      CloseHandle(raw);
      return std::nullopt;
    }
    offset += read;
  }
  CloseHandle(raw);
  return bytes;
}

void print_error(const ytec::clonecore::Error& error) {
  std::wcerr << L"ERROR_CODE="
             << ytec::clonecore::error_code_name(error.code) << L'\n'
             << L"NATIVE_CODE=" << error.native_code << L'\n'
             << L"OPERATION=" << error.operation << L'\n'
             << L"MESSAGE=" << error.message << L'\n';
}

int fail(const wchar_t* message) {
  std::wcerr << L"YDC_PRODUCT_DATA_SHRINK_VM_FAIL " << message << L'\n';
  return 1;
}

}  // namespace

int wmain(const int argc, wchar_t* argv[]) {
  if (_setmode(_fileno(stdout), _O_U8TEXT) == -1 ||
      _setmode(_fileno(stderr), _O_U8TEXT) == -1) {
    return 1;
  }
  if (argc != 3 || std::wstring_view(argv[1]) != L"--authorization" ||
      std::wstring_view(argv[2]) != kAuthorization) {
    std::wcerr << L"VM専用の固定許可語が必要です。処理は開始していません。\n";
    return 64;
  }
  if (!is_administrator() || !is_virtualbox_guest() || sizeof(void*) != 8U) {
    return fail(L"VirtualBox AMD64 guest administrator is required");
  }
  const auto source_number = disk_number_for_volume(L"S:");
  const auto system_number = disk_number_for_volume(L"C:");
  if (!source_number || !system_number || *source_number == *system_number) {
    return fail(L"fixed source or system volume mapping is missing");
  }

  auto provider = ytec::diskmodel::make_windows_disk_inventory_provider();
  const auto before_inventory = provider->enumerate();
  if (!before_inventory || !before_inventory.value().issues.empty() ||
      before_inventory.value().disks.size() != 6U) {
    if (!before_inventory) {
      print_error(before_inventory.error());
    }
    return fail(L"the fixed six-disk inventory is required");
  }
  const ytec::diskmodel::DiskInfo* source = nullptr;
  const ytec::diskmodel::DiskInfo* target = nullptr;
  std::size_t system_count = 0U;
  for (const auto& disk : before_inventory.value().disks) {
    if (!contains_case_insensitive(disk.model, L"VBOX") ||
        disk.removable.value_or(true) ||
        contains_case_insensitive(disk.bus_type, L"USB")) {
      return fail(L"all media must be fixed VirtualBox non-USB disks");
    }
    if (disk.is_system_disk) {
      ++system_count;
      if (disk.disk_number != *system_number ||
          disk.size_bytes != kSystemDiskBytes) {
        return fail(L"fixed system disk identity mismatch");
      }
    }
    if (disk.disk_number == *source_number) {
      source = &disk;
    }
    if (!disk.is_system_disk && disk.size_bytes == kTargetDiskBytes &&
        disk.partition_style == ytec::diskmodel::PartitionStyle::raw &&
        disk.partitions.empty()) {
      if (target != nullptr) {
        return fail(L"multiple raw target disks matched");
      }
      target = &disk;
    }
  }
  if (system_count != 1U || source == nullptr || target == nullptr ||
      source->is_system_disk || source->size_bytes != kSourceDiskBytes ||
      source->partition_style != ytec::diskmodel::PartitionStyle::gpt ||
      source->partitions.empty() || source->logical_sector_size != 512U ||
      target->disk_number == source->disk_number ||
      target->disk_number == *system_number || target->logical_sector_size != 512U ||
      target->offline.value_or(true) || target->read_only.value_or(true)) {
    return fail(L"fixed data source or smaller RAW target identity mismatch");
  }

  const auto source_identity =
      ytec::diskmodel::make_stable_disk_identity(*source, false);
  const auto target_identity =
      ytec::diskmodel::make_stable_disk_identity(*target, false);
  if (!source_identity || !target_identity) {
    return fail(L"stable disk identity creation failed");
  }
  const std::wstring source_fixture =
      L"S:\\" + std::wstring(kFixtureDirectory) + L"\\" +
      std::wstring(kFixtureFile);
  const auto fixture_before = read_regular_file(source_fixture);
  if (!fixture_before || fixture_before->size() < 1024U * 1024U) {
    return fail(L"source sentinel is missing or too small");
  }

  const auto work = executable_directory();
  if (!work) {
    return fail(L"guest work directory is unavailable");
  }
  const std::filesystem::path bundle_directory = *work / L"data-backup.dcmig";
  const std::filesystem::path manifest_path =
      bundle_directory / ytec::migrationengine::kShrinkBundleManifestFileName;
  const std::filesystem::path scratch_directory = *work / L"scratch";
  if (GetFileAttributesW(bundle_directory.c_str()) != INVALID_FILE_ATTRIBUTES ||
      (!CreateDirectoryW(scratch_directory.c_str(), nullptr) &&
       GetLastError() != ERROR_ALREADY_EXISTS)) {
    return fail(L"new bundle and scratch paths are required");
  }

  std::size_t progress_events = 0U;
  bool progress_monotonic = true;
  std::uint64_t last_written = 0U;
  const ytec::clonecore::DiskOperationCallbacks callbacks{
      .progress =
          [&](const ytec::clonecore::DiskOperationProgress& progress) {
            progress_monotonic =
                progress_monotonic && progress.written_bytes >= last_written;
            last_written = progress.written_bytes;
            ++progress_events;
          },
  };
  const std::string created_utc = current_utc_timestamp();
  if (created_utc.empty()) {
    return fail(L"UTC timestamp creation failed");
  }
  auto logger = ytec::clonecore::make_stderr_logger();
  const auto backup_started = std::chrono::steady_clock::now();
  auto backup =
      ytec::windowsapp::execute_online_shrink_backup_job_with_windows_apis(
          ytec::windowsapp::OnlineShrinkBackupJobRequest{
              .selected_source = *source,
              .final_bundle_directory = bundle_directory.wstring(),
              .scratch_directory = scratch_directory.wstring(),
              .administrator = true,
              .windows_major = 0U,
              .windows_minor = 0U,
              .windows_build = 0U,
              .windows_architecture = {},
              .created_utc = created_utc,
              .app_version = "0.2.0-dev-vm-data-shrink",
              .async_wait =
                  ytec::vssrequester::AsyncWaitOptions{
                      .timeout_ms = 120'000,
                      .poll_interval_ms = 250,
                  },
              .callbacks = callbacks,
              .logger = &logger,
          });
  const auto backup_finished = std::chrono::steady_clock::now();
  if (!backup) {
    print_error(backup.error());
    return fail(L"online data shrink backup failed");
  }
  auto verified = ytec::migrationengine::verify_shrink_bundle_read_only(
      manifest_path.wstring());
  if (!verified) {
    print_error(verified.error());
    return fail(L"completed shrink bundle verification failed");
  }
  const std::size_t data_partition_count = static_cast<std::size_t>(
      std::count_if(
          verified.value().manifest.partitions.begin(),
          verified.value().manifest.partitions.end(),
          [](const auto& partition) {
            return partition.role ==
                ytec::migrationcore::MigrationPartitionRole::data;
          }));
  if (!backup.value().workflow.snapshot_data_copied ||
      !backup.value().workflow.backup_completed ||
      !backup.value().workflow.snapshots_deleted ||
      !backup.value().final_bundle_committed_after_vss_cleanup ||
      !backup.value().bundle.committed_after_complete_verification ||
      verified.value().manifest.source.is_system_disk ||
      verified.value().manifest.source.size_bytes != kSourceDiskBytes ||
      data_partition_count != 1U ||
      verified.value().payloads.size() != 1U ||
      verified.value().manifest.windows_major != 0U ||
      verified.value().manifest.windows_build != 0U) {
    return fail(L"data-only shrink bundle report or manifest is incomplete");
  }

  const ytec::clonecore::TargetConfirmation confirmation{
      .first_step_acknowledged = true,
      .typed_token = ytec::clonecore::make_target_confirmation_token(
          target_identity.value()),
  };
  if (confirmation.typed_token.empty()) {
    return fail(L"target confirmation token creation failed");
  }
  auto restore_service =
      ytec::winpeapp::make_windows_shrink_restore_execution_service();
  const auto restore_started = std::chrono::steady_clock::now();
  auto restored = restore_service->execute(
      ytec::winpeapp::RestoreExecutionRequest{
          .expected_target = target_identity.value(),
          .expected_image =
              ytec::imageformat::RestoreImageIdentity{
                  .length_bytes = verified.value().manifest_length_bytes,
                  .global_hash = verified.value().manifest_sha256,
              },
          .verified_image_path = verified.value().manifest_path,
          .transfer_mode = ytec::imageformat::TransferMode::shrink,
          .scratch_directory = scratch_directory.wstring(),
          .confirmation = confirmation,
          .callbacks = callbacks,
      });
  const auto restore_finished = std::chrono::steady_clock::now();
  if (!restored) {
    print_error(restored.error());
    return fail(L"smaller RAW target restore failed");
  }
  if (!restored.value().complete_image_verified_before_write ||
      !restored.value().backup_manifest_verified_before_write ||
      !restored.value().read_back_verified ||
      !restored.value().partition_table_committed ||
      !restored.value().target_returned_online ||
      restored.value().boot_finalization_required ||
      !restored.value().temporary_mounts_released ||
      !restored.value().boot_finalization_verified ||
      restored.value().restored_chunk_count != 1U) {
    return fail(L"data-only shrink restore report is incomplete");
  }

  const auto after_inventory = provider->enumerate();
  if (!after_inventory || !after_inventory.value().issues.empty() ||
      after_inventory.value().disks.size() != 6U) {
    return fail(L"post-restore inventory is incomplete");
  }
  const ytec::diskmodel::DiskInfo* source_after = nullptr;
  const ytec::diskmodel::DiskInfo* target_after = nullptr;
  for (const auto& disk : after_inventory.value().disks) {
    if (disk.disk_number == source->disk_number) {
      source_after = &disk;
    }
    if (disk.disk_number == target->disk_number) {
      target_after = &disk;
    }
  }
  if (source_after == nullptr || target_after == nullptr ||
      target_after->partition_style != ytec::diskmodel::PartitionStyle::gpt ||
      target_after->partitions.empty()) {
    return fail(L"restored GPT target was not reidentified");
  }
  const auto observed_source_identity =
      ytec::diskmodel::make_stable_disk_identity(*source_after, false);
  if (!observed_source_identity ||
      !ytec::clonecore::validate_stable_identity(
          source_identity.value(),
          observed_source_identity.value(),
          L"VMデータ原本再確認")) {
    return fail(L"source identity changed during backup and restore");
  }
  const auto fixture_after = read_regular_file(source_fixture);
  if (!fixture_after || *fixture_after != *fixture_before) {
    return fail(L"source sentinel changed during backup and restore");
  }

  const auto data_partition = std::max_element(
      target_after->partitions.begin(),
      target_after->partitions.end(),
      [](const auto& left, const auto& right) {
        return left.size_bytes < right.size_bytes;
      });
  const std::array<ytec::diskmodel::VolumePartitionLocation, 1> location{
      ytec::diskmodel::VolumePartitionLocation{
          .table_index = 0U,
          .offset_bytes = data_partition->offset_bytes,
      },
  };
  auto binding = ytec::diskmodel::query_windows_volume_bindings_by_offset(
      *target_after, std::span(location));
  if (!binding || binding.value().size() != 1U) {
    if (!binding) {
      print_error(binding.error());
    }
    return fail(L"restored data volume mapping failed");
  }
  const std::wstring restored_fixture =
      binding.value()[0].volume_device_path + std::wstring(kFixtureDirectory) +
      L"\\" + std::wstring(kFixtureFile);
  const auto restored_bytes = read_regular_file(restored_fixture);
  if (!restored_bytes || *restored_bytes != *fixture_before) {
    return fail(L"restored sentinel does not match the source");
  }
  if (!progress_monotonic || progress_events == 0U) {
    return fail(L"progress callbacks were absent or non-monotonic");
  }

  const auto backup_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          backup_finished - backup_started)
          .count();
  const auto restore_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          restore_finished - restore_started)
          .count();
  std::wcout << L"YDC_PRODUCT_DATA_SHRINK_PASS\n"
             << L"SOURCE_BYTES=" << kSourceDiskBytes << L'\n'
             << L"TARGET_BYTES=" << kTargetDiskBytes << L'\n'
             << L"FIXTURE_BYTES=" << fixture_before->size() << L'\n'
             << L"PAYLOAD_BYTES=" << backup.value().bundle.total_payload_bytes
             << L'\n'
             << L"RESTORED_BYTES=" << restored.value().restored_data_bytes
             << L'\n'
             << L"BACKUP_MS=" << backup_ms << L'\n'
             << L"RESTORE_MS=" << restore_ms << L'\n'
             << L"SOURCE_UNCHANGED=true\n"
             << L"BOOT_FINALIZATION_REQUIRED=false\n"
             << std::flush;
  return 0;
}
