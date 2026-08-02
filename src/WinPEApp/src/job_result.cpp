#include "ytec/winpeapp/job_result.h"

#include "ytec/imageformat/job_file.h"

#include <Windows.h>

#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ytec::winpeapp {
namespace {

clonecore::Error result_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

bool valid_compact_utc(const std::wstring_view value) noexcept {
  if (value.size() != 16U || value[8] != L'-' || value[15] != L'Z') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8U || index == 15U) {
      continue;
    }
    if (value[index] < L'0' || value[index] > L'9') {
      return false;
    }
  }
  return true;
}

bool is_local_absolute_path(const std::wstring_view path) noexcept {
  return path.size() >= 4U && path.size() < 32U * 1024U &&
      ((path[0] >= L'A' && path[0] <= L'Z') ||
       (path[0] >= L'a' && path[0] <= L'z')) &&
      path[1] == L':' && path[2] == L'\\' &&
      path.find(L'/') == std::wstring_view::npos &&
      path.find(L':', 2U) == std::wstring_view::npos &&
      path.find(L"\\..\\") == std::wstring_view::npos &&
      !path.ends_with(L"\\..") && !path.ends_with(L'\\') &&
      !path.ends_with(L' ') && !path.ends_with(L'.');
}

bool has_extension_ignore_case(
    const std::wstring_view path,
    const std::wstring_view extension) noexcept {
  if (path.size() < extension.size() ||
      extension.size() >
          static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return false;
  }
  const std::wstring_view suffix = path.substr(path.size() - extension.size());
  return CompareStringOrdinal(
             suffix.data(),
             static_cast<int>(suffix.size()),
             extension.data(),
             static_cast<int>(extension.size()),
             TRUE) == CSTR_EQUAL;
}

}  // namespace

clonecore::Result<std::vector<std::byte>> serialize_job_result_log(
    const JobResultRecord& record) {
  return imageformat::serialize_job_result_log(record);
}

clonecore::Result<std::wstring> build_job_result_log_path(
    const std::wstring& job_path,
    const std::wstring& compact_utc) {
  if (!is_local_absolute_path(job_path) ||
      !has_extension_ignore_case(job_path, L".json") ||
      !valid_compact_utc(compact_utc)) {
    return clonecore::Result<std::wstring>::failure(result_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_BAD_PATHNAME,
        L"WinPE実行結果ログパス",
        L"ジョブパスまたはUTC時刻が不正です"));
  }
  std::wstring result = job_path.substr(0, job_path.size() - 5U);
  result += L".result-";
  result += compact_utc;
  result += L".log";
  if (result.size() >= 32U * 1024U) {
    return clonecore::Result<std::wstring>::failure(result_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_FILENAME_EXCED_RANGE,
        L"WinPE実行結果ログパス",
        L"結果ログパスがWindowsの安全上限を超えています"));
  }
  return clonecore::Result<std::wstring>::success(std::move(result));
}

clonecore::Status write_new_job_result_log(
    const std::wstring& path,
    const std::span<const std::byte> bytes) {
  return imageformat::write_new_verified_job_result_log(path, bytes);
}

}  // namespace ytec::winpeapp
