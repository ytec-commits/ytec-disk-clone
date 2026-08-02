#include "ytec/imageformat/job_file.h"

#include "ytec/clonecore/unique_handle.h"
#include "ytec/imageformat/job_manifest.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwctype>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ytec::imageformat {
namespace {

clonecore::Error job_file_error(
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

bool is_local_absolute_json_path(const std::wstring& path) {
  constexpr std::wstring_view extension = L".json";
  if (path.size() < 8 || path.size() >= 32U * 1024U ||
      std::iswalpha(static_cast<wint_t>(path[0])) == 0 ||
      path[1] != L':' || (path[2] != L'\\' && path[2] != L'/') ||
      path.ends_with(L"\\") || path.ends_with(L"/") ||
      path.ends_with(L" ") || path.ends_with(L".") ||
      path.size() <= extension.size()) {
    return false;
  }
  const std::wstring_view suffix =
      std::wstring_view(path).substr(path.size() - extension.size());
  return CompareStringOrdinal(
             suffix.data(),
             static_cast<int>(suffix.size()),
             extension.data(),
             static_cast<int>(extension.size()),
             TRUE) == CSTR_EQUAL;
}

bool is_local_absolute_log_path(const std::wstring& path) {
  constexpr std::wstring_view extension = L".log";
  if (path.size() < 8 || path.size() >= 32U * 1024U ||
      std::iswalpha(static_cast<wint_t>(path[0])) == 0 ||
      path[1] != L':' || path[2] != L'\\' ||
      path.find(L'/') != std::wstring::npos ||
      path.find(L':', 2U) != std::wstring::npos ||
      path.find(L"\\..\\") != std::wstring::npos ||
      path.ends_with(L"\\..") || path.ends_with(L"\\") ||
      path.ends_with(L" ") || path.ends_with(L".") ||
      path.size() <= extension.size()) {
    return false;
  }
  const std::wstring_view suffix =
      std::wstring_view(path).substr(path.size() - extension.size());
  return CompareStringOrdinal(
             suffix.data(),
             static_cast<int>(suffix.size()),
             extension.data(),
             static_cast<int>(extension.size()),
             TRUE) == CSTR_EQUAL;
}

clonecore::Status verify_output_parent_chain(
    const std::wstring& path,
    const std::wstring_view operation,
    const std::wstring_view invalid_message) {
  std::size_t separator = path.find(L'\\', 3U);
  while (separator != std::wstring::npos) {
    const std::wstring parent = path.substr(0, separator);
    const DWORD attributes = GetFileAttributesW(parent.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::query_failed,
          operation,
          GetLastError()));
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return clonecore::Status::failure(job_file_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_REPARSE_TAG_INVALID,
          std::wstring(operation),
          std::wstring(invalid_message)));
    }
    separator = path.find(L'\\', separator + 1U);
  }
  return clonecore::success_status();
}

clonecore::Status failure_from_last_error(
    std::wstring operation,
    const DWORD fallback) {
  const DWORD native_code = GetLastError();
  return clonecore::Status::failure(clonecore::make_win32_error(
      clonecore::ErrorCode::io_failed,
      std::move(operation),
      native_code == ERROR_SUCCESS ? fallback : native_code));
}

std::wstring digest_to_wide_hex(const Sha256Digest& digest) {
  constexpr std::array<wchar_t, 16> kHexDigits{
      L'0', L'1', L'2', L'3', L'4', L'5', L'6', L'7',
      L'8', L'9', L'A', L'B', L'C', L'D', L'E', L'F'};
  std::wstring result;
  result.reserve(digest.size() * 2U);
  for (const std::byte value : digest) {
    const auto numeric = std::to_integer<unsigned int>(value);
    result.push_back(kHexDigits[(numeric >> 4U) & 0x0FU]);
    result.push_back(kHexDigits[numeric & 0x0FU]);
  }
  return result;
}

