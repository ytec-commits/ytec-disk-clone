#include "ytec/winpeapp/app_runner.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::winpeapp {
namespace {

clonecore::Error loader_error(
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

bool is_local_absolute_file_path(const std::wstring_view path) {
  std::size_t colon_index{};
  if (path.size() >= 3 &&
      ((path[0] >= L'A' && path[0] <= L'Z') ||
       (path[0] >= L'a' && path[0] <= L'z')) &&
      path[1] == L':' && path[2] == L'\\') {
    colon_index = 1;
  } else if (
      path.size() >= 7 && path.substr(0, 4) == L"\\\\?\\" &&
      ((path[4] >= L'A' && path[4] <= L'Z') ||
       (path[4] >= L'a' && path[4] <= L'z')) &&
      path[5] == L':' && path[6] == L'\\') {
    colon_index = 5;
  } else {
    return false;
  }
  return path.find(L':', colon_index + 1) == std::wstring_view::npos &&
         path.find(L'/') == std::wstring_view::npos &&
         path.back() != L'\\';
}

class WindowsJobManifestLoader final : public IJobManifestLoader {
 public:
  clonecore::Result<std::vector<std::byte>> load(
      const std::wstring& path) override {
    if (!is_local_absolute_file_path(path)) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          loader_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_BAD_PATHNAME,
              L"WinPEジョブファイルパス",
              L"ローカルドライブ上の絶対ファイルパスだけを指定できます"));
    }

    clonecore::UniqueHandle file(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!file) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"WinPEジョブファイル読取り開始",
              GetLastError()));
    }

    FILE_ATTRIBUTE_TAG_INFO tag_info{};
    if (!GetFileInformationByHandleEx(
            file.get(),
            FileAttributeTagInfo,
            &tag_info,
            sizeof(tag_info))) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"WinPEジョブファイル属性確認",
              GetLastError()));
    }
    if ((tag_info.FileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          loader_error(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_REPARSE_TAG_INVALID,
              L"WinPEジョブファイル属性確認",
              L"ディレクトリまたはreparse pointは使用できません"));
    }

    LARGE_INTEGER initial_size{};
    if (!GetFileSizeEx(file.get(), &initial_size)) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"WinPEジョブファイル長確認",
              GetLastError()));
    }
    if (initial_size.QuadPart <= 0 ||
        static_cast<std::uint64_t>(initial_size.QuadPart) >
            imageformat::kMaximumJobManifestBytes) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          loader_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_FILE_TOO_LARGE,
              L"WinPEジョブファイル長確認",
              L"ジョブファイルが空か、許容上限を超えています"));
    }

    const std::size_t size =
        static_cast<std::size_t>(initial_size.QuadPart);
    std::vector<std::byte> bytes(size);
    std::size_t offset{};
    while (offset < bytes.size()) {
      const DWORD requested = static_cast<DWORD>(
          std::min<std::size_t>(
              bytes.size() - offset,
              (std::numeric_limits<DWORD>::max)()));
      DWORD read{};
      if (!ReadFile(
              file.get(),
              bytes.data() + offset,
              requested,
              &read,
              nullptr)) {
        return clonecore::Result<std::vector<std::byte>>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::io_failed,
                L"WinPEジョブファイル読取り",
                GetLastError()));
      }
      if (read == 0) {
        return clonecore::Result<std::vector<std::byte>>::failure(
            loader_error(
                clonecore::ErrorCode::verification_failed,
                ERROR_HANDLE_EOF,
                L"WinPEジョブファイル読取り",
                L"確認済みファイル長より前で終端しました"));
      }
      offset += read;
    }

    LARGE_INTEGER final_size{};
    if (!GetFileSizeEx(file.get(), &final_size) ||
        final_size.QuadPart != initial_size.QuadPart) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          loader_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_FILE_INVALID,
              L"WinPEジョブファイル再確認",
              L"読取り中にファイル長が変化しました"));
    }
    return clonecore::Result<std::vector<std::byte>>::success(
        std::move(bytes));
  }
};

class HashLockedJobManifestLoader final : public IJobManifestLoader {
 public:
  HashLockedJobManifestLoader(
      std::unique_ptr<IJobManifestLoader> inner,
      const imageformat::Sha256Digest& expected_payload_hash)
      : inner_(std::move(inner)),
        expected_payload_hash_(expected_payload_hash) {}

  clonecore::Result<std::vector<std::byte>> load(
      const std::wstring& path) override {
    if (inner_ == nullptr) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          loader_error(
              clonecore::ErrorCode::internal_error,
              ERROR_INVALID_HANDLE,
              L"WinPE予約ジョブ実行時固定",
              L"ジョブ読取りサービスがありません"));
    }
    auto bytes = inner_->load(path);
    if (!bytes) {
      return bytes;
    }
    auto verified = imageformat::parse_and_verify_hashed_job_manifest(
        bytes.value());
    if (!verified) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          verified.error());
    }
    if (verified.value().payload_hash != expected_payload_hash_) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          loader_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_REVISION_MISMATCH,
              L"WinPE予約ジョブ実行時固定",
              L"安全確認後にジョブ内容が変化しました。再確認が必要です"));
    }
    return bytes;
  }

 private:
  std::unique_ptr<IJobManifestLoader> inner_;
  imageformat::Sha256Digest expected_payload_hash_{};
};

}  // namespace

std::unique_ptr<IJobManifestLoader> make_windows_job_manifest_loader() {
  return std::make_unique<WindowsJobManifestLoader>();
}

std::unique_ptr<IJobManifestLoader> make_hash_locked_job_manifest_loader(
    std::unique_ptr<IJobManifestLoader> inner,
    const imageformat::Sha256Digest& expected_payload_hash) {
  return std::make_unique<HashLockedJobManifestLoader>(
      std::move(inner), expected_payload_hash);
}

}  // namespace ytec::winpeapp
