#include "ytec/imageformat/job_result.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ytec::imageformat {
namespace {

constexpr std::size_t kMaximumDetailsBytes = 48U * 1024U;

clonecore::Error result_error(
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

std::string digest_to_hex(const Sha256Digest& digest) {
  constexpr std::array<char, 16> kHex{
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
  std::string output;
  output.reserve(digest.size() * 2U);
  for (const std::byte value : digest) {
    const auto number = std::to_integer<unsigned int>(value);
    output.push_back(kHex[(number >> 4U) & 0x0FU]);
    output.push_back(kHex[number & 0x0FU]);
  }
  return output;
}

clonecore::Result<Sha256Digest> digest_from_hex(
    const std::string_view text,
    const std::wstring_view field) {
  if (text.size() != Sha256Digest{}.size() * 2U) {
    return clonecore::Result<Sha256Digest>::failure(result_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        std::wstring(field),
        L"SHA-256は大文字16進64文字でなければなりません"));
  }
  const auto nibble = [](const char value) -> std::optional<unsigned int> {
    if (value >= '0' && value <= '9') {
      return static_cast<unsigned int>(value - '0');
    }
    if (value >= 'A' && value <= 'F') {
      return static_cast<unsigned int>(value - 'A' + 10);
    }
    return std::nullopt;
  };
  Sha256Digest digest{};
  for (std::size_t index = 0; index < digest.size(); ++index) {
    const auto high = nibble(text[index * 2U]);
    const auto low = nibble(text[index * 2U + 1U]);
    if (!high.has_value() || !low.has_value()) {
      return clonecore::Result<Sha256Digest>::failure(result_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          std::wstring(field),
          L"SHA-256に正規形でない文字があります"));
    }
    digest[index] = static_cast<std::byte>((*high << 4U) | *low);
  }
  return clonecore::Result<Sha256Digest>::success(digest);
}

bool valid_completed_utc(const std::string_view value) noexcept {
  if (value.size() != 20U || value[4] != '-' || value[7] != '-' ||
      value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
      value[19] != 'Z') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 4U || index == 7U || index == 10U || index == 13U ||
        index == 16U || index == 19U) {
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(value[index])) == 0) {
      return false;
    }
  }
  const auto decimal = [&value](
      const std::size_t offset, const std::size_t count) -> WORD {
    WORD result{};
    for (std::size_t index = 0; index < count; ++index) {
      result = static_cast<WORD>(
          result * 10U + static_cast<unsigned int>(value[offset + index] - '0'));
    }
    return result;
  };
  SYSTEMTIME time{};
  time.wYear = decimal(0U, 4U);
  time.wMonth = decimal(5U, 2U);
  time.wDay = decimal(8U, 2U);
  time.wHour = decimal(11U, 2U);
  time.wMinute = decimal(14U, 2U);
  time.wSecond = decimal(17U, 2U);
  FILETIME converted{};
  return time.wYear >= 2020U && SystemTimeToFileTime(&time, &converted) != FALSE;
}

bool valid_app_version(const std::string_view value) noexcept {
  return !value.empty() && value.size() <= 32U &&
      std::all_of(value.begin(), value.end(), [](const char character) {
        const unsigned char value = static_cast<unsigned char>(character);
        return std::isalnum(value) != 0 || character == '.' ||
            character == '-' || character == '+';
      });
}

bool valid_utf8(const std::string_view value) noexcept {
  if (value.find('\0') != std::string_view::npos ||
      value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return false;
  }
  return value.empty() ||
      MultiByteToWideChar(
          CP_UTF8,
          MB_ERR_INVALID_CHARS,
          value.data(),
          static_cast<int>(value.size()),
          nullptr,
          0) > 0;
}

std::optional<std::uint64_t> parse_canonical_size(
    const std::string_view value) noexcept {
  if (value.empty() || (value.size() > 1U && value.front() == '0')) {
    return std::nullopt;
  }
  std::uint64_t parsed{};
  const auto [end, error] = std::from_chars(
      value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) {
    return std::nullopt;
  }
  return parsed;
}

