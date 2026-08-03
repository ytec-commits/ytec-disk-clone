#include "ytec/imageformat/job_file.h"
#include "ytec/imageformat/job_manifest.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

ytec::clonecore::StableDiskIdentity source_identity() {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = 0,
      .model = L"Y-TEC テスト元 SSD",
      .size_bytes = 128ULL * 1024U * 1024U * 1024U,
      .logical_sector_size = 512,
      .serial_suffix = "SRC01234",
      .device_instance_id = L"SCSI\\DISK&VEN_YTEC&PROD_SOURCE\\0",
      .is_system_disk = true,
  };
}

ytec::clonecore::StableDiskIdentity target_identity() {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = 4,
      .model = L"Y-TEC テスト先 SSD",
      .size_bytes = 256ULL * 1024U * 1024U * 1024U,
      .logical_sector_size = 512,
      .serial_suffix = "DST05678",
      .device_instance_id = L"SCSI\\DISK&VEN_YTEC&PROD_TARGET\\4",
      .is_system_disk = false,
  };
}

ytec::imageformat::JobManifest clone_job() {
  return ytec::imageformat::JobManifest{
      .schema_version = ytec::imageformat::kJobManifestSchemaVersion,
      .job_type = ytec::imageformat::JobType::clone,
      .source = source_identity(),
      .target = target_identity(),
      .image_path = {},
      .requested_conversion =
          ytec::imageformat::RequestedConversion::preserve,
      .created_utc = "2026-07-31T03:00:00Z",
      .app_version = "0.1.0-dev",
      .destructive_target_confirmed = true,
  };
}

std::string as_string(const std::vector<std::byte>& bytes) {
  std::string result(bytes.size(), '\0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    result[index] = static_cast<char>(
        std::to_integer<unsigned char>(bytes[index]));
  }
  return result;
}

std::wstring unique_job_test_path(const wchar_t* suffix) {
  std::vector<wchar_t> directory(32U * 1024U, L'\0');
  const DWORD length = GetTempPathW(
      static_cast<DWORD>(directory.size()), directory.data());
  check(
      length > 0 && length < directory.size(),
      "Temporary directory should be available");
  return std::wstring(directory.data(), length) +
         L"ytec-tsumugi-job-" +
         std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()) + L"-" + suffix + L".json";
}

class TemporaryFile final {
 public:
  explicit TemporaryFile(std::wstring path) : path_(std::move(path)) {
    static_cast<void>(DeleteFileW(path_.c_str()));
  }
  ~TemporaryFile() {
    static_cast<void>(DeleteFileW(path_.c_str()));
  }

  TemporaryFile(const TemporaryFile&) = delete;
  TemporaryFile& operator=(const TemporaryFile&) = delete;

  [[nodiscard]] const std::wstring& path() const noexcept {
    return path_;
  }

 private:
  std::wstring path_;
};

class ExistingTemporaryFile final {
 public:
  explicit ExistingTemporaryFile(std::wstring path)
      : path_(std::move(path)) {}
  ~ExistingTemporaryFile() {
    static_cast<void>(DeleteFileW(path_.c_str()));
  }

  ExistingTemporaryFile(const ExistingTemporaryFile&) = delete;
  ExistingTemporaryFile& operator=(const ExistingTemporaryFile&) = delete;

 private:
  std::wstring path_;
};

void test_clone_job_round_trip_is_deterministic() {
  const auto first =
      ytec::imageformat::serialize_hashed_job_manifest(clone_job());
  const auto second =
      ytec::imageformat::serialize_hashed_job_manifest(clone_job());
  check(first.has_value(), "Valid clone job should serialize");
  check(second.has_value(), "Repeated clone job should serialize");
  check(first.value() == second.value(), "Canonical output must be stable");

  const auto parsed =
      ytec::imageformat::parse_and_verify_hashed_job_manifest(
          first.value());
  check(parsed.has_value(), "Canonical clone job should verify");
  check(
      parsed.value().manifest.job_type ==
          ytec::imageformat::JobType::clone,
      "Job type must round trip");
  check(
      parsed.value().manifest.source->model ==
          source_identity().model,
      "UTF-8 model must round trip");
  check(
      parsed.value().manifest.target->disk_number == 4,
      "Target disk number must round trip");

  const std::string text = as_string(first.value());
  check(
      text.find("\"jobHashSha256\":\"") != std::string::npos,
      "Serialized job must contain its SHA-256");
  check(
      text.find("Y-TEC") != std::string::npos,
      "Payload should remain human-inspectable JSON");
}

