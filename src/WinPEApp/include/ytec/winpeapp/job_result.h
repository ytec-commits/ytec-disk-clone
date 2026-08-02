#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/imageformat/job_result.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace ytec::winpeapp {

inline constexpr std::size_t kMaximumJobResultLogBytes =
    imageformat::kMaximumJobResultLogBytes;
using JobResultOutcome = imageformat::JobResultOutcome;
using JobResultRecord = imageformat::JobResultRecord;

// Produces a bounded UTF-8 diagnostic log with the originating job hash and a
// SHA-256 of the detail body. Paths, serials, and credentials are not added by
// the serializer; callers remain responsible for supplying safe details.
[[nodiscard]] clonecore::Result<std::vector<std::byte>>
serialize_job_result_log(const JobResultRecord& record);

// Places a timestamped result beside the originating .json job. The job path
// is never replaced. The timestamp must be YYYYMMDD-HHMMSSZ.
[[nodiscard]] clonecore::Result<std::wstring> build_job_result_log_path(
    const std::wstring& job_path,
    const std::wstring& compact_utc);

// Creates a new local .log only, flushes it, and verifies every byte through
// the same handle. Existing files are never replaced. On failure, only a file
// created by the current call is removed.
[[nodiscard]] clonecore::Status write_new_job_result_log(
    const std::wstring& path,
    std::span<const std::byte> bytes);

}  // namespace ytec::winpeapp
