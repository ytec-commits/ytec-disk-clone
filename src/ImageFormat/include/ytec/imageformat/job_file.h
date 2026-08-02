#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/imageformat/sha256.h"

#include <cstddef>
#include <span>
#include <string>

namespace ytec::imageformat {

// Saves a verified job manifest as a new local file. Existing files are never
// replaced. Product code uses this narrow writer for job JSON only; it cannot
// open a physical disk or volume.
[[nodiscard]] clonecore::Status write_new_verified_job_file(
    const std::wstring& path,
    std::span<const std::byte> bytes);

// Saves one bounded WinPE job-result log beside a job. The file must be a new
// local .log, existing files are never replaced, and every byte is read back
// through the same handle before success is returned.
[[nodiscard]] clonecore::Status write_new_verified_job_result_log(
    const std::wstring& path,
    std::span<const std::byte> bytes);

// Atomically claims one explicitly auto-once job before destructive work.
// The claim name and body are bound to the verified payload SHA-256. CREATE_NEW
// makes an existing or partially created claim a permanent fail-closed stop;
// a failed claim is intentionally never deleted automatically.
[[nodiscard]] clonecore::Result<std::wstring>
claim_job_auto_execution_once(
    const std::wstring& job_path,
    const Sha256Digest& job_payload_hash);

}  // namespace ytec::imageformat
