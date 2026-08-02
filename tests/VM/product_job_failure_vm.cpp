#include "ytec/clonecore/unique_handle.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/diskmodel/physical_disk.h"
#include "ytec/winpeapp/app_runner.h"

#include "vm_clone_execution_service.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef YTEC_VM_DESTRUCTIVE_TEST_ONLY
#error This executable must never be built as a product target.
#endif

namespace {

constexpr std::wstring_view kAuthorization =
    L"YTEC-VM-ONLY-PRODUCT-JOB-FAILURE";
constexpr std::wstring_view kCorruptImage = L"corrupt-image";
constexpr std::wstring_view kTamperedJob = L"tampered-job";
constexpr std::wstring_view kJobPath =
    L"X:\\TsumugiValidation\\restore-job.json";
constexpr std::wstring_view kImagePath =
    L"X:\\TsumugiValidation\\synthetic-restore.dcimg";
constexpr std::uint64_t kTargetBytes = 8ULL * 1024ULL * 1024ULL;
constexpr std::size_t kProbeBytes = 4096U;

bool fixed_restore_target_is_safe(
    ytec::diskmodel::IDiskInventoryProvider& provider) {
  const auto report = provider.enumerate();
  if (!report || !report.value().issues.empty() ||
      report.value().disks.size() != 1U) {
    return false;
  }
  const auto& target = report.value().disks.front();
  return target.disk_number == 0U && target.size_bytes == kTargetBytes &&
      target.logical_sector_size == 512U &&
      target.partition_style == ytec::diskmodel::PartitionStyle::raw &&
      target.partitions.empty() && !target.is_system_disk &&
      target.offline.has_value() && !target.offline.value() &&
      target.read_only.has_value() && !target.read_only.value() &&
      target.removable.has_value() && !target.removable.value() &&
      _wcsicmp(target.model.c_str(), L"VBOX HARDDISK") == 0;
}

ytec::clonecore::Result<std::vector<std::byte>> read_target_probes(
    ytec::diskmodel::IDiskInventoryProvider& provider) {
  const auto report = provider.enumerate();
  if (!report || !report.value().issues.empty() ||
      report.value().disks.size() != 1U) {
    return ytec::clonecore::Result<std::vector<std::byte>>::failure(
        ytec::clonecore::Error{
            .code = ytec::clonecore::ErrorCode::verification_failed,
            .native_code = ERROR_INVALID_DATA,
            .operation = L"VM失敗経路 復元先列挙",
            .message = L"固定1台の合成復元先ではありません",
        });
  }
  const auto identity = ytec::diskmodel::make_stable_disk_identity(
      report.value().disks.front(), false);
  if (!identity) {
    return ytec::clonecore::Result<std::vector<std::byte>>::failure(
        identity.error());
  }
  auto opened =
      ytec::diskmodel::open_verified_read_only_physical_disk_with_windows_apis(
          identity.value());
  if (!opened || !opened.value().reader) {
    if (!opened) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          opened.error());
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::failure(
        ytec::clonecore::Error{
            .code = ytec::clonecore::ErrorCode::io_failed,
            .native_code = ERROR_INVALID_HANDLE,
            .operation = L"VM失敗経路 復元先読取り",
            .message = L"読取り専用Readerがありません",
        });
  }
  const auto first = opened.value().reader->read(0, kProbeBytes);
  const auto last = opened.value().reader->read(
      kTargetBytes - kProbeBytes, kProbeBytes);
  if (!first) {
    return ytec::clonecore::Result<std::vector<std::byte>>::failure(
        first.error());
  }
  if (!last) {
    return ytec::clonecore::Result<std::vector<std::byte>>::failure(
        last.error());
  }
  std::vector<std::byte> probes;
  probes.reserve(first.value().size() + last.value().size());
  probes.insert(probes.end(), first.value().begin(), first.value().end());
  probes.insert(probes.end(), last.value().begin(), last.value().end());
  return ytec::clonecore::Result<std::vector<std::byte>>::success(
      std::move(probes));
}

