#include "ytec/windowsapp/job_result_import.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

constexpr std::array<std::wstring_view, 2> kResultPrefixes{
    L"Tsumugi-clone-job.result-",
    L"Tsumugi-restore-job.result-",
};

clonecore::Error import_error(
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
  const auto decimal = [&value](
      const std::size_t offset, const std::size_t count) -> WORD {
    WORD result{};
    for (std::size_t index = 0; index < count; ++index) {
      result = static_cast<WORD>(
          result * 10U + static_cast<unsigned int>(value[offset + index] - L'0'));
    }
    return result;
  };
  SYSTEMTIME time{};
  time.wYear = decimal(0U, 4U);
  time.wMonth = decimal(4U, 2U);
  time.wDay = decimal(6U, 2U);
  time.wHour = decimal(9U, 2U);
  time.wMinute = decimal(11U, 2U);
  time.wSecond = decimal(13U, 2U);
  FILETIME converted{};
  return time.wYear >= 2020U && SystemTimeToFileTime(&time, &converted) != FALSE;
}

bool equals_ordinal_ignore_case(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  if (left.size() != right.size() ||
      left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return false;
  }
  return CompareStringOrdinal(
             left.data(),
             static_cast<int>(left.size()),
             right.data(),
             static_cast<int>(right.size()),
             TRUE) == CSTR_EQUAL;
}

std::wstring file_name_from_path(const std::wstring& path) {
  const std::size_t separator = path.find_last_of(L'\\');
  return separator == std::wstring::npos
      ? path
      : path.substr(separator + 1U);
}

bool is_local_absolute_result_path(const std::wstring_view path) noexcept {
  return path.size() >= 4U && path.size() < 32U * 1024U &&
      ((path[0] >= L'A' && path[0] <= L'Z') ||
       (path[0] >= L'a' && path[0] <= L'z')) &&
      path[1] == L':' && path[2] == L'\\' &&
      path.find(L'/') == std::wstring_view::npos &&
      path.find(L':', 2U) == std::wstring_view::npos &&
      path.find(L"\\..\\") == std::wstring_view::npos &&
      !path.ends_with(L"\\..") && !path.ends_with(L'\\') &&
      is_product_job_result_file_name(file_name_from_path(std::wstring(path)));
}

bool file_name_matches_record(
    const std::wstring_view file_name,
    const imageformat::JobResultRecord& record) {
  std::wstring_view expected_prefix;
  if (record.job_type == imageformat::JobType::clone ||
      record.job_type == imageformat::JobType::mbr_to_gpt) {
    expected_prefix = kResultPrefixes[0];
  } else if (record.job_type == imageformat::JobType::restore_image) {
    expected_prefix = kResultPrefixes[1];
  } else {
    return false;
  }
  if (!is_product_job_result_file_name(file_name) ||
      !equals_ordinal_ignore_case(
          file_name.substr(0, expected_prefix.size()), expected_prefix)) {
    return false;
  }
  const std::string& utc = record.completed_utc;
  if (utc.size() != 20U) {
    return false;
  }
  std::wstring compact;
  compact.reserve(16U);
  for (const std::size_t index : {
           0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U}) {
    compact.push_back(static_cast<wchar_t>(utc[index]));
  }
  compact.push_back(L'-');
  for (const std::size_t index : {11U, 12U, 14U, 15U, 17U, 18U}) {
    compact.push_back(static_cast<wchar_t>(utc[index]));
  }
  compact.push_back(L'Z');
  return file_name.substr(expected_prefix.size(), 16U) == compact;
}

