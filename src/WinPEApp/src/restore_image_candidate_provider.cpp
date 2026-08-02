#include "ytec/winpeapp/app_runner.h"

#include <Windows.h>

#include <algorithm>
#include <memory>
#include <utility>

namespace ytec::winpeapp {
namespace {

class WindowsRestoreImageCandidateProvider final
    : public IRestoreImageCandidateProvider {
 public:
  clonecore::Result<std::vector<std::wstring>> candidates_for(
      const std::wstring& configured_path) override {
    SetLastError(ERROR_SUCCESS);
    const DWORD drive_mask = GetLogicalDrives();
    if (drive_mask == 0) {
      const DWORD native_code = GetLastError();
      return clonecore::Result<std::vector<std::wstring>>::failure(
          clonecore::Error{
              .code = clonecore::ErrorCode::query_failed,
              .native_code =
                  native_code == ERROR_SUCCESS
                      ? ERROR_GEN_FAILURE
                      : native_code,
              .operation = L"WinPE復元イメージ候補ドライブの列挙",
              .message = L"利用可能なローカルドライブを列挙できません",
          });
    }

    std::vector<std::wstring> candidates =
        build_restore_image_candidate_paths(configured_path, drive_mask);
    std::erase_if(
        candidates,
        [](const std::wstring& candidate) {
          const std::wstring root = candidate.substr(0, 3);
          const UINT drive_type = GetDriveTypeW(root.c_str());
          if (drive_type != DRIVE_FIXED &&
              drive_type != DRIVE_REMOVABLE) {
            return true;
          }
          const DWORD attributes = GetFileAttributesW(candidate.c_str());
          return attributes == INVALID_FILE_ATTRIBUTES ||
                 (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        });
    return clonecore::Result<std::vector<std::wstring>>::success(
        std::move(candidates));
  }
};

}  // namespace

std::vector<std::wstring> build_restore_image_candidate_paths(
    const std::wstring& configured_path,
    const std::uint32_t logical_drive_mask) {
  if (configured_path.size() < 4 ||
      configured_path[1] != L':' ||
      (configured_path[2] != L'\\' &&
       configured_path[2] != L'/')) {
    return {};
  }

  std::wstring suffix = configured_path.substr(2);
  suffix[0] = L'\\';
  std::vector<std::wstring> candidates;
  candidates.reserve(23);
  for (std::uint32_t index = 2; index < 26; ++index) {
    const wchar_t drive_letter =
        static_cast<wchar_t>(L'A' + index);
    if (drive_letter == L'X' ||
        (logical_drive_mask & (1U << index)) == 0) {
      continue;
    }
    std::wstring candidate;
    candidate.reserve(2 + suffix.size());
    candidate.push_back(drive_letter);
    candidate.push_back(L':');
    candidate.append(suffix);
    candidates.push_back(std::move(candidate));
  }
  return candidates;
}

std::unique_ptr<IRestoreImageCandidateProvider>
make_windows_restore_image_candidate_provider() {
  return std::make_unique<WindowsRestoreImageCandidateProvider>();
}

}  // namespace ytec::winpeapp