ytec::clonecore::Status flip_file_byte(
    const std::wstring_view path,
    const bool last_byte) {
  const DWORD attributes = GetFileAttributesW(std::wstring(path).c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return ytec::clonecore::Status::failure(ytec::clonecore::Error{
        .code = ytec::clonecore::ErrorCode::invalid_argument,
        .native_code = ERROR_INVALID_DATA,
        .operation = L"VM失敗経路 RAMファイル確認",
        .message = L"通常ファイルではありません",
    });
  }
  ytec::clonecore::UniqueHandle file(CreateFileW(
      std::wstring(path).c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
      nullptr));
  if (!file) {
    return ytec::clonecore::Status::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::io_failed,
            L"VM失敗経路 RAMファイルopen",
            GetLastError()));
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0) {
    return ytec::clonecore::Status::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::io_failed,
            L"VM失敗経路 RAMファイルsize",
            GetLastError()));
  }
  LARGE_INTEGER offset{};
  offset.QuadPart = last_byte ? size.QuadPart - 1 : 0;
  if (!SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN)) {
    return ytec::clonecore::Status::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::io_failed,
            L"VM失敗経路 RAMファイルseek",
            GetLastError()));
  }
  std::byte value{};
  DWORD transferred{};
  if (!ReadFile(file.get(), &value, 1U, &transferred, nullptr) ||
      transferred != 1U) {
    return ytec::clonecore::Status::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::io_failed,
            L"VM失敗経路 RAMファイルread",
            GetLastError()));
  }
  value ^= std::byte{0x01};
  if (!SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN) ||
      !WriteFile(file.get(), &value, 1U, &transferred, nullptr) ||
      transferred != 1U || !FlushFileBuffers(file.get())) {
    return ytec::clonecore::Status::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::io_failed,
            L"VM失敗経路 RAMファイル改ざん",
            GetLastError()));
  }
  return ytec::clonecore::success_status();
}

int fail(const std::wstring_view message) {
  std::wcerr << L"YDC_PRODUCT_JOB_FAILURE_VM_FAIL " << message << L'\n';
  return 5;
}

}  // namespace

int wmain(const int argc, wchar_t* argv[]) {
  SetConsoleOutputCP(CP_UTF8);
  if (argc != 7 || std::wstring_view(argv[1]) != L"--scenario" ||
      std::wstring_view(argv[3]) != L"--confirmation" ||
      std::wstring_view(argv[5]) != L"--authorization" ||
      std::wstring_view(argv[6]) != kAuthorization) {
    std::wcerr << L"VM専用の固定シナリオ、確認語、許可語が必要です。\n";
    return 2;
  }
  const std::wstring_view scenario(argv[2]);
  const std::wstring confirmation(argv[4]);
  if ((scenario != kCorruptImage && scenario != kTamperedJob) ||
      confirmation.empty()) {
    std::wcerr << L"固定失敗シナリオまたは確認語が不正です。\n";
    return 2;
  }
  if (!ytec::vmtest::is_virtualbox_guest() ||
      !ytec::vmtest::is_administrator()) {
    std::wcerr << L"VirtualBox上の管理者WinPEだけで実行できます。\n";
    return 3;
  }

  auto provider = ytec::diskmodel::make_windows_disk_inventory_provider();
  if (!fixed_restore_target_is_safe(*provider)) {
    std::wcerr << L"8MiBオンラインRAWの固定VirtualBox復元先ではありません。\n";
    return 4;
  }
  const auto before = read_target_probes(*provider);
  if (!before) {
    return fail(before.error().message);
  }

  const auto mutated = flip_file_byte(
      scenario == kCorruptImage ? kImagePath : kJobPath,
      scenario == kCorruptImage);
  if (!mutated) {
    return fail(mutated.error().message);
  }

  auto loader = ytec::winpeapp::make_windows_job_manifest_loader();
  auto verifier = ytec::winpeapp::make_windows_restore_image_verifier();
  auto safety = ytec::winpeapp::make_windows_restore_execution_safety_probe();
  auto candidates =
      ytec::winpeapp::make_windows_restore_image_candidate_provider();
  auto restore = ytec::winpeapp::make_windows_restore_execution_service();
  auto product_clone =
      ytec::winpeapp::make_windows_clone_job_execution_service();
  std::ostringstream output;
  std::ostringstream error;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-execute",
       L"--job-path",
       std::wstring(kJobPath),
       L"--acknowledge-target-erasure",
       L"--confirmation",
       confirmation,
       L"--text"},
      *provider,
      output,
      error,
      nullptr,
      nullptr,
      loader.get(),
      verifier.get(),
      safety.get(),
      candidates.get(),
      nullptr,
      restore.get(),
      product_clone.get(),
      nullptr);
  if (exit_code == 0 || error.str().empty()) {
    std::cerr << "Unexpected product failure result. exit=" << exit_code
              << "\nstdout:\n" << output.str()
              << "\nstderr:\n" << error.str();
    return 5;
  }

  const auto after = read_target_probes(*provider);
  if (!after || after.value() != before.value() ||
      !fixed_restore_target_is_safe(*provider)) {
    return fail(L"失敗経路で復元先の状態または先頭/末尾バイトが変化しました");
  }

  std::cout
      << (scenario == kCorruptImage
              ? "YDC_PRODUCT_RESTORE_CORRUPT_IMAGE_PASS"
              : "YDC_PRODUCT_RESTORE_TAMPERED_JOB_PASS")
      << " targetDisk=0 targetOnline=true targetRaw=true "
         "targetBytesUnchanged=true partitionTableCommitted=false\n";
  return 0;
}
