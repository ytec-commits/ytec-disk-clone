#include "ytec/clonecore/error.h"
#include "ytec/clonecore/log.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/windowsapp/online_backup_job.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <fcntl.h>
#include <iostream>
#include <io.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef YTEC_VM_DESTRUCTIVE_TEST_ONLY
#error This validation harness must never be built as a product target.
#endif

namespace {

constexpr std::wstring_view kAuthorization =
    L"YTEC-VM-ONLY-PRODUCT-VSS-BACKUP";
constexpr std::uint64_t kSystemDiskBytes =
    96ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kCapacityDiskBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kOutputDiskBytes =
    32ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kCancellationThreshold =
    128ULL * 1024ULL * 1024ULL;

enum class Mode : std::uint8_t {
  capacity,
  cancel,
  success,
};

struct ParsedArguments final {
  Mode mode{Mode::success};
};

struct ProgressObservation final {
  std::uint64_t read_bytes{};
  std::uint64_t written_bytes{};
  std::uint64_t verified_bytes{};
  std::uint64_t total_read_bytes{};
  std::uint64_t total_write_bytes{};
  std::uint64_t total_verify_bytes{};
  std::size_t callback_count{};
  bool monotonic{true};
  bool cancellation_requested{};
  std::optional<std::chrono::steady_clock::time_point>
      cancellation_requested_at;
};

bool equals_case_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  return std::equal(
      left.begin(),
      left.end(),
      right.begin(),
      [](const wchar_t a, const wchar_t b) {
        return std::towlower(a) == std::towlower(b);
      });
}

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
             [](const wchar_t a, const wchar_t b) {
               return std::towlower(a) == std::towlower(b);
             }) != text.end();
}

bool contains_ascii_case_insensitive(
    const std::vector<std::uint8_t>& bytes,
    const std::string_view needle) {
  if (needle.empty() || bytes.size() < needle.size()) {
    return false;
  }
  for (std::size_t offset = 0;
       offset <= bytes.size() - needle.size();
       ++offset) {
    bool matches = true;
    for (std::size_t index = 0; index < needle.size(); ++index) {
      unsigned char observed = bytes[offset + index];
      unsigned char expected =
          static_cast<unsigned char>(needle[index]);
      if (observed >= 'a' && observed <= 'z') {
        observed = static_cast<unsigned char>(observed - 'a' + 'A');
      }
      if (expected >= 'a' && expected <= 'z') {
        expected = static_cast<unsigned char>(expected - 'a' + 'A');
      }
      if (observed != expected) {
        matches = false;
        break;
      }
    }
    if (matches) {
      return true;
    }
  }
  return false;
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
  const BOOL checked =
      CheckTokenMembership(nullptr, administrators, &member);
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

  // UEFI guests can expose generic registry BIOS strings. Fall back to the
  // same raw SMBIOS check used by the other destructive VM harnesses.
  constexpr DWORD kRawSmbiosProvider = 'RSMB';
  const UINT required =
      GetSystemFirmwareTable(kRawSmbiosProvider, 0, nullptr, 0);
  if (required == 0 || required > 1024U * 1024U) {
    return false;
  }
  std::vector<std::uint8_t> firmware(required);
  if (GetSystemFirmwareTable(
          kRawSmbiosProvider,
          0,
          firmware.data(),
          static_cast<DWORD>(firmware.size())) != required) {
    return false;
  }
  return contains_ascii_case_insensitive(firmware, "VIRTUALBOX") ||
      contains_ascii_case_insensitive(firmware, "INNOTEK");
}

std::optional<ParsedArguments> parse_arguments(
    const int argc,
    wchar_t** argv) {
  std::optional<Mode> mode;
  bool authorized = false;
  for (int index = 1; index < argc; ++index) {
    const std::wstring_view argument(argv[index]);
    if (argument == L"--mode" && index + 1 < argc && !mode) {
      const std::wstring_view value(argv[++index]);
      if (value == L"capacity") {
        mode = Mode::capacity;
      } else if (value == L"cancel") {
        mode = Mode::cancel;
      } else if (value == L"success") {
        mode = Mode::success;
      } else {
        return std::nullopt;
      }
      continue;
    }
    if (argument == L"--authorization" &&
        index + 1 < argc && !authorized) {
      authorized = std::wstring_view(argv[++index]) == kAuthorization;
      continue;
    }
    return std::nullopt;
  }
  if (!mode || !authorized) {
    return std::nullopt;
  }
  return ParsedArguments{.mode = *mode};
}