void test_v4_transfer_mode_and_legacy_schemas_are_strict() {
  auto automatic = clone_job();
  automatic.execution_mode =
      ytec::imageformat::JobExecutionMode::auto_once;
  const auto automatic_bytes =
      ytec::imageformat::serialize_hashed_job_manifest(automatic);
  check(automatic_bytes.has_value(), "Auto-once clone should serialize");
  check(
      as_string(automatic_bytes.value()).find(
          "\"executionMode\":\"auto-once\"") != std::string::npos,
      "Current schema must bind execution mode into the hashed payload");
  check(
      as_string(automatic_bytes.value()).find(
          "\"transferMode\":\"exact\"") != std::string::npos,
      "Current schema must bind the transfer mode into the hashed payload");
  const auto automatic_parsed =
      ytec::imageformat::parse_and_verify_hashed_job_manifest(
          automatic_bytes.value());
  check(
      automatic_parsed.has_value() &&
          automatic_parsed.value().manifest.execution_mode ==
              ytec::imageformat::JobExecutionMode::auto_once,
      "Auto-once mode must round trip");

  auto previous = clone_job();
  previous.schema_version =
      ytec::imageformat::kPreviousJobManifestSchemaVersion;
  previous.execution_mode =
      ytec::imageformat::JobExecutionMode::auto_once;
  const auto previous_bytes =
      ytec::imageformat::serialize_hashed_job_manifest(previous);
  check(previous_bytes.has_value(), "Previous v3 job should remain readable");
  check(
      as_string(previous_bytes.value()).find("transferMode") ==
          std::string::npos,
      "Previous v3 canonical payload must not gain a v4 field");
  const auto previous_parsed =
      ytec::imageformat::parse_and_verify_hashed_job_manifest(
          previous_bytes.value());
  check(
      previous_parsed.has_value() &&
          previous_parsed.value().manifest.transfer_mode ==
              ytec::imageformat::TransferMode::exact &&
          previous_parsed.value().manifest.execution_mode ==
              ytec::imageformat::JobExecutionMode::auto_once,
      "Previous v3 jobs should map to exact mode without losing execution mode");

  auto legacy = clone_job();
  legacy.schema_version =
      ytec::imageformat::kLegacyJobManifestSchemaVersion;
  const auto legacy_bytes =
      ytec::imageformat::serialize_hashed_job_manifest(legacy);
  check(legacy_bytes.has_value(), "Legacy v2 job should remain readable");
  check(
      as_string(legacy_bytes.value()).find("executionMode") ==
          std::string::npos,
      "Legacy canonical payload must not gain a v3 field");
  const auto legacy_parsed =
      ytec::imageformat::parse_and_verify_hashed_job_manifest(
          legacy_bytes.value());
  check(
      legacy_parsed.has_value() &&
          legacy_parsed.value().manifest.execution_mode ==
              ytec::imageformat::JobExecutionMode::review_required,
      "Legacy jobs must always require WinPE review");

  legacy.execution_mode =
      ytec::imageformat::JobExecutionMode::auto_once;
  check(
      !ytec::imageformat::serialize_hashed_job_manifest(legacy)
           .has_value(),
      "Legacy schema must not smuggle auto execution");
}

void test_shrink_clone_allows_smaller_target_but_exact_does_not() {
  auto job = clone_job();
  job.target->size_bytes = 64ULL * 1024U * 1024U * 1024U;
  check(
      !ytec::imageformat::serialize_hashed_job_manifest(job).has_value(),
      "Exact mode must continue rejecting a smaller target");
  job.transfer_mode = ytec::imageformat::TransferMode::shrink;
  job.image_path = L"D:\\Tsumugi-work\\clone-staging.dcmig";
  const auto encoded =
      ytec::imageformat::serialize_hashed_job_manifest(job);
  check(encoded.has_value(), "Shrink mode should allow a smaller target job");
  const auto parsed =
      ytec::imageformat::parse_and_verify_hashed_job_manifest(encoded.value());
  check(
      parsed.has_value() &&
          parsed.value().manifest.transfer_mode ==
              ytec::imageformat::TransferMode::shrink,
      "Shrink mode must round-trip inside the job hash");

  job.requested_conversion =
      ytec::imageformat::RequestedConversion::mbr_to_gpt;
  check(
      !ytec::imageformat::serialize_hashed_job_manifest(job).has_value(),
      "Shrink mode must not silently combine with MBR-to-GPT conversion");
}