std::vector<std::byte> build_auto_claim_bytes(
    const Sha256Digest& digest) {
  constexpr std::string_view kPrefix =
      "YTEC-TSUMUGI-AUTO-CLAIM/1\r\njobHashSha256=";
  const std::wstring wide_hash = digest_to_wide_hex(digest);
  std::string text(kPrefix);
  text.reserve(kPrefix.size() + wide_hash.size() + 2U);
  for (const wchar_t character : wide_hash) {
    text.push_back(static_cast<char>(character));
  }
  text.append("\r\n");
  std::vector<std::byte> bytes(text.size());
  std::transform(
      text.begin(),
      text.end(),
      bytes.begin(),
      [](const char character) {
        return static_cast<std::byte>(
            static_cast<unsigned char>(character));
      });
  return bytes;
}

clonecore::Result<std::wstring> build_auto_claim_path(
    const std::wstring& job_path,
    const Sha256Digest& digest) {
  if (!is_local_absolute_json_path(job_path) ||
      job_path.find(L'/') != std::wstring::npos ||
      job_path.find(L':', 2U) != std::wstring::npos ||
      job_path.find(L"\\..\\") != std::wstring::npos ||
      job_path.ends_with(L"\\..")) {
    return clonecore::Result<std::wstring>::failure(job_file_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_BAD_PATHNAME,
        L"WinPE一回限り自動実行記録パス",
        L"ローカルドライブ上の正規化したジョブパスが必要です"));
  }
  constexpr std::size_t kJsonExtensionCharacters = 5U;
  std::wstring path =
      job_path.substr(0, job_path.size() - kJsonExtensionCharacters);
  path.append(L".auto-once-");
  path.append(digest_to_wide_hex(digest));
  path.append(L".claim");
  if (path.size() >= 32U * 1024U) {
    return clonecore::Result<std::wstring>::failure(job_file_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_FILENAME_EXCED_RANGE,
        L"WinPE一回限り自動実行記録パス",
        L"自動実行記録パスがWindowsの安全上限を超えています"));
  }
  return clonecore::Result<std::wstring>::success(std::move(path));
}

}  // namespace

clonecore::Status write_new_verified_job_file(
    const std::wstring& path,
    const std::span<const std::byte> bytes) {
  if (!is_local_absolute_json_path(path) || bytes.empty() ||
      bytes.size() > kMaximumJobManifestBytes) {
    return clonecore::Status::failure(job_file_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"ジョブファイル保存",
        L"保存先はローカルドライブ上の新しい.jsonファイルでなければなりません"));
  }
  const auto verified = parse_and_verify_hashed_job_manifest(bytes);
  if (!verified) {
    return clonecore::Status::failure(verified.error());
  }

  clonecore::UniqueHandle file(CreateFileW(
      path.c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
      nullptr));
  if (!file) {
    return failure_from_last_error(
        L"ジョブファイル新規作成", ERROR_FILE_EXISTS);
  }

  const auto fail_and_remove =
      [&file, &path](clonecore::Status status) {
        file.reset();
        static_cast<void>(DeleteFileW(path.c_str()));
        return status;
      };

  DWORD written{};
  if (!WriteFile(
          file.get(),
          bytes.data(),
          static_cast<DWORD>(bytes.size()),
          &written,
          nullptr)) {
    return fail_and_remove(
        failure_from_last_error(L"ジョブファイル書込み", ERROR_WRITE_FAULT));
  }
  if (static_cast<std::size_t>(written) != bytes.size()) {
    return fail_and_remove(clonecore::Status::failure(job_file_error(
        clonecore::ErrorCode::io_failed,
        ERROR_WRITE_FAULT,
        L"ジョブファイル書込み",
        L"ジョブファイルを最後まで書き込めませんでした")));
  }
  if (!FlushFileBuffers(file.get())) {
    return fail_and_remove(
        failure_from_last_error(L"ジョブファイルflush", ERROR_WRITE_FAULT));
  }

  LARGE_INTEGER observed_size{};
  if (!GetFileSizeEx(file.get(), &observed_size)) {
    return fail_and_remove(failure_from_last_error(
        L"ジョブファイルサイズ確認", ERROR_READ_FAULT));
  }
  if (observed_size.QuadPart < 0 ||
      static_cast<unsigned long long>(observed_size.QuadPart) !=
          bytes.size()) {
    return fail_and_remove(clonecore::Status::failure(job_file_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"ジョブファイルサイズ確認",
        L"保存したジョブのサイズが作成データと一致しません")));
  }

  LARGE_INTEGER beginning{};
  if (!SetFilePointerEx(file.get(), beginning, nullptr, FILE_BEGIN)) {
    return fail_and_remove(failure_from_last_error(
        L"ジョブファイル読戻し位置", ERROR_READ_FAULT));
  }
  std::vector<std::byte> observed(bytes.size());
  DWORD read{};
  if (!ReadFile(
          file.get(),
          observed.data(),
          static_cast<DWORD>(observed.size()),
          &read,
          nullptr)) {
    return fail_and_remove(
        failure_from_last_error(L"ジョブファイル読戻し", ERROR_READ_FAULT));
  }
  if (static_cast<std::size_t>(read) != observed.size() ||
      !std::equal(observed.begin(), observed.end(), bytes.begin())) {
    return fail_and_remove(clonecore::Status::failure(job_file_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"ジョブファイル読戻し検証",
        L"保存したジョブが作成データと一致しません")));
  }
  if (!parse_and_verify_hashed_job_manifest(observed)) {
    return fail_and_remove(clonecore::Status::failure(job_file_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"ジョブファイルハッシュ再検証",
        L"保存したジョブのハッシュ検証に失敗しました")));
  }

  file.reset();
  return clonecore::success_status();
}