std::wstring output_path(const Mode mode) {
  switch (mode) {
    case Mode::capacity:
      return L"T:\\YDC-Product-VSS\\capacity.dcimg";
    case Mode::cancel:
      return L"U:\\YDC-Product-VSS\\cancel.dcimg";
    case Mode::success:
      return L"U:\\YDC-Product-VSS\\system.dcimg";
  }
  return {};
}

std::wstring volume_root(const Mode mode) {
  return mode == Mode::capacity ? L"T:\\" : L"U:\\";
}

std::uint64_t expected_destination_bytes(const Mode mode) {
  return mode == Mode::capacity
             ? kCapacityDiskBytes
             : kOutputDiskBytes;
}

std::optional<std::uint32_t> disk_number_for_volume(
    const std::wstring& root) {
  const std::wstring path = L"\\\\.\\" + root.substr(0, 2);
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
  if (extents->NumberOfDiskExtents != 1) {
    return std::nullopt;
  }
  return extents->Extents[0].DiskNumber;
}

bool volume_is_ntfs(const std::wstring& root) {
  std::array<wchar_t, MAX_PATH> file_system{};
  return GetVolumeInformationW(
             root.c_str(),
             nullptr,
             0,
             nullptr,
             nullptr,
             nullptr,
             file_system.data(),
             static_cast<DWORD>(file_system.size())) != FALSE &&
         equals_case_insensitive(file_system.data(), L"NTFS");
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
             : std::string();
}

void print_error(const ytec::clonecore::Error& error) {
  std::wcerr << L"ERROR_CODE="
             << ytec::clonecore::error_code_name(error.code) << L'\n'
             << L"NATIVE_CODE=" << error.native_code << L'\n'
             << L"OPERATION=" << error.operation << L'\n'
             << L"MESSAGE=" << error.message << L'\n';
}