void test_payload_mutation_is_rejected() {
  auto serialized =
      ytec::imageformat::serialize_hashed_job_manifest(clone_job());
  check(serialized.has_value(), "Fixture should serialize");
  std::string text = as_string(serialized.value());
  const std::size_t marker = text.find("\"sizeBytes\":");
  check(marker != std::string::npos, "Fixture size field must exist");
  const auto digit = std::find_if(
      serialized.value().begin() +
          static_cast<std::ptrdiff_t>(marker),
      serialized.value().end(),
      [](const std::byte value) {
        const char character = static_cast<char>(
            std::to_integer<unsigned char>(value));
        return character >= '0' && character <= '9';
      });
  check(digit != serialized.value().end(), "Fixture size digit must exist");
  *digit = *digit == std::byte{'9'} ? std::byte{'8'} : std::byte{'9'};

  const auto parsed =
      ytec::imageformat::parse_and_verify_hashed_job_manifest(
          serialized.value());
  check(!parsed.has_value(), "Mutated payload must fail");
  check(
      parsed.error().code ==
          ytec::clonecore::ErrorCode::verification_failed,
      "Mutated canonical payload must fail as a hash mismatch");
}

void test_noncanonical_json_is_rejected() {
  auto serialized =
      ytec::imageformat::serialize_hashed_job_manifest(clone_job());
  check(serialized.has_value(), "Fixture should serialize");
  serialized.value().push_back(std::byte{'\n'});
  const auto parsed =
      ytec::imageformat::parse_and_verify_hashed_job_manifest(
          serialized.value());
  check(!parsed.has_value(), "Trailing whitespace must fail closed");
}

void test_confirmation_and_system_target_are_required() {
  auto job = clone_job();
  job.destructive_target_confirmed = false;
  const auto missing_confirmation =
      ytec::imageformat::serialize_hashed_job_manifest(job);
  check(
      !missing_confirmation.has_value(),
      "Destructive job without confirmation must not serialize");

  auto restore_target = target_identity();
  restore_target.is_system_disk = true;
  ytec::imageformat::JobManifest restore_job{
      .schema_version = ytec::imageformat::kJobManifestSchemaVersion,
      .job_type = ytec::imageformat::JobType::restore_image,
      .source = std::nullopt,
      .target = restore_target,
      .image_path = L"D:\\Backups\\system.dcimg",
      .restore_image_identity =
          ytec::imageformat::RestoreImageIdentity{
              .length_bytes = 4096,
              .global_hash = [] {
                ytec::imageformat::Sha256Digest digest{};
                digest[0] = std::byte{0x42};
                return digest;
              }(),
          },
      .requested_conversion =
          ytec::imageformat::RequestedConversion::preserve,
      .created_utc = "2026-07-31T03:01:00Z",
      .app_version = "0.1.0-dev",
      .destructive_target_confirmed = true,
  };
  const auto system_restore =
      ytec::imageformat::serialize_hashed_job_manifest(restore_job);
  check(
      !system_restore.has_value(),
      "Running Windows disk must not be a restore target");

  restore_target.is_system_disk = false;
  restore_job.target = restore_target;
  restore_job.restore_image_identity.reset();
  const auto missing_image_identity =
      ytec::imageformat::serialize_hashed_job_manifest(restore_job);
  check(
      !missing_image_identity.has_value(),
      "Restore job without an image fingerprint must not serialize");
}

void test_image_job_requires_local_absolute_path() {
  ytec::imageformat::JobManifest job{
      .schema_version = ytec::imageformat::kJobManifestSchemaVersion,
      .job_type = ytec::imageformat::JobType::create_image,
      .source = source_identity(),
      .target = std::nullopt,
      .image_path = L"D:\\バックアップ\\システム.dcimg",
      .requested_conversion =
          ytec::imageformat::RequestedConversion::preserve,
      .created_utc = "2026-07-31T03:02:00Z",
      .app_version = "0.1.0-dev",
      .destructive_target_confirmed = false,
  };
  const auto valid =
      ytec::imageformat::serialize_hashed_job_manifest(job);
  check(valid.has_value(), "Local image path should serialize");
  check(
      ytec::imageformat::parse_and_verify_hashed_job_manifest(
          valid.value())
          .has_value(),
      "Local image job should verify");

  job.image_path = L"\\\\server\\share\\system.dcimg";
  const auto network =
      ytec::imageformat::serialize_hashed_job_manifest(job);
  check(
      !network.has_value(),
      "Network path must be rejected by the local-only v1 job format");
}

void test_size_and_utf8_boundaries_fail_closed() {
  const std::vector<std::byte> oversized(
      ytec::imageformat::kMaximumJobManifestBytes + 1,
      std::byte{'A'});
  const auto oversized_result =
      ytec::imageformat::parse_and_verify_hashed_job_manifest(oversized);
  check(!oversized_result.has_value(), "Oversized job must be rejected");

  auto serialized =
      ytec::imageformat::serialize_hashed_job_manifest(clone_job());
  check(serialized.has_value(), "Fixture should serialize");
  const std::string text = as_string(serialized.value());
  const std::size_t model = text.find("Y-TEC ");
  check(model != std::string::npos, "Fixture model must exist");
  serialized.value()[model + 6] = std::byte{0xFF};
  const auto invalid_utf8 =
      ytec::imageformat::parse_and_verify_hashed_job_manifest(
          serialized.value());
  check(!invalid_utf8.has_value(), "Invalid UTF-8 must be rejected");
}

