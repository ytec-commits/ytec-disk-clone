#include "vm_clone_execution_service.h"

#include "ytec/clonecore/unique_handle.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/imageformat/job_file.h"
#include "ytec/imageformat/job_manifest.h"
#include "ytec/imageformat/job_result.h"
#include "ytec/winpeapp/app_runner.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef YTEC_VM_DESTRUCTIVE_TEST_ONLY
#error This executable must never be built as a product target.
#endif

namespace {

constexpr std::wstring_view kAuthorization =
    L"YTEC-VM-ONLY-PRODUCT-AUTO-ONCE-MONITOR";
constexpr std::wstring_view kJobPath =
    L"Y:\\Tsumugi\\Tsumugi-clone-job.json";
constexpr std::wstring_view kResultPattern =
    L"Y:\\Tsumugi\\Tsumugi-clone-job.result-*.log";
constexpr std::uint64_t kTargetBytes =
    3ULL * 1024ULL * 1024ULL * 1024ULL;

class UniqueFindHandle final {
 public:
  explicit UniqueFindHandle(HANDLE handle) noexcept : handle_(handle) {}
  ~UniqueFindHandle() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) {
      FindClose(handle_);
    }
  }

  UniqueFindHandle(const UniqueFindHandle&) = delete;
  UniqueFindHandle& operator=(const UniqueFindHandle&) = delete;

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return handle_ != INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE handle_{INVALID_HANDLE_VALUE};
};

ytec::clonecore::Error monitor_error(
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

ytec::clonecore::Result<std::optional<std::wstring>>
find_unique_result_log() {
  WIN32_FIND_DATAW entry{};
  UniqueFindHandle search(FindFirstFileW(kResultPattern.data(), &entry));
  if (!search) {
    const DWORD native_code = GetLastError();
    if (native_code == ERROR_FILE_NOT_FOUND) {
      return ytec::clonecore::Result<std::optional<std::wstring>>::success(
          std::nullopt);
    }
    return ytec::clonecore::Result<std::optional<std::wstring>>::failure(
        monitor_error(
            ytec::clonecore::ErrorCode::query_failed,
            native_code,
            L"auto-once結果ログ検索",
            L"結果ログ候補を列挙できませんでした"));
  }

  std::optional<std::wstring> result;
  do {
    if ((entry.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
      return ytec::clonecore::Result<std::optional<std::wstring>>::failure(
          monitor_error(
              ytec::clonecore::ErrorCode::unsupported_layout,
              ERROR_REPARSE_TAG_INVALID,
              L"auto-once結果ログ属性",
              L"結果ログ候補が通常ファイルではありません"));
    }
    if (result.has_value()) {
      return ytec::clonecore::Result<std::optional<std::wstring>>::failure(
          monitor_error(
              ytec::clonecore::ErrorCode::invalid_data,
              ERROR_DUP_NAME,
              L"auto-once結果ログ一意性",
              L"結果ログ候補が複数あります"));
    }
    result = L"Y:\\Tsumugi\\" + std::wstring(entry.cFileName);
  } while (FindNextFileW(search.get(), &entry));

  const DWORD native_code = GetLastError();
  if (native_code != ERROR_NO_MORE_FILES) {
    return ytec::clonecore::Result<std::optional<std::wstring>>::failure(
        monitor_error(
            ytec::clonecore::ErrorCode::query_failed,
            native_code,
            L"auto-once結果ログ列挙",
            L"結果ログ候補の列挙を完了できませんでした"));
  }
  return ytec::clonecore::Result<std::optional<std::wstring>>::success(
      std::move(result));
}

ytec::clonecore::Result<std::vector<std::byte>> read_bounded_file(
    const std::wstring& path) {
  ytec::clonecore::UniqueHandle file(CreateFileW(
      path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  if (!file) {
    return ytec::clonecore::Result<std::vector<std::byte>>::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::io_failed,
            L"auto-once結果ログ読取り",
            GetLastError()));
  }
  FILE_STANDARD_INFO information{};
  if (!GetFileInformationByHandleEx(
          file.get(),
          FileStandardInfo,
          &information,
          sizeof(information)) ||
      information.Directory || information.EndOfFile.QuadPart <= 0 ||
      information.EndOfFile.QuadPart >
          static_cast<LONGLONG>(
              ytec::imageformat::kMaximumJobResultLogBytes)) {
    return ytec::clonecore::Result<std::vector<std::byte>>::failure(
        monitor_error(
            ytec::clonecore::ErrorCode::invalid_data,
            ERROR_FILE_INVALID,
            L"auto-once結果ログ寸法",
            L"結果ログが通常の有界ファイルではありません"));
  }
  std::vector<std::byte> bytes(
      static_cast<std::size_t>(information.EndOfFile.QuadPart));
  DWORD read{};
  if (!ReadFile(
          file.get(),
          bytes.data(),
          static_cast<DWORD>(bytes.size()),
          &read,
          nullptr) ||
      static_cast<std::size_t>(read) != bytes.size()) {
    return ytec::clonecore::Result<std::vector<std::byte>>::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::io_failed,
            L"auto-once結果ログ全量読取り",
            GetLastError()));
  }
  return ytec::clonecore::Result<std::vector<std::byte>>::success(
      std::move(bytes));
}

