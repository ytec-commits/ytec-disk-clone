#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/imageformat/job_manifest.h"
#include "ytec/imageformat/sha256.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ytec::imageformat {

inline constexpr std::size_t kMaximumJobResultLogBytes = 64U * 1024U;

enum class JobResultOutcome : std::uint8_t {
  passed,
  failed,
};

struct JobResultRecord final {
  Sha256Digest job_payload_hash{};
  JobType job_type{JobType::clone};
  JobResultOutcome outcome{JobResultOutcome::failed};
  std::string completed_utc;
  std::string app_version;
  std::string details_utf8;
};

// Produces one canonical, bounded UTF-8 result log. The detail body is
// normalized to one trailing LF and protected by detailsSha256.
[[nodiscard]] clonecore::Result<std::vector<std::byte>>
serialize_job_result_log(const JobResultRecord& record);

// Strictly verifies field order, spelling, canonical values, UTF-8, declared
// detail length/SHA-256, and exact reserialization. Unknown or trailing data is
// rejected.
[[nodiscard]] clonecore::Result<JobResultRecord>
parse_and_verify_job_result_log(std::span<const std::byte> bytes);

}  // namespace ytec::imageformat