bool regular_file_size(
    const std::wstring& path,
    std::uint64_t& size) {
  HANDLE file = CreateFileW(
      path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  LARGE_INTEGER length{};
  const BOOL queried = GetFileSizeEx(file, &length);
  CloseHandle(file);
  if (queried == FALSE || length.QuadPart <= 0) {
    return false;
  }
  size = static_cast<std::uint64_t>(length.QuadPart);
  return true;
}

bool path_absent(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes == INVALID_FILE_ATTRIBUTES &&
         GetLastError() == ERROR_FILE_NOT_FOUND;
}

int fail(const wchar_t* message) {
  std::wcerr << L"YDC_PRODUCT_VSS_VM_FAIL " << message << L'\n';
  return 1;
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
  // The VM runner redirects both streams and decodes them as UTF-8.  Without
  // an explicit Unicode mode, the MSVC wide stream conversion stops at the
  // first Japanese character under the guest's default C locale, hiding the
  // actual error and all later diagnostics.
  if (_setmode(_fileno(stdout), _O_U8TEXT) == -1 ||
      _setmode(_fileno(stderr), _O_U8TEXT) == -1) {
    return 1;
  }

  const auto parsed = parse_arguments(argc, argv);
  if (!parsed) {
    std::wcerr
        << L"VM専用です。--mode capacity|cancel|success "
        << L"--authorization YTEC-VM-ONLY-PRODUCT-VSS-BACKUP が必要です\n";
    return 64;
  }
  if (!is_administrator()) {
    return fail(L"administrator token is required");
  }
  if (!is_virtualbox_guest()) {
    return fail(L"VirtualBox guest identity is required");
  }
  if (sizeof(void*) != 8U) {
    return fail(L"AMD64 process is required");
  }

  const std::wstring root = volume_root(parsed->mode);
  const std::wstring final_path = output_path(parsed->mode);
  const std::wstring partial_path = final_path + L".partial";
  if (!volume_is_ntfs(root)) {
    return fail(L"fixed NTFS destination volume is missing");
  }
  const auto destination_number = disk_number_for_volume(root);
  if (!destination_number) {
    return fail(L"destination volume has no unique disk mapping");
  }
  if (!path_absent(final_path) || !path_absent(partial_path)) {
    return fail(L"new final and partial paths are required");
  }

  auto provider =
      ytec::diskmodel::make_windows_disk_inventory_provider();
  const auto inventory = provider->enumerate();
  if (!inventory) {
    print_error(inventory.error());
    return fail(L"disk inventory failed");
  }
  if (!inventory.value().issues.empty() ||
      inventory.value().disks.size() != 4U) {
    return fail(L"the fixed four-disk inventory is required");
  }

  const ytec::diskmodel::DiskInfo* system_disk = nullptr;
  const ytec::diskmodel::DiskInfo* destination_disk = nullptr;
  std::size_t system_disk_count = 0;
  for (const auto& disk : inventory.value().disks) {
    if (!contains_case_insensitive(disk.model, L"VBOX") ||
        disk.removable.value_or(true) ||
        equals_case_insensitive(disk.bus_type, L"USB")) {
      return fail(L"all disks must be fixed VirtualBox non-USB media");
    }
    if (disk.is_system_disk) {
      ++system_disk_count;
      system_disk = &disk;
    }
    if (disk.disk_number == destination_number.value()) {
      destination_disk = &disk;
    }
  }
  if (system_disk_count != 1U || system_disk == nullptr ||
      system_disk->size_bytes != kSystemDiskBytes ||
      system_disk->partition_style !=
          ytec::diskmodel::PartitionStyle::gpt ||
      system_disk->logical_sector_size != 512U ||
      system_disk->partitions.size() != 4U ||
      destination_disk == nullptr || destination_disk->is_system_disk ||
      destination_disk->disk_number == system_disk->disk_number ||
      destination_disk->size_bytes !=
          expected_destination_bytes(parsed->mode) ||
      destination_disk->offline.value_or(true) ||
      destination_disk->read_only.value_or(true)) {
    return fail(L"fixed source or destination disk identity mismatch");
  }

  ProgressObservation progress;
  const auto started = std::chrono::steady_clock::now();
  auto logger = ytec::clonecore::make_stderr_logger();
  const auto result =
      ytec::windowsapp::execute_online_backup_job_with_windows_apis(
          ytec::windowsapp::OnlineBackupJobRequest{
              .selected_source = *system_disk,
              .final_path = final_path,
              .administrator = true,
              .windows_major = 10,
              .windows_minor = 0,
              .windows_build = 19045,
              .windows_architecture = "AMD64",
              .created_utc = current_utc_timestamp(),
              .app_version = "0.2.0-dev",
              .async_wait =
                  ytec::vssrequester::AsyncWaitOptions{
                      .timeout_ms = 120'000,
                      .poll_interval_ms = 250,
                      .cancellation_requested =
                          [&progress]() {
                            return progress.cancellation_requested;
                          },
                  },
              .callbacks =
                  ytec::clonecore::DiskOperationCallbacks{
                      .progress =
                          [&progress, mode = parsed->mode](
                              const ytec::clonecore::DiskOperationProgress&
                                  update) {
                            progress.monotonic = progress.monotonic &&
                                update.read_bytes >= progress.read_bytes &&
                                update.written_bytes >=
                                    progress.written_bytes &&
                                update.verified_bytes >=
                                    progress.verified_bytes;
                            progress.read_bytes = update.read_bytes;
                            progress.written_bytes = update.written_bytes;
                            progress.verified_bytes = update.verified_bytes;
                            progress.total_read_bytes =
                                update.total_read_bytes;
                            progress.total_write_bytes =
                                update.total_write_bytes;
                            progress.total_verify_bytes =
                                update.total_verify_bytes;
                            ++progress.callback_count;
                            if (mode == Mode::cancel &&
                                update.cancellation_allowed &&
                                update.written_bytes >=
                                    kCancellationThreshold &&
                                !progress.cancellation_requested) {
                              progress.cancellation_requested = true;
                              progress.cancellation_requested_at =
                                  std::chrono::steady_clock::now();
                            }
                          },
                      .cancellation_requested =
                          [&progress]() {
                            return progress.cancellation_requested;
                          },
                  },
              .logger = &logger,
          });
  const auto finished = std::chrono::steady_clock::now();
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          finished - started)
          .count();

  if (!progress.monotonic || progress.callback_count == 0U) {
    if (!result) {
      print_error(result.error());
    }
    return fail(L"progress callbacks were absent or non-monotonic");
  }

  if (parsed->mode == Mode::capacity) {
    const bool result_ok = static_cast<bool>(result);
    const DWORD result_native =
        result_ok ? ERROR_SUCCESS : result.error().native_code;
    const bool final_absent = path_absent(final_path);
    const bool partial_absent = path_absent(partial_path);
    if (result_ok || result_native != ERROR_DISK_FULL ||
        !final_absent || !partial_absent) {
      if (!result_ok) {
        print_error(result.error());
      }
      return fail(L"capacity failure did not fail closed");
    }
    std::wcout << L"YDC_PRODUCT_VSS_CAPACITY_PASS\n"
               << L"ELAPSED_MS=" << elapsed_ms << L'\n'
               << L"PROGRESS_CALLBACKS=" << progress.callback_count
               << L'\n'
               << std::flush;
    return 0;
  }

  if (parsed->mode == Mode::cancel) {
    if (result || !progress.cancellation_requested ||
        !progress.cancellation_requested_at ||
        result.error().native_code != ERROR_CANCELLED ||
        result.error().code != ytec::clonecore::ErrorCode::cancelled ||
        !path_absent(final_path) || !path_absent(partial_path)) {
      if (!result) {
        print_error(result.error());
      }
      return fail(L"cancel did not fail closed and clean partial output");
    }
    const auto response_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            finished - *progress.cancellation_requested_at)
            .count();
    if (response_ms < 0 || response_ms > 10'000) {
      return fail(L"cancellation response exceeded ten seconds");
    }
    std::wcout << L"YDC_PRODUCT_VSS_CANCEL_PASS\n"
               << L"ELAPSED_MS=" << elapsed_ms << L'\n'
               << L"CANCEL_RESPONSE_MS=" << response_ms << L'\n'
               << L"WRITTEN_BEFORE_CANCEL=" << progress.written_bytes
               << L'\n'
               << L"PROGRESS_CALLBACKS=" << progress.callback_count
               << L'\n';
    return 0;
  }

  if (!result) {
    print_error(result.error());
    return fail(L"product online backup failed");
  }
  const auto& report = result.value();
  std::uint64_t final_size = 0;
  if (!regular_file_size(final_path, final_size) ||
      final_size != report.image.image_length ||
      final_size <= 1024ULL * 1024ULL * 1024ULL ||
      final_size >= kOutputDiskBytes ||
      !path_absent(partial_path) ||
      !report.final_file_committed_after_vss ||
      !report.workflow.snapshot_data_copied ||
      !report.workflow.backup_completed ||
      !report.workflow.snapshots_deleted ||
      !report.image.all_chunks_read_back_verified ||
      !report.image.global_hash_read_back_verified ||
      !report.image.committed ||
      progress.read_bytes != progress.total_read_bytes ||
      progress.written_bytes != progress.total_write_bytes ||
      progress.verified_bytes != progress.total_verify_bytes) {
    return fail(L"successful product report or final file is incomplete");
  }
  const std::uint64_t bytes_per_second =
      elapsed_ms > 0
          ? (final_size * 1000ULL) /
                static_cast<std::uint64_t>(elapsed_ms)
          : 0;
  std::wcout << L"YDC_PRODUCT_VSS_SUCCESS_PASS\n"
             << L"ELAPSED_MS=" << elapsed_ms << L'\n'
             << L"IMAGE_BYTES=" << final_size << L'\n'
             << L"STORED_DATA_BYTES=" << report.image.stored_data_bytes
             << L'\n'
             << L"ZERO_FILLED_BYTES=" << report.image.zero_filled_bytes
             << L'\n'
             << L"CHUNK_COUNT=" << report.image.chunk_count << L'\n'
             << L"WRITER_COUNT=" << report.workflow.writer_count << L'\n'
             << L"VOLUME_COUNT=" << report.workflow.volume_count << L'\n'
             << L"BYTES_PER_SECOND=" << bytes_per_second << L'\n'
             << L"PROGRESS_CALLBACKS=" << progress.callback_count << L'\n';
  return 0;
}
