#include "ytec/windowsapp/job_result_import.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iostream>
#include <optional>
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

class MockCandidateProvider final
    : public ytec::windowsapp::IJobResultCandidateProvider {
 public:
  ytec::clonecore::Result<std::vector<std::wstring>> candidates() override {
    ++call_count;
    return ytec::clonecore::Result<std::vector<std::wstring>>::success(paths);
  }

  int call_count{};
  std::vector<std::wstring> paths;
};

class SequencedLoader final : public ytec::windowsapp::IJobResultLoader {
 public:
  ytec::clonecore::Result<std::vector<std::byte>> load(
      const std::wstring& path) override {
    received_paths.push_back(path);
    const std::size_t index = call_count++;
    if (index >= responses.size()) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          ytec::clonecore::Error{
              .code = ytec::clonecore::ErrorCode::query_failed,
              .native_code = ERROR_FILE_NOT_FOUND,
              .operation = L"モック結果ログ",
              .message = L"応答がありません",
          });
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        responses[index]);
  }

  std::size_t call_count{};
  std::vector<std::wstring> received_paths;
  std::vector<std::vector<std::byte>> responses;
};

std::vector<std::byte> result_bytes(
    const std::string& completed_utc,
    const ytec::imageformat::JobType type,
    const ytec::imageformat::JobResultOutcome outcome) {
  ytec::imageformat::Sha256Digest job_hash{};
  job_hash[0] = type == ytec::imageformat::JobType::clone
      ? std::byte{0xC1}
      : std::byte{0xA2};
  const auto serialized = ytec::imageformat::serialize_job_result_log(
      ytec::imageformat::JobResultRecord{
          .job_payload_hash = job_hash,
          .job_type = type,
          .outcome = outcome,
          .completed_utc = completed_utc,
          .app_version = "0.2.0",
          .details_utf8 = "verified result\n",
      });
  check(serialized.has_value(), "Result fixture should serialize");
  return serialized.value();
}

void test_search_directories_are_fixed_and_exclude_x() {
  constexpr std::uint32_t kDriveC = 1U << (L'C' - L'A');
  constexpr std::uint32_t kDriveE = 1U << (L'E' - L'A');
  constexpr std::uint32_t kDriveX = 1U << (L'X' - L'A');
  const auto directories =
      ytec::windowsapp::build_job_result_search_directories(
          kDriveC | kDriveE | kDriveX);
  check(directories == std::vector<std::wstring>{
            L"C:\\", L"C:\\Tsumugi\\", L"E:\\", L"E:\\Tsumugi\\"},
        "Only two fixed directories per eligible drive should be searched");
}

void test_result_file_name_requires_exact_product_shape() {
  check(
      ytec::windowsapp::is_product_job_result_file_name(
          L"Tsumugi-clone-job.result-20260801-123456Z.log") &&
          ytec::windowsapp::is_product_job_result_file_name(
              L"tsumugi-restore-job.result-20260801-123456Z.LOG"),
      "Clone and restore product result names should be case-insensitive");
  check(
      !ytec::windowsapp::is_product_job_result_file_name(
          L"Tsumugi-clone-job.result-2026-08-01.log") &&
          !ytec::windowsapp::is_product_job_result_file_name(
              L"Tsumugi-clone-job.result-20261340-256199Z.log") &&
          !ytec::windowsapp::is_product_job_result_file_name(
              L"other.result-20260801-123456Z.log"),
      "Guessed or malformed result names must not be imported");
}

void test_import_returns_verified_results_newest_first() {
  MockCandidateProvider provider;
  provider.paths = {
      L"E:\\Tsumugi\\Tsumugi-clone-job.result-20260801-120000Z.log",
      L"F:\\Tsumugi-restore-job.result-20260801-130000Z.log",
  };
  SequencedLoader loader;
  loader.responses = {
      result_bytes(
          "2026-08-01T12:00:00Z",
          ytec::imageformat::JobType::clone,
          ytec::imageformat::JobResultOutcome::passed),
      result_bytes(
          "2026-08-01T13:00:00Z",
          ytec::imageformat::JobType::restore_image,
          ytec::imageformat::JobResultOutcome::failed),
  };

  const auto imported = ytec::windowsapp::import_verified_job_results(
      provider, loader);
  check(imported.has_value() && imported.value().size() == 2U,
        "Both canonical result logs should import");
  check(
      imported.value()[0].record.job_type ==
              ytec::imageformat::JobType::restore_image &&
          imported.value()[0].record.outcome ==
              ytec::imageformat::JobResultOutcome::failed &&
          imported.value()[1].record.job_type ==
              ytec::imageformat::JobType::clone,
      "Imported results should be sorted by verified completion UTC newest first");
}

void test_import_rejects_tampered_candidate_without_partial_results() {
  MockCandidateProvider provider;
  provider.paths = {
      L"E:\\Tsumugi-clone-job.result-20260801-120000Z.log",
      L"F:\\Tsumugi-restore-job.result-20260801-130000Z.log",
  };
  auto tampered = result_bytes(
      "2026-08-01T13:00:00Z",
      ytec::imageformat::JobType::restore_image,
      ytec::imageformat::JobResultOutcome::failed);
  tampered.back() = std::byte{'X'};
  SequencedLoader loader;
  loader.responses = {
      result_bytes(
          "2026-08-01T12:00:00Z",
          ytec::imageformat::JobType::clone,
          ytec::imageformat::JobResultOutcome::passed),
      tampered,
  };

  const auto imported = ytec::windowsapp::import_verified_job_results(
      provider, loader);
  check(!imported.has_value(),
        "One tampered fixed-name log must block a misleading partial import");
  check(loader.call_count == 2U,
        "The tampered candidate should be opened and strictly verified");
}

void test_import_rejects_duplicate_before_loading() {
  MockCandidateProvider provider;
  provider.paths = {
      L"E:\\Tsumugi-clone-job.result-20260801-120000Z.log",
      L"e:\\tsumugi-clone-job.result-20260801-120000Z.LOG",
  };
  SequencedLoader loader;
  const auto imported = ytec::windowsapp::import_verified_job_results(
      provider, loader);
  check(!imported.has_value() && loader.call_count == 0U,
        "Windows-equivalent duplicate paths must fail before file access");
}

void test_import_rejects_file_name_content_mismatch() {
  MockCandidateProvider provider;
  provider.paths = {
      L"E:\\Tsumugi-clone-job.result-20260801-120000Z.log",
  };
  SequencedLoader loader;
  loader.responses = {
      result_bytes(
          "2026-08-01T13:00:00Z",
          ytec::imageformat::JobType::restore_image,
          ytec::imageformat::JobResultOutcome::passed),
  };
  const auto imported = ytec::windowsapp::import_verified_job_results(
      provider, loader);
  check(!imported.has_value() &&
            imported.error().code ==
                ytec::clonecore::ErrorCode::identity_mismatch,
        "A renamed result must not misrepresent its job type or completion time");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"search_directories_are_fixed_and_exclude_x",
       test_search_directories_are_fixed_and_exclude_x},
      {"result_file_name_requires_exact_product_shape",
       test_result_file_name_requires_exact_product_shape},
      {"import_returns_verified_results_newest_first",
       test_import_returns_verified_results_newest_first},
      {"import_rejects_tampered_candidate_without_partial_results",
       test_import_rejects_tampered_candidate_without_partial_results},
      {"import_rejects_duplicate_before_loading",
       test_import_rejects_duplicate_before_loading},
      {"import_rejects_file_name_content_mismatch",
       test_import_rejects_file_name_content_mismatch},
  };
  int failures{};
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << failure.message << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}