clonecore::Result<std::string_view> value_after_prefix(
    const std::string_view line,
    const std::string_view prefix,
    const std::wstring_view field) {
  if (!line.starts_with(prefix)) {
    return clonecore::Result<std::string_view>::failure(result_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        std::wstring(field),
        L"項目名、順序、または値が正規形ではありません"));
  }
  return clonecore::Result<std::string_view>::success(
      line.substr(prefix.size()));
}

}  // namespace

clonecore::Result<std::vector<std::byte>> serialize_job_result_log(
    const JobResultRecord& record) {
  std::string details = record.details_utf8;
  if (!details.ends_with('\n')) {
    details.push_back('\n');
  }
  if (!valid_completed_utc(record.completed_utc) ||
      !valid_app_version(record.app_version) || !valid_utf8(details) ||
      details.size() > kMaximumDetailsBytes) {
    return clonecore::Result<std::vector<std::byte>>::failure(result_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"WinPE実行結果ログ生成",
        L"完了日時、アプリ版、UTF-8詳細、または詳細長が不正です"));
  }
  const auto details_bytes = std::as_bytes(std::span(details));
  auto details_hash = sha256(details_bytes);
  if (!details_hash) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        details_hash.error());
  }

  std::string text =
      "Y-TEC Tsumugi Drive WinPE 実行結果\n"
      "schemaVersion=1\n"
      "jobHashSha256=" + digest_to_hex(record.job_payload_hash) + "\n"
      "jobType=" + std::string(job_type_name(record.job_type)) +
      "\nresult=" +
      std::string(record.outcome == JobResultOutcome::passed ? "PASS" : "FAIL") +
      "\ncompletedUtc=" + record.completed_utc +
      "\nappVersion=" + record.app_version +
      "\ndetailsLength=" + std::to_string(details.size()) +
      "\ndetailsSha256=" + digest_to_hex(details_hash.value()) +
      "\n---\n" + details;
  if (text.size() > kMaximumJobResultLogBytes) {
    return clonecore::Result<std::vector<std::byte>>::failure(result_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_BUFFER_OVERFLOW,
        L"WinPE実行結果ログ長",
        L"実行結果ログが安全上限を超えています"));
  }
  std::vector<std::byte> bytes(text.size());
  std::transform(
      text.begin(), text.end(), bytes.begin(), [](const char character) {
        return static_cast<std::byte>(static_cast<unsigned char>(character));
      });
  return clonecore::Result<std::vector<std::byte>>::success(std::move(bytes));
}