bool completed_target_present(
    ytec::diskmodel::IDiskInventoryProvider& provider) {
  const auto report = provider.enumerate();
  if (!report || !report.value().issues.empty()) {
    return false;
  }
  const auto target = std::find_if(
      report.value().disks.begin(),
      report.value().disks.end(),
      [](const auto& disk) {
        return disk.size_bytes == kTargetBytes &&
            _wcsicmp(disk.model.c_str(), L"VBOX HARDDISK") == 0;
      });
  return target != report.value().disks.end() &&
      !target->is_system_disk && target->offline.has_value() &&
      !target->offline.value() && target->read_only.has_value() &&
      !target->read_only.value() && target->removable.has_value() &&
      !target->removable.value() &&
      target->partition_style == ytec::diskmodel::PartitionStyle::gpt &&
      target->partitions.size() == 4U;
}

}  // namespace

int wmain(const int argc, wchar_t* argv[]) {
  SetConsoleOutputCP(CP_UTF8);
  if (argc != 9 || std::wstring_view(argv[1]) != L"--profile" ||
      std::wstring_view(argv[2]) != L"clone" ||
      std::wstring_view(argv[3]) != L"--mode" ||
      std::wstring_view(argv[4]) != L"auto-once" ||
      std::wstring_view(argv[5]) != L"--confirmation" ||
      std::wstring_view(argv[6]).empty() ||
      std::wstring_view(argv[7]) != L"--authorization" ||
      std::wstring_view(argv[8]) != kAuthorization) {
    std::wcerr << L"VM専用auto-once監視の固定引数が必要です。\n";
    return 2;
  }
  if (!ytec::vmtest::is_virtualbox_guest() ||
      !ytec::vmtest::is_administrator()) {
    std::wcerr << L"VirtualBox上の管理者WinPEだけで実行できます。\n";
    return 3;
  }

  auto loader = ytec::winpeapp::make_windows_job_manifest_loader();
  auto job_bytes = loader->load(std::wstring(kJobPath));
  if (!job_bytes) {
    std::wcerr << job_bytes.error().operation << L": "
               << job_bytes.error().message << L"\n";
    return 4;
  }
  auto job = ytec::imageformat::parse_and_verify_hashed_job_manifest(
      job_bytes.value());
  if (!job ||
      job.value().manifest.job_type != ytec::imageformat::JobType::clone ||
      job.value().manifest.execution_mode !=
          ytec::imageformat::JobExecutionMode::auto_once) {
    std::wcerr << L"auto-onceクローンジョブを検証できません。\n";
    return 5;
  }

  auto provider = ytec::diskmodel::make_windows_disk_inventory_provider();
  std::optional<std::wstring> result_path;
  bool target_completed = false;
  for (std::uint32_t attempt = 0; attempt < 480U; ++attempt) {
    auto candidate = find_unique_result_log();
    if (!candidate) {
      std::wcerr << candidate.error().operation << L": "
                 << candidate.error().message << L"\n";
      return 6;
    }
    if (candidate.value().has_value()) {
      result_path = candidate.take_value();
    }
    target_completed = completed_target_present(*provider);
    if (target_completed && result_path.has_value()) {
      break;
    }
    Sleep(250U);
  }
  if (!target_completed || !result_path.has_value()) {
    std::wcerr << L"auto-once実行の完了または結果ログを120秒以内に確認できません。\n";
    return 7;
  }
  Sleep(500U);

  auto result_bytes = read_bounded_file(*result_path);
  if (!result_bytes) {
    std::wcerr << result_bytes.error().operation << L": "
               << result_bytes.error().message << L"\n";
    return 8;
  }
  auto result = ytec::imageformat::parse_and_verify_job_result_log(
      result_bytes.value());
  if (!result || result.value().job_payload_hash != job.value().payload_hash ||
      result.value().job_type != ytec::imageformat::JobType::clone ||
      result.value().outcome !=
          ytec::imageformat::JobResultOutcome::passed) {
    std::wcerr << L"auto-once結果ログの正規性、ジョブハッシュ、または結果が一致しません。\n";
    return 9;
  }

  auto repeated = ytec::imageformat::claim_job_auto_execution_once(
      std::wstring(kJobPath), job.value().payload_hash);
  if (repeated ||
      repeated.error().code !=
          ytec::clonecore::ErrorCode::confirmation_required ||
      (repeated.error().native_code != ERROR_FILE_EXISTS &&
       repeated.error().native_code != ERROR_ALREADY_EXISTS)) {
    std::wcerr << L"同一auto-onceジョブの再実行拒否を確認できません。\n";
    return 10;
  }

  std::wcout << L"YDC_PRODUCT_AUTO_ONCE_PASS resultLog="
             << *result_path
             << L" targetOnline=true partitionTableCommitted=true "
                L"repeatClaimRejected=true\n";
  return 0;
}