clonecore::Status write_new_verified_job_result_log(
    const std::wstring& path,
    const std::span<const std::byte> bytes) {
  constexpr std::size_t kMaximumResultLogBytes = 64U * 1024U;
  if (!is_local_absolute_log_path(path) || bytes.empty() ||
      bytes.size() > kMaximumResultLogBytes) {
    return clonecore::Status::failure(job_file_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"WinPE実行結果ログ保存",
        L"保存先またはログ長が不正です"));
  }
  const auto parents = verify_output_parent_chain(
      path,
      L"WinPE結果保存先の親要素確認",
      L"親要素がディレクトリでないかreparse pointです");
  if (!parents) {
    return parents;
  }

  clonecore::UniqueHandle file(CreateFileW(
      path.c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
      nullptr));
  if (!file) {
    return failure_from_last_error(
        L"WinPE実行結果ログ新規作成", ERROR_FILE_EXISTS);
  }
  const auto fail_and_remove = [&file, &path](clonecore::Status status) {
    file.reset();
    static_cast<void>(DeleteFileW(path.c_str()));
    return status;
  };

  DWORD written{};
  if (!WriteFile(
          file.get(),
          bytes.data(),
          static_cast<DWORD>(bytes.size()),
          &written,
          nullptr)) {
    return fail_and_remove(failure_from_last_error(
        L"WinPE実行結果ログ書込み", ERROR_WRITE_FAULT));
  }
  if (static_cast<std::size_t>(written) != bytes.size()) {
    return fail_and_remove(clonecore::Status::failure(job_file_error(
        clonecore::ErrorCode::io_failed,
        ERROR_WRITE_FAULT,
        L"WinPE実行結果ログ書込み",
        L"実行結果ログを最後まで書き込めませんでした")));
  }
  if (!FlushFileBuffers(file.get())) {
    return fail_and_remove(failure_from_last_error(
        L"WinPE実行結果ログflush", ERROR_WRITE_FAULT));
  }

  LARGE_INTEGER observed_size{};
  if (!GetFileSizeEx(file.get(), &observed_size) ||
      observed_size.QuadPart < 0 ||
      static_cast<unsigned long long>(observed_size.QuadPart) !=
          bytes.size()) {
    return fail_and_remove(clonecore::Status::failure(job_file_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"WinPE実行結果ログサイズ確認",
        L"保存した結果ログのサイズが生成データと一致しません")));
  }
  LARGE_INTEGER beginning{};
  if (!SetFilePointerEx(file.get(), beginning, nullptr, FILE_BEGIN)) {
    return fail_and_remove(failure_from_last_error(
        L"WinPE実行結果ログ読戻し位置", ERROR_READ_FAULT));
  }
  std::vector<std::byte> observed(bytes.size());
  DWORD read{};
  if (!ReadFile(
          file.get(),
          observed.data(),
          static_cast<DWORD>(observed.size()),
          &read,
          nullptr)) {
    return fail_and_remove(failure_from_last_error(
        L"WinPE実行結果ログ読戻し", ERROR_READ_FAULT));
  }
  if (static_cast<std::size_t>(read) != observed.size() ||
      !std::equal(observed.begin(), observed.end(), bytes.begin())) {
    return fail_and_remove(clonecore::Status::failure(job_file_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"WinPE実行結果ログ読戻し検証",
        L"保存した結果ログが生成データと一致しません")));
  }

  file.reset();
  return clonecore::success_status();
}

