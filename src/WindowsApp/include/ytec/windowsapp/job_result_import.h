#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/imageformat/job_result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::windowsapp {

inline constexpr std::size_t kMaximumImportedJobResults = 256U;

class IJobResultCandidateProvider {
 public:
  virtual ~IJobResultCandidateProvider() = default;
  [[nodiscard]] virtual clonecore::Result<std::vector<std::wstring>>
  candidates() = 0;
};

class IJobResultLoader {
 public:
  virtual ~IJobResultLoader() = default;
  [[nodiscard]] virtual clonecore::Result<std::vector<std::byte>> load(
      const std::wstring& path) = 0;
};

struct ImportedJobResult final {
  std::wstring path;
  std::wstring file_name;
  imageformat::JobResultRecord record;
};

// Produces the fixed root/Tsumugi directories for C: through Z:, excluding
// WinPE's X:. Directory traversal and recursive search are never used.
[[nodiscard]] std::vector<std::wstring> build_job_result_search_directories(
    std::uint32_t logical_drive_mask);

// Accepts only product-created clone/restore result names with an exact
// YYYYMMDD-HHMMSSZ timestamp.
[[nodiscard]] bool is_product_job_result_file_name(
    std::wstring_view file_name) noexcept;

// Strictly verifies every bounded candidate and returns newest first. One
// unreadable, duplicate, or invalid fixed-name result fails the import rather
// than being silently ignored.
[[nodiscard]] clonecore::Result<std::vector<ImportedJobResult>>
import_verified_job_results(
    IJobResultCandidateProvider& candidate_provider,
    IJobResultLoader& loader);

[[nodiscard]] std::unique_ptr<IJobResultCandidateProvider>
make_windows_job_result_candidate_provider();

[[nodiscard]] std::unique_ptr<IJobResultLoader>
make_windows_job_result_loader();

}  // namespace ytec::windowsapp