clonecore::Result<JobResultRecord> parse_and_verify_job_result_log(
    const std::span<const std::byte> bytes) {
  if (bytes.empty() || bytes.size() > kMaximumJobResultLogBytes) {
    return clonecore::Result<JobResultRecord>::failure(result_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_TOO_LARGE,
        L"WinPE実行結果ログ長",
        L"結果ログが空か、許容上限を超えています"));
  }
  const std::string text(
      reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (!valid_utf8(text)) {
    return clonecore::Result<JobResultRecord>::failure(result_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_NO_UNICODE_TRANSLATION,
        L"WinPE実行結果ログUTF-8",
        L"結果ログを厳格なUTF-8として解釈できません"));
  }

  std::array<std::string_view, 10> lines{};
  std::size_t position{};
  for (auto& line : lines) {
    const std::size_t end = text.find('\n', position);
    if (end == std::string::npos) {
      return clonecore::Result<JobResultRecord>::failure(result_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_HANDLE_EOF,
          L"WinPE実行結果ログ構造",
          L"必須ヘッダーが途中で終端しました"));
    }
    line = std::string_view(text).substr(position, end - position);
    position = end + 1U;
  }
  if (lines[0] != "Y-TEC Tsumugi Drive WinPE 実行結果" ||
      lines[1] != "schemaVersion=1" || lines[9] != "---") {
    return clonecore::Result<JobResultRecord>::failure(result_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"WinPE実行結果ログ構造",
        L"識別行、schemaVersion、または詳細区切りが不正です"));
  }

  auto job_hash_text = value_after_prefix(
      lines[2], "jobHashSha256=", L"WinPE結果ジョブハッシュ");
  auto job_type_text = value_after_prefix(
      lines[3], "jobType=", L"WinPE結果ジョブ種別");
  auto outcome_text = value_after_prefix(
      lines[4], "result=", L"WinPE結果状態");
  auto completed_text = value_after_prefix(
      lines[5], "completedUtc=", L"WinPE結果完了日時");
  auto app_version_text = value_after_prefix(
      lines[6], "appVersion=", L"WinPE結果アプリ版");
  auto details_length_text = value_after_prefix(
      lines[7], "detailsLength=", L"WinPE結果詳細長");
  auto details_hash_text = value_after_prefix(
      lines[8], "detailsSha256=", L"WinPE結果詳細ハッシュ");
  if (!job_hash_text || !job_type_text || !outcome_text || !completed_text ||
      !app_version_text || !details_length_text || !details_hash_text) {
    const clonecore::Error* errors[]{
        job_hash_text ? nullptr : &job_hash_text.error(),
        job_type_text ? nullptr : &job_type_text.error(),
        outcome_text ? nullptr : &outcome_text.error(),
        completed_text ? nullptr : &completed_text.error(),
        app_version_text ? nullptr : &app_version_text.error(),
        details_length_text ? nullptr : &details_length_text.error(),
        details_hash_text ? nullptr : &details_hash_text.error(),
    };
    for (const clonecore::Error* error : errors) {
      if (error != nullptr) {
        return clonecore::Result<JobResultRecord>::failure(*error);
      }
    }
  }

  auto job_hash = digest_from_hex(
      job_hash_text.value(), L"WinPE結果ジョブハッシュ");
  auto expected_details_hash = digest_from_hex(
      details_hash_text.value(), L"WinPE結果詳細ハッシュ");
  const auto declared_length = parse_canonical_size(details_length_text.value());
  const std::string details = text.substr(position);
  if (!job_hash || !expected_details_hash || !declared_length.has_value() ||
      declared_length.value() != details.size() ||
      details.size() > kMaximumDetailsBytes || !details.ends_with('\n')) {
    return clonecore::Result<JobResultRecord>::failure(result_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"WinPE実行結果ログ詳細",
        L"詳細長、末尾、またはSHA-256表現が不正です"));
  }
  auto observed_details_hash = sha256(std::as_bytes(std::span(details)));
  if (!observed_details_hash ||
      observed_details_hash.value() != expected_details_hash.value()) {
    return clonecore::Result<JobResultRecord>::failure(result_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"WinPE実行結果ログ詳細SHA-256",
        L"詳細本文が記録済みSHA-256と一致しません"));
  }

  std::optional<JobType> job_type;
  for (const JobType candidate : {
           JobType::clone,
           JobType::create_image,
           JobType::restore_image,
           JobType::mbr_to_gpt}) {
    if (job_type_text.value() == job_type_name(candidate)) {
      job_type = candidate;
      break;
    }
  }
  std::optional<JobResultOutcome> outcome;
  if (outcome_text.value() == "PASS") {
    outcome = JobResultOutcome::passed;
  } else if (outcome_text.value() == "FAIL") {
    outcome = JobResultOutcome::failed;
  }
  if (!job_type.has_value() || !outcome.has_value() ||
      !valid_completed_utc(completed_text.value()) ||
      !valid_app_version(app_version_text.value())) {
    return clonecore::Result<JobResultRecord>::failure(result_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"WinPE実行結果ログ項目",
        L"ジョブ種別、結果、完了日時、またはアプリ版が不正です"));
  }

  JobResultRecord record{
      .job_payload_hash = job_hash.take_value(),
      .job_type = *job_type,
      .outcome = *outcome,
      .completed_utc = std::string(completed_text.value()),
      .app_version = std::string(app_version_text.value()),
      .details_utf8 = details,
  };
  auto canonical = serialize_job_result_log(record);
  if (!canonical || canonical.value().size() != bytes.size() ||
      !std::equal(canonical.value().begin(), canonical.value().end(), bytes.begin())) {
    return clonecore::Result<JobResultRecord>::failure(result_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"WinPE実行結果ログ正規形",
        L"結果ログが正規形へ再生成できません"));
  }
  return clonecore::Result<JobResultRecord>::success(std::move(record));
}

}  // namespace ytec::imageformat