class WindowsJobResultCandidateProvider final
    : public IJobResultCandidateProvider {
 public:
  clonecore::Result<std::vector<std::wstring>> candidates() override {
    SetLastError(ERROR_SUCCESS);
    const DWORD logical_drives = GetLogicalDrives();
    if (logical_drives == 0U && GetLastError() != ERROR_SUCCESS) {
      return clonecore::Result<std::vector<std::wstring>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::enumeration_failed,
              L"WinPE結果ログのドライブ列挙",
              GetLastError()));
    }

    std::uint32_t eligible_mask{};
    for (wchar_t letter = L'C'; letter <= L'Z'; ++letter) {
      if (letter == L'X') {
        continue;
      }
      const std::uint32_t bit =
          1U << static_cast<std::uint32_t>(letter - L'A');
      if ((logical_drives & bit) == 0U) {
        continue;
      }
      const std::wstring root{letter, L':', L'\\'};
      const UINT type = GetDriveTypeW(root.c_str());
      if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) {
        eligible_mask |= bit;
      }
    }

    std::vector<std::wstring> paths;
    for (const auto& directory :
         build_job_result_search_directories(eligible_mask)) {
      if (directory.size() > 3U) {
        const std::wstring directory_path =
            directory.substr(0, directory.size() - 1U);
        const DWORD attributes = GetFileAttributesW(directory_path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
          const DWORD native_code = GetLastError();
          if (native_code == ERROR_FILE_NOT_FOUND ||
              native_code == ERROR_PATH_NOT_FOUND) {
            continue;
          }
          return clonecore::Result<std::vector<std::wstring>>::failure(
              clonecore::make_win32_error(
                  clonecore::ErrorCode::query_failed,
                  L"WinPE結果ログ候補フォルダー確認",
                  native_code));
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
          return clonecore::Result<std::vector<std::wstring>>::failure(
              import_error(
                  clonecore::ErrorCode::unsupported_layout,
                  ERROR_REPARSE_TAG_INVALID,
                  L"WinPE結果ログ候補フォルダー確認",
                  L"Tsumugi候補がディレクトリでないかreparse pointです"));
        }
      }
      const std::wstring pattern = directory + L"Tsumugi-*-job.result-*.log";
      WIN32_FIND_DATAW data{};
      HANDLE search = FindFirstFileW(pattern.c_str(), &data);
      if (search == INVALID_HANDLE_VALUE) {
        const DWORD native_code = GetLastError();
        if (native_code == ERROR_FILE_NOT_FOUND ||
            native_code == ERROR_PATH_NOT_FOUND) {
          continue;
        }
        return clonecore::Result<std::vector<std::wstring>>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::enumeration_failed,
                L"WinPE結果ログ候補列挙",
                native_code));
      }
      const auto close_search = [&search]() noexcept {
        if (search != INVALID_HANDLE_VALUE) {
          FindClose(search);
          search = INVALID_HANDLE_VALUE;
        }
      };
      for (;;) {
        const std::wstring_view name(data.cFileName);
        if (is_product_job_result_file_name(name)) {
          if ((data.dwFileAttributes &
               (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
              0U) {
            close_search();
            return clonecore::Result<std::vector<std::wstring>>::failure(
                import_error(
                    clonecore::ErrorCode::unsupported_layout,
                    ERROR_REPARSE_TAG_INVALID,
                    L"WinPE結果ログ候補属性",
                    L"固定候補名にディレクトリまたはreparse pointがあります"));
          }
          paths.push_back(directory + std::wstring(name));
          if (paths.size() > kMaximumImportedJobResults) {
            close_search();
            return clonecore::Result<std::vector<std::wstring>>::failure(
                import_error(
                    clonecore::ErrorCode::invalid_data,
                    ERROR_BUFFER_OVERFLOW,
                    L"WinPE結果ログ候補数",
                    L"結果ログ候補が安全上限を超えています"));
          }
        }
        if (FindNextFileW(search, &data) == FALSE) {
          const DWORD native_code = GetLastError();
          close_search();
          if (native_code != ERROR_NO_MORE_FILES) {
            return clonecore::Result<std::vector<std::wstring>>::failure(
                clonecore::make_win32_error(
                    clonecore::ErrorCode::enumeration_failed,
                    L"WinPE結果ログ候補列挙継続",
                    native_code));
          }
          break;
        }
      }
    }
    return clonecore::Result<std::vector<std::wstring>>::success(
        std::move(paths));
  }
};

class WindowsJobResultLoader final : public IJobResultLoader {
 public:
  clonecore::Result<std::vector<std::byte>> load(
      const std::wstring& path) override {
    if (!is_local_absolute_result_path(path)) {
      return clonecore::Result<std::vector<std::byte>>::failure(import_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_BAD_PATHNAME,
          L"WinPE結果ログパス",
          L"ローカルドライブ上の製品固定名だけを読み込めます"));
    }
    clonecore::UniqueHandle file(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!file) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"WinPE結果ログ読取り開始",
              GetLastError()));
    }
    FILE_ATTRIBUTE_TAG_INFO tag_info{};
    if (!GetFileInformationByHandleEx(
            file.get(), FileAttributeTagInfo, &tag_info, sizeof(tag_info))) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"WinPE結果ログ属性確認",
              GetLastError()));
    }
    if ((tag_info.FileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
      return clonecore::Result<std::vector<std::byte>>::failure(import_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_REPARSE_TAG_INVALID,
          L"WinPE結果ログ属性確認",
          L"ディレクトリまたはreparse pointは読み込めません"));
    }
    LARGE_INTEGER initial_size{};
    if (!GetFileSizeEx(file.get(), &initial_size) ||
        initial_size.QuadPart <= 0 ||
        static_cast<std::uint64_t>(initial_size.QuadPart) >
            imageformat::kMaximumJobResultLogBytes) {
      return clonecore::Result<std::vector<std::byte>>::failure(import_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_FILE_TOO_LARGE,
          L"WinPE結果ログ長確認",
          L"結果ログが空か、許容上限を超えています"));
    }
    std::vector<std::byte> bytes(
        static_cast<std::size_t>(initial_size.QuadPart));
    std::size_t offset{};
    while (offset < bytes.size()) {
      const DWORD requested = static_cast<DWORD>((std::min<std::size_t>)(
          bytes.size() - offset,
          (std::numeric_limits<DWORD>::max)()));
      DWORD read{};
      if (!ReadFile(
              file.get(), bytes.data() + offset, requested, &read, nullptr) ||
          read == 0U) {
        return clonecore::Result<std::vector<std::byte>>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::io_failed,
                L"WinPE結果ログ読取り",
                GetLastError()));
      }
      offset += read;
    }
    LARGE_INTEGER final_size{};
    if (!GetFileSizeEx(file.get(), &final_size) ||
        final_size.QuadPart != initial_size.QuadPart) {
      return clonecore::Result<std::vector<std::byte>>::failure(import_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_FILE_INVALID,
          L"WinPE結果ログ再確認",
          L"読取り中にファイル長が変化しました"));
    }
    return clonecore::Result<std::vector<std::byte>>::success(
        std::move(bytes));
  }
};

}  // namespace

