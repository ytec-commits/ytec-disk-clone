#include "ytec/winpeapp/app_runner.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::winpeapp {
namespace {

constexpr std::size_t kMaximumJobManifestCandidates = 92U;
constexpr std::array<std::wstring_view, 4> kCandidateSuffixes{
    L"\\Tsumugi\\Tsumugi-clone-job.json",
    L"\\Tsumugi\\Tsumugi-restore-job.json",
    L"\\Tsumugi-clone-job.json",
    L"\\Tsumugi-restore-job.json",
};

clonecore::Error candidate_error(
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

class WindowsJobManifestCandidateProvider final
    : public IJobManifestCandidateProvider {
 public:
  clonecore::Result<std::vector<std::wstring>> candidates() override {
    SetLastError(ERROR_SUCCESS);
    const DWORD logical_drives = GetLogicalDrives();
    if (logical_drives == 0U && GetLastError() != ERROR_SUCCESS) {
      return clonecore::Result<std::vector<std::wstring>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::enumeration_failed,
              L"WinPE予約ジョブ候補ドライブ列挙",
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

    std::vector<std::wstring> existing;
    for (const auto& path :
         build_job_manifest_candidate_paths(eligible_mask)) {
      SetLastError(ERROR_SUCCESS);
      const DWORD attributes = GetFileAttributesW(path.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD native_code = GetLastError();
        if (native_code == ERROR_FILE_NOT_FOUND ||
            native_code == ERROR_PATH_NOT_FOUND) {
          continue;
        }
        return clonecore::Result<std::vector<std::wstring>>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::query_failed,
                L"WinPE予約ジョブ候補属性確認",
                native_code));
      }
      if ((attributes &
           (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        return clonecore::Result<std::vector<std::wstring>>::failure(
            candidate_error(
                clonecore::ErrorCode::unsupported_layout,
                ERROR_REPARSE_TAG_INVALID,
                L"WinPE予約ジョブ候補属性確認",
                L"固定候補名にディレクトリまたはreparse pointがあります"));
      }
      existing.push_back(path);
    }
    return clonecore::Result<std::vector<std::wstring>>::success(
        std::move(existing));
  }
};

}  // namespace

std::vector<std::wstring> build_job_manifest_candidate_paths(
    const std::uint32_t logical_drive_mask) {
  std::vector<std::wstring> paths;
  paths.reserve(kMaximumJobManifestCandidates);
  for (wchar_t letter = L'C'; letter <= L'Z'; ++letter) {
    if (letter == L'X') {
      continue;
    }
    const std::uint32_t bit =
        1U << static_cast<std::uint32_t>(letter - L'A');
    if ((logical_drive_mask & bit) == 0U) {
      continue;
    }
    for (const auto suffix : kCandidateSuffixes) {
      std::wstring path(1U, letter);
      path += L":";
      path += suffix;
      paths.push_back(std::move(path));
    }
  }
  return paths;
}

clonecore::Result<std::optional<DiscoveredJobManifest>>
discover_unique_job_manifest(
    IJobManifestCandidateProvider& candidate_provider,
    IJobManifestLoader& loader) {
  auto candidates = candidate_provider.candidates();
  if (!candidates) {
    return clonecore::Result<std::optional<DiscoveredJobManifest>>::failure(
        candidates.error());
  }
  if (candidates.value().size() > kMaximumJobManifestCandidates) {
    return clonecore::Result<std::optional<DiscoveredJobManifest>>::failure(
        candidate_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_BUFFER_OVERFLOW,
            L"WinPE予約ジョブ候補数",
            L"予約ジョブ候補が安全上限を超えています"));
  }

  for (std::size_t index = 0; index < candidates.value().size(); ++index) {
    if (candidates.value()[index].empty()) {
      return clonecore::Result<std::optional<DiscoveredJobManifest>>::failure(
          candidate_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_BAD_PATHNAME,
              L"WinPE予約ジョブ候補",
              L"空の候補パスは使用できません"));
    }
    for (std::size_t earlier = 0; earlier < index; ++earlier) {
      if (equals_ordinal_ignore_case(
              candidates.value()[earlier], candidates.value()[index])) {
        return clonecore::Result<std::optional<DiscoveredJobManifest>>::failure(
            candidate_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_DUP_NAME,
                L"WinPE予約ジョブ候補",
                L"同じ予約ジョブ候補が重複しています"));
      }
    }
  }

  std::optional<DiscoveredJobManifest> discovered;
  for (const auto& path : candidates.value()) {
    auto bytes = loader.load(path);
    if (!bytes) {
      return clonecore::Result<std::optional<DiscoveredJobManifest>>::failure(
          bytes.error());
    }
    auto verified = imageformat::parse_and_verify_hashed_job_manifest(
        bytes.value());
    if (!verified) {
      return clonecore::Result<std::optional<DiscoveredJobManifest>>::failure(
          verified.error());
    }
    if (discovered.has_value()) {
      return clonecore::Result<std::optional<DiscoveredJobManifest>>::failure(
          candidate_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_DUP_NAME,
              L"WinPE予約ジョブ自動検出",
              L"有効な予約ジョブが複数あります。自動選択せず停止しました"));
    }
    discovered = DiscoveredJobManifest{
        .path = path,
        .verified_job = verified.take_value(),
    };
  }

  return clonecore::Result<std::optional<DiscoveredJobManifest>>::success(
      std::move(discovered));
}

std::unique_ptr<IJobManifestCandidateProvider>
make_windows_job_manifest_candidate_provider() {
  return std::make_unique<WindowsJobManifestCandidateProvider>();
}

}  // namespace ytec::winpeapp