clonecore::Result<std::wstring> claim_job_auto_execution_once(
    const std::wstring& job_path,
    const Sha256Digest& job_payload_hash) {
  auto claim_path = build_auto_claim_path(job_path, job_payload_hash);
  if (!claim_path) {
    return claim_path;
  }
  const auto parents = verify_output_parent_chain(
      claim_path.value(),
      L"WinPE一回限り自動実行記録の親要素確認",
      L"自動実行記録の親要素がディレクトリでないかreparse pointです");
  if (!parents) {
    return clonecore::Result<std::wstring>::failure(parents.error());
  }
  const std::vector<std::byte> bytes =
      build_auto_claim_bytes(job_payload_hash);
  clonecore::UniqueHandle file(CreateFileW(
      claim_path.value().c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
      nullptr));
  if (!file) {
    const DWORD native_code = GetLastError();
    if (native_code == ERROR_FILE_EXISTS ||
        native_code == ERROR_ALREADY_EXISTS) {
      return clonecore::Result<std::wstring>::failure(job_file_error(
          clonecore::ErrorCode::confirmation_required,
          native_code,
          L"WinPE一回限り自動実行記録",
          L"このジョブには既に実行開始記録があります。自動では再実行しません"));
    }
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"WinPE一回限り自動実行記録の新規作成",
            native_code));
  }

  const auto fail_closed = [&file](clonecore::Status status) {
    // The file is deliberately retained. Its mere existence prevents a
    // second automatic destructive attempt after power loss or short write.
    file.reset();
    return clonecore::Result<std::wstring>::failure(status.error());
  };

  DWORD written{};
  if (!WriteFile(
          file.get(),
          bytes.data(),
          static_cast<DWORD>(bytes.size()),
          &written,
          nullptr)) {
    return fail_closed(failure_from_last_error(
        L"WinPE一回限り自動実行記録の書込み",
        ERROR_WRITE_FAULT));
  }
  if (static_cast<std::size_t>(written) != bytes.size()) {
    return fail_closed(clonecore::Status::failure(job_file_error(
        clonecore::ErrorCode::io_failed,
        ERROR_WRITE_FAULT,
        L"WinPE一回限り自動実行記録の書込み",
        L"実行開始記録を最後まで書き込めませんでした")));
  }
  if (!FlushFileBuffers(file.get())) {
    return fail_closed(failure_from_last_error(
        L"WinPE一回限り自動実行記録のflush",
        ERROR_WRITE_FAULT));
  }
  LARGE_INTEGER beginning{};
  if (!SetFilePointerEx(file.get(), beginning, nullptr, FILE_BEGIN)) {
    return fail_closed(failure_from_last_error(
        L"WinPE一回限り自動実行記録の読戻し位置",
        ERROR_READ_FAULT));
  }
  std::vector<std::byte> observed(bytes.size());
  DWORD read{};
  if (!ReadFile(
          file.get(),
          observed.data(),
          static_cast<DWORD>(observed.size()),
          &read,
          nullptr)) {
    return fail_closed(failure_from_last_error(
        L"WinPE一回限り自動実行記録の読戻し",
        ERROR_READ_FAULT));
  }
  if (static_cast<std::size_t>(read) != observed.size() ||
      !std::equal(observed.begin(), observed.end(), bytes.begin())) {
    return fail_closed(clonecore::Status::failure(job_file_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"WinPE一回限り自動実行記録の読戻し検証",
        L"保存した実行開始記録が生成データと一致しません")));
  }
  file.reset();
  return clonecore::Result<std::wstring>::success(
      claim_path.take_value());
}

}  // namespace ytec::imageformat