void test_job_file_is_new_only_and_verified() {
  const auto serialized =
      ytec::imageformat::serialize_hashed_job_manifest(clone_job());
  check(serialized.has_value(), "Fixture should serialize");
  TemporaryFile file(unique_job_test_path(L"new-only"));

  const auto first = ytec::imageformat::write_new_verified_job_file(
      file.path(), serialized.value());
  check(first.has_value(), "Valid job should save to a new file");

  const auto second = ytec::imageformat::write_new_verified_job_file(
      file.path(), serialized.value());
  check(!second.has_value(), "Existing job file must not be replaced");

  WIN32_FILE_ATTRIBUTE_DATA attributes{};
  check(
      GetFileAttributesExW(
          file.path().c_str(),
          GetFileExInfoStandard,
          &attributes) != FALSE,
      "Saved job file should still exist");
  ULARGE_INTEGER size{};
  size.HighPart = attributes.nFileSizeHigh;
  size.LowPart = attributes.nFileSizeLow;
  check(
      size.QuadPart == serialized.value().size(),
      "Failed overwrite attempt must preserve the original job");
}

void test_invalid_job_is_not_created() {
  auto serialized =
      ytec::imageformat::serialize_hashed_job_manifest(clone_job());
  check(serialized.has_value(), "Fixture should serialize");
  serialized.value()[0] = std::byte{'['};
  TemporaryFile file(unique_job_test_path(L"invalid"));

  const auto result = ytec::imageformat::write_new_verified_job_file(
      file.path(), serialized.value());
  check(!result.has_value(), "Invalid job must not be written");
  const DWORD attributes = GetFileAttributesW(file.path().c_str());
  const DWORD native_code = GetLastError();
  check(
      attributes == INVALID_FILE_ATTRIBUTES &&
          (native_code == ERROR_FILE_NOT_FOUND ||
           native_code == ERROR_PATH_NOT_FOUND),
      "Invalid job must leave no output file");
}

void test_auto_once_claim_is_create_new_and_hash_bound() {
  auto automatic = clone_job();
  automatic.execution_mode =
      ytec::imageformat::JobExecutionMode::auto_once;
  const auto serialized =
      ytec::imageformat::serialize_hashed_job_manifest(automatic);
  check(serialized.has_value(), "Auto-once fixture should serialize");
  const auto verified =
      ytec::imageformat::parse_and_verify_hashed_job_manifest(
          serialized.value());
  check(verified.has_value(), "Auto-once fixture should verify");
  TemporaryFile job_path(unique_job_test_path(L"auto-claim-source"));

  const auto first =
      ytec::imageformat::claim_job_auto_execution_once(
          job_path.path(), verified.value().payload_hash);
  check(first.has_value(), "First auto-once claim should succeed");
  ExistingTemporaryFile claim_cleanup(first.value());
  check(
      first.value().find(L".auto-once-") != std::wstring::npos &&
          first.value().ends_with(L".claim"),
      "Claim path should be visibly scoped to auto-once execution");

  const auto second =
      ytec::imageformat::claim_job_auto_execution_once(
          job_path.path(), verified.value().payload_hash);
  check(!second.has_value(), "Same job must never be auto-claimed twice");
  check(
      second.error().code ==
          ytec::clonecore::ErrorCode::confirmation_required,
      "Existing claim should return to explicit manual confirmation");

  const auto invalid =
      ytec::imageformat::claim_job_auto_execution_once(
          L"relative-job.json", verified.value().payload_hash);
  check(!invalid.has_value(), "Relative claim source path must fail closed");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests{
      {"clone job round trip", test_clone_job_round_trip_is_deterministic},
      {"v4 transfer mode and legacy schemas",
       test_v4_transfer_mode_and_legacy_schemas_are_strict},
      {"shrink clone smaller target",
       test_shrink_clone_allows_smaller_target_but_exact_does_not},
      {"payload mutation", test_payload_mutation_is_rejected},
      {"canonical JSON", test_noncanonical_json_is_rejected},
      {"confirmation safety", test_confirmation_and_system_target_are_required},
      {"image path", test_image_job_requires_local_absolute_path},
      {"size and UTF-8", test_size_and_utf8_boundaries_fail_closed},
      {"new-only job file", test_job_file_is_new_only_and_verified},
      {"invalid job file", test_invalid_job_is_not_created},
      {"auto-once claim", test_auto_once_claim_is_create_new_and_hash_bound},
  };

  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "[PASS] " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "[FAIL] " << name << ": " << failure.message << '\n';
    }
  }
  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << tests.size() << " test(s) passed\n";
  return 0;
}
