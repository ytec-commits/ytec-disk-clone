#pragma once

#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/result.h"
#include "ytec/imageformat/sha256.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ytec::imageformat {

inline constexpr std::uint32_t kLegacyJobManifestSchemaVersion = 2;
inline constexpr std::uint32_t kPreviousJobManifestSchemaVersion = 3;
inline constexpr std::uint32_t kJobManifestSchemaVersion = 4;
inline constexpr std::size_t kMaximumJobManifestBytes = 64U * 1024U;

enum class JobType : std::uint8_t {
  clone,
  create_image,
  restore_image,
  mbr_to_gpt,
};

enum class RequestedConversion : std::uint8_t {
  preserve,
  mbr_to_gpt,
};

enum class JobExecutionMode : std::uint8_t {
  review_required,
  auto_once,
};

enum class TransferMode : std::uint8_t {
  exact,
  shrink,
};

struct RestoreImageIdentity final {
  std::uint64_t length_bytes{};
  Sha256Digest global_hash{};
};

struct JobManifest final {
  std::uint32_t schema_version{kJobManifestSchemaVersion};
  JobType job_type{JobType::clone};
  std::optional<clonecore::StableDiskIdentity> source;
  std::optional<clonecore::StableDiskIdentity> target;
  std::wstring image_path;
  std::optional<RestoreImageIdentity> restore_image_identity;
  RequestedConversion requested_conversion{RequestedConversion::preserve};
  TransferMode transfer_mode{TransferMode::exact};
  std::string created_utc;
  std::string app_version;
  JobExecutionMode execution_mode{JobExecutionMode::review_required};
  bool destructive_target_confirmed{};
};

struct VerifiedJobManifest final {
  JobManifest manifest;
  Sha256Digest payload_hash{};
};

// Produces strict canonical UTF-8 JSON. The SHA-256 covers the complete
// canonical payload object and is stored in the outer object. The output does
// not contain passwords, recovery keys, product keys, or unmasked serials.
[[nodiscard]] clonecore::Result<std::vector<std::byte>>
serialize_hashed_job_manifest(const JobManifest& manifest);

// Treats the file as untrusted. Only the exact v2 legacy, v3 previous, or v4
// current canonical field order and spelling are accepted; unknown fields,
// invalid UTF-8, trailing bytes, noncanonical encodings, and hash mismatches
// fail closed. Legacy v2 jobs always map to review-required execution, and
// v2/v3 jobs map to exact transfer mode.
[[nodiscard]] clonecore::Result<VerifiedJobManifest>
parse_and_verify_hashed_job_manifest(std::span<const std::byte> json);

[[nodiscard]] std::string_view job_type_name(JobType type) noexcept;

[[nodiscard]] std::string_view requested_conversion_name(
    RequestedConversion conversion) noexcept;

[[nodiscard]] std::string_view job_execution_mode_name(
    JobExecutionMode mode) noexcept;

[[nodiscard]] std::string_view transfer_mode_name(
    TransferMode mode) noexcept;

}  // namespace ytec::imageformat