std::vector<std::wstring> build_job_result_search_directories(
    const std::uint32_t logical_drive_mask) {
  std::vector<std::wstring> directories;
  directories.reserve(46U);
  for (wchar_t letter = L'C'; letter <= L'Z'; ++letter) {
    if (letter == L'X') {
      continue;
    }
    const std::uint32_t bit =
        1U << static_cast<std::uint32_t>(letter - L'A');
    if ((logical_drive_mask & bit) == 0U) {
      continue;
    }
    directories.push_back(std::wstring{letter, L':', L'\\'});
    directories.push_back(
        std::wstring{letter, L':', L'\\'} + L"Tsumugi\\");
  }
  return directories;
}

bool is_product_job_result_file_name(
    const std::wstring_view file_name) noexcept {
  constexpr std::wstring_view kExtension = L".log";
  for (const auto prefix : kResultPrefixes) {
    if (file_name.size() ==
            prefix.size() + 16U + kExtension.size() &&
        equals_ordinal_ignore_case(
            file_name.substr(0, prefix.size()), prefix) &&
        equals_ordinal_ignore_case(
            file_name.substr(file_name.size() - kExtension.size()),
            kExtension) &&
        valid_compact_utc(file_name.substr(prefix.size(), 16U))) {
      return true;
    }
  }
  return false;
}

clonecore::Result<std::vector<ImportedJobResult>>
import_verified_job_results(
    IJobResultCandidateProvider& candidate_provider,
    IJobResultLoader& loader) {
  auto candidates = candidate_provider.candidates();
  if (!candidates) {
    return clonecore::Result<std::vector<ImportedJobResult>>::failure(
        candidates.error());
  }
  if (candidates.value().size() > kMaximumImportedJobResults) {
    return clonecore::Result<std::vector<ImportedJobResult>>::failure(
        import_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_BUFFER_OVERFLOW,
            L"WinPE結果ログ候補数",
            L"結果ログ候補が安全上限を超えています"));
  }
  for (std::size_t index = 0; index < candidates.value().size(); ++index) {
    if (!is_local_absolute_result_path(candidates.value()[index])) {
      return clonecore::Result<std::vector<ImportedJobResult>>::failure(
          import_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_BAD_PATHNAME,
              L"WinPE結果ログ候補名",
              L"製品固定形式でない候補名を拒否しました"));
    }
    for (std::size_t earlier = 0; earlier < index; ++earlier) {
      if (equals_ordinal_ignore_case(
              candidates.value()[earlier], candidates.value()[index])) {
        return clonecore::Result<std::vector<ImportedJobResult>>::failure(
            import_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_DUP_NAME,
                L"WinPE結果ログ候補",
                L"同じ結果ログ候補が重複しています"));
      }
    }
  }

  std::vector<ImportedJobResult> imported;
  imported.reserve(candidates.value().size());
  for (const auto& path : candidates.value()) {
    auto bytes = loader.load(path);
    if (!bytes) {
      return clonecore::Result<std::vector<ImportedJobResult>>::failure(
          bytes.error());
    }
    auto record = imageformat::parse_and_verify_job_result_log(bytes.value());
    if (!record) {
      return clonecore::Result<std::vector<ImportedJobResult>>::failure(
          record.error());
    }
    const std::wstring file_name = file_name_from_path(path);
    if (!file_name_matches_record(file_name, record.value())) {
      return clonecore::Result<std::vector<ImportedJobResult>>::failure(
          import_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_REVISION_MISMATCH,
              L"WinPE結果ログ名と内容の照合",
              L"ジョブ種別または完了UTCが固定ファイル名と一致しません"));
    }
    imported.push_back(ImportedJobResult{
        .path = path,
        .file_name = file_name,
        .record = record.take_value(),
    });
  }
  std::sort(
      imported.begin(),
      imported.end(),
      [](const ImportedJobResult& left, const ImportedJobResult& right) {
        if (left.record.completed_utc != right.record.completed_utc) {
          return left.record.completed_utc > right.record.completed_utc;
        }
        return left.path < right.path;
      });
  return clonecore::Result<std::vector<ImportedJobResult>>::success(
      std::move(imported));
}

std::unique_ptr<IJobResultCandidateProvider>
make_windows_job_result_candidate_provider() {
  return std::make_unique<WindowsJobResultCandidateProvider>();
}

std::unique_ptr<IJobResultLoader> make_windows_job_result_loader() {
  return std::make_unique<WindowsJobResultLoader>();
}

}  // namespace ytec::windowsapp
