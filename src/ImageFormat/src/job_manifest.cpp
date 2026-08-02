#include "ytec/imageformat/job_manifest.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::imageformat {
namespace {

constexpr std::size_t kMaximumModelCharacters = 256;
constexpr std::size_t kMaximumSerialCharacters = 64;
constexpr std::size_t kMaximumDeviceIdCharacters = 1024;
constexpr std::size_t kMaximumImagePathCharacters = 32U * 1024U;
constexpr std::size_t kMaximumAppVersionCharacters = 64;
constexpr std::size_t kSha256HexCharacters = 64;
constexpr char kHexDigits[] = "0123456789ABCDEF";

clonecore::Error job_error(
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

template <typename T>
clonecore::Result<T> job_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Result<T>::failure(job_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

clonecore::Result<std::string> to_utf8(const std::wstring_view value) {
  if (value.empty()) {
    return clonecore::Result<std::string>::success({});
  }
  if (value.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return job_failure<std::string>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_ARITHMETIC_OVERFLOW,
        L"ジョブJSON UTF-8変換",
        L"文字列が長すぎます");
  }
  const int required = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0,
      nullptr,
      nullptr);
  if (required <= 0) {
    return job_failure<std::string>(
        clonecore::ErrorCode::invalid_data,
        GetLastError(),
        L"ジョブJSON UTF-8変換",
        L"UTF-16文字列が不正です");
  }
  std::string result(static_cast<std::size_t>(required), '\0');
  const int converted = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      result.data(),
      required,
      nullptr,
      nullptr);
  if (converted != required) {
    return job_failure<std::string>(
        clonecore::ErrorCode::invalid_data,
        GetLastError(),
        L"ジョブJSON UTF-8変換",
        L"UTF-16文字列を完全に変換できません");
  }
  return clonecore::Result<std::string>::success(std::move(result));
}

clonecore::Result<std::wstring> from_utf8(const std::string_view value) {
  if (value.empty()) {
    return clonecore::Result<std::wstring>::success({});
  }
  if (value.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return job_failure<std::wstring>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"ジョブJSON UTF-8検証",
        L"文字列が長すぎます");
  }
  const int required = MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0);
  if (required <= 0) {
    return job_failure<std::wstring>(
        clonecore::ErrorCode::invalid_data,
        GetLastError(),
        L"ジョブJSON UTF-8検証",
        L"UTF-8文字列が不正です");
  }
  std::wstring result(static_cast<std::size_t>(required), L'\0');
  const int converted = MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      result.data(),
      required);
  if (converted != required) {
    return job_failure<std::wstring>(
        clonecore::ErrorCode::invalid_data,
        GetLastError(),
        L"ジョブJSON UTF-8検証",
        L"UTF-8文字列を完全に変換できません");
  }
  return clonecore::Result<std::wstring>::success(std::move(result));
}

bool is_visible_ascii(const std::string_view value) noexcept {
  for (const unsigned char character : value) {
    if (character < 0x20U || character > 0x7EU) {
      return false;
    }
  }
  return true;
}

bool is_digit_at(const std::string_view value, const std::size_t index) {
  return index < value.size() &&
         value[index] >= '0' && value[index] <= '9';
}

bool is_valid_utc_timestamp(const std::string_view value) {
  if (value.size() != 20 ||
      value[4] != '-' || value[7] != '-' ||
      value[10] != 'T' || value[13] != ':' ||
      value[16] != ':' || value[19] != 'Z') {
    return false;
  }
  constexpr std::array<std::size_t, 14> kDigitPositions{
      0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
  for (const std::size_t index : kDigitPositions) {
    if (!is_digit_at(value, index)) {
      return false;
    }
  }
  const auto two_digits = [&value](const std::size_t index) {
    return static_cast<unsigned int>((value[index] - '0') * 10) +
           static_cast<unsigned int>(value[index + 1] - '0');
  };
  const unsigned int month = two_digits(5);
  const unsigned int day = two_digits(8);
  const unsigned int hour = two_digits(11);
  const unsigned int minute = two_digits(14);
  const unsigned int second = two_digits(17);
  return month >= 1 && month <= 12 &&
         day >= 1 && day <= 31 &&
         hour <= 23 && minute <= 59 && second <= 60;
}

bool is_absolute_local_path(const std::wstring_view path) {
  if (path.size() >= 3 &&
      ((path[0] >= L'A' && path[0] <= L'Z') ||
       (path[0] >= L'a' && path[0] <= L'z')) &&
      path[1] == L':' && path[2] == L'\\') {
    return true;
  }
  constexpr std::wstring_view kExtendedDrivePrefix = L"\\\\?\\";
  if (path.size() >= 7 &&
      path.substr(0, kExtendedDrivePrefix.size()) ==
          kExtendedDrivePrefix &&
      ((path[4] >= L'A' && path[4] <= L'Z') ||
       (path[4] >= L'a' && path[4] <= L'z')) &&
      path[5] == L':' && path[6] == L'\\') {
    return true;
  }
  constexpr std::wstring_view kVolumePrefix = L"\\\\?\\Volume{";
  return path.size() > kVolumePrefix.size() &&
         path.substr(0, kVolumePrefix.size()) == kVolumePrefix &&
         path.find(L"}\\", kVolumePrefix.size()) !=
             std::wstring_view::npos;
}

clonecore::Status validate_identity(
    const clonecore::StableDiskIdentity& identity,
    const std::wstring_view role) {
  if (identity.model.size() > kMaximumModelCharacters ||
      identity.serial_suffix.size() > kMaximumSerialCharacters ||
      identity.device_instance_id.size() >
          kMaximumDeviceIdCharacters ||
      !is_visible_ascii(identity.serial_suffix)) {
    return clonecore::Status::failure(job_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        std::wstring(role) + L"ジョブ識別情報",
        L"ディスク識別文字列の長さまたは文字種が不正です"));
  }
  return clonecore::validate_stable_identity(identity, identity, role);
}

clonecore::Status validate_manifest(const JobManifest& manifest) {
  if (manifest.schema_version != kLegacyJobManifestSchemaVersion &&
      manifest.schema_version != kJobManifestSchemaVersion) {
    return clonecore::Status::failure(job_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REVISION_MISMATCH,
        L"ジョブJSONスキーマ",
        L"対応していないジョブスキーマです"));
  }
  if (manifest.schema_version == kLegacyJobManifestSchemaVersion &&
      manifest.execution_mode != JobExecutionMode::review_required) {
    return clonecore::Status::failure(job_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"ジョブJSON実行方式",
        L"旧形式ジョブはWinPEでの手動確認が必要です"));
  }
  if (manifest.execution_mode != JobExecutionMode::review_required &&
      manifest.execution_mode != JobExecutionMode::auto_once) {
    return clonecore::Status::failure(job_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"ジョブJSON実行方式",
        L"ジョブ実行方式が不正です"));
  }
  if (manifest.execution_mode == JobExecutionMode::auto_once &&
      manifest.job_type != JobType::clone &&
      manifest.job_type != JobType::restore_image) {
    return clonecore::Status::failure(job_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"ジョブJSON一回限り自動実行",
        L"一回限り自動実行はクローンまたは復元ジョブだけで使用できます"));
  }
  if (!is_valid_utc_timestamp(manifest.created_utc) ||
      manifest.app_version.empty() ||
      manifest.app_version.size() > kMaximumAppVersionCharacters ||
      !is_visible_ascii(manifest.app_version) ||
      manifest.image_path.size() > kMaximumImagePathCharacters) {
    return clonecore::Status::failure(job_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"ジョブJSON共通項目",
        L"作成日時、アプリバージョン、またはイメージパスが不正です"));
  }
  if (manifest.source.has_value()) {
    const auto source_status =
        validate_identity(*manifest.source, L"コピー元");
    if (!source_status) {
      return source_status;
    }
  }
  if (manifest.target.has_value()) {
    const auto target_status =
        validate_identity(*manifest.target, L"コピー先");
    if (!target_status) {
      return target_status;
    }
  }

  const bool has_image_path = !manifest.image_path.empty();
  const bool has_restore_image_identity =
      manifest.restore_image_identity.has_value();
  if (has_image_path && !is_absolute_local_path(manifest.image_path)) {
    return clonecore::Status::failure(job_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_BAD_PATHNAME,
        L"ジョブJSONイメージパス",
        L"ローカルの絶対Windowsパスだけを指定できます"));
  }
  if (has_restore_image_identity) {
    const bool all_zero = std::all_of(
        manifest.restore_image_identity->global_hash.begin(),
        manifest.restore_image_identity->global_hash.end(),
        [](const std::byte value) { return value == std::byte{0}; });
    if (manifest.restore_image_identity->length_bytes == 0 || all_zero) {
      return clonecore::Status::failure(job_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_DATA,
          L"復元イメージ識別情報",
          L"検証済みイメージの長さまたは全体SHA-256が不正です"));
    }
  }

  switch (manifest.job_type) {
    case JobType::clone:
      if (!manifest.source || !manifest.target || has_image_path ||
          has_restore_image_identity ||
          manifest.requested_conversion !=
              RequestedConversion::preserve ||
          !manifest.destructive_target_confirmed) {
        break;
      }
      return clonecore::validate_clone_selection(
          *manifest.source,
          *manifest.source,
          *manifest.target,
          *manifest.target);
    case JobType::create_image:
      if (!manifest.source || manifest.target || !has_image_path ||
          has_restore_image_identity ||
          manifest.requested_conversion !=
              RequestedConversion::preserve ||
          manifest.destructive_target_confirmed) {
        break;
      }
      return clonecore::success_status();
    case JobType::restore_image:
      if (manifest.source || !manifest.target || !has_image_path ||
          !has_restore_image_identity ||
          manifest.requested_conversion !=
              RequestedConversion::preserve ||
          !manifest.destructive_target_confirmed) {
        break;
      }
      if (manifest.target->is_system_disk) {
        return clonecore::Status::failure(job_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_ACCESS_DENIED,
            L"復元ジョブのシステムディスク保護",
            L"実行中システムのディスクは復元先にできません"));
      }
      return clonecore::success_status();
    case JobType::mbr_to_gpt:
      if (!manifest.source || !manifest.target || has_image_path ||
          has_restore_image_identity ||
          manifest.requested_conversion !=
              RequestedConversion::mbr_to_gpt ||
          !manifest.destructive_target_confirmed) {
        break;
      }
      return clonecore::validate_clone_selection(
          *manifest.source,
          *manifest.source,
          *manifest.target,
          *manifest.target);
  }
  return clonecore::Status::failure(job_error(
      clonecore::ErrorCode::invalid_argument,
      ERROR_INVALID_DATA,
      L"ジョブJSON種別整合性",
      L"ジョブ種別とコピー元、コピー先、イメージ、確認状態が一致しません"));
}

void append_json_string_from_utf8(
    std::string& output,
    const std::string_view value) {
  output.push_back('"');
  for (const unsigned char character : value) {
    switch (character) {
      case '"':
        output.append("\\\"");
        break;
      case '\\':
        output.append("\\\\");
        break;
      case '\b':
        output.append("\\b");
        break;
      case '\f':
        output.append("\\f");
        break;
      case '\n':
        output.append("\\n");
        break;
      case '\r':
        output.append("\\r");
        break;
      case '\t':
        output.append("\\t");
        break;
      default:
        if (character < 0x20U) {
          output.append("\\u00");
          output.push_back(kHexDigits[(character >> 4U) & 0x0FU]);
          output.push_back(kHexDigits[character & 0x0FU]);
        } else {
          output.push_back(static_cast<char>(character));
        }
        break;
    }
  }
  output.push_back('"');
}

clonecore::Status append_json_wstring(
    std::string& output,
    const std::wstring_view value) {
  const auto utf8 = to_utf8(value);
  if (!utf8) {
    return clonecore::Status::failure(utf8.error());
  }
  append_json_string_from_utf8(output, utf8.value());
  return clonecore::success_status();
}

void append_uint64(std::string& output, const std::uint64_t value) {
  std::array<char, 32> buffer{};
  const auto converted =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  output.append(buffer.data(), converted.ptr);
}

std::string digest_to_hex(const Sha256Digest& digest);

void append_restore_image_identity(
    std::string& output,
    const std::optional<RestoreImageIdentity>& identity) {
  if (!identity) {
    output.append("null");
    return;
  }
  output.append("{\"lengthBytes\":");
  append_uint64(output, identity->length_bytes);
  output.append(",\"globalHashSha256\":");
  append_json_string_from_utf8(
      output, digest_to_hex(identity->global_hash));
  output.push_back('}');
}

clonecore::Status append_identity(
    std::string& output,
    const std::optional<clonecore::StableDiskIdentity>& identity) {
  if (!identity) {
    output.append("null");
    return clonecore::success_status();
  }
  output.append("{\"diskNumber\":");
  append_uint64(output, identity->disk_number);
  output.append(",\"model\":");
  auto status = append_json_wstring(output, identity->model);
  if (!status) {
    return status;
  }
  output.append(",\"sizeBytes\":");
  append_uint64(output, identity->size_bytes);
  output.append(",\"logicalSectorSize\":");
  append_uint64(output, identity->logical_sector_size);
  output.append(",\"serialSuffix\":");
  append_json_string_from_utf8(output, identity->serial_suffix);
  output.append(",\"deviceInstanceId\":");
  status = append_json_wstring(output, identity->device_instance_id);
  if (!status) {
    return status;
  }
  output.append(",\"isSystemDisk\":");
  output.append(identity->is_system_disk ? "true" : "false");
  output.push_back('}');
  return clonecore::success_status();
}

clonecore::Result<std::string> serialize_payload(
    const JobManifest& manifest) {
  const auto valid = validate_manifest(manifest);
  if (!valid) {
    return clonecore::Result<std::string>::failure(valid.error());
  }

  std::string payload;
  payload.reserve(1024);
  payload.append("{\"schemaVersion\":");
  append_uint64(payload, manifest.schema_version);
  payload.append(",\"jobType\":");
  append_json_string_from_utf8(payload, job_type_name(manifest.job_type));
  payload.append(",\"source\":");
  auto status = append_identity(payload, manifest.source);
  if (!status) {
    return clonecore::Result<std::string>::failure(status.error());
  }
  payload.append(",\"target\":");
  status = append_identity(payload, manifest.target);
  if (!status) {
    return clonecore::Result<std::string>::failure(status.error());
  }
  payload.append(",\"imagePath\":");
  status = append_json_wstring(payload, manifest.image_path);
  if (!status) {
    return clonecore::Result<std::string>::failure(status.error());
  }
  payload.append(",\"restoreImageIdentity\":");
  append_restore_image_identity(
      payload, manifest.restore_image_identity);
  payload.append(",\"requestedConversion\":");
  append_json_string_from_utf8(
      payload,
      requested_conversion_name(manifest.requested_conversion));
  payload.append(",\"createdUtc\":");
  append_json_string_from_utf8(payload, manifest.created_utc);
  payload.append(",\"appVersion\":");
  append_json_string_from_utf8(payload, manifest.app_version);
  if (manifest.schema_version == kJobManifestSchemaVersion) {
    payload.append(",\"executionMode\":");
    append_json_string_from_utf8(
        payload, job_execution_mode_name(manifest.execution_mode));
  }
  payload.append(",\"destructiveTargetConfirmed\":");
  payload.append(
      manifest.destructive_target_confirmed ? "true" : "false");
  payload.push_back('}');
  return clonecore::Result<std::string>::success(std::move(payload));
}

int hex_value(const char character) noexcept {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'A' && character <= 'F') {
    return character - 'A' + 10;
  }
  return -1;
}

std::string digest_to_hex(const Sha256Digest& digest) {
  std::string result;
  result.reserve(kSha256HexCharacters);
  for (const std::byte value : digest) {
    const auto byte = std::to_integer<unsigned int>(value);
    result.push_back(kHexDigits[(byte >> 4U) & 0x0FU]);
    result.push_back(kHexDigits[byte & 0x0FU]);
  }
  return result;
}

clonecore::Result<Sha256Digest> digest_from_hex(
    const std::string_view value) {
  if (value.size() != kSha256HexCharacters) {
    return job_failure<Sha256Digest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSONハッシュ解析",
        L"SHA-256文字列の長さが不正です");
  }
  Sha256Digest digest{};
  for (std::size_t index = 0; index < digest.size(); ++index) {
    const int high = hex_value(value[index * 2]);
    const int low = hex_value(value[index * 2 + 1]);
    if (high < 0 || low < 0) {
      return job_failure<Sha256Digest>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"ジョブJSONハッシュ解析",
          L"SHA-256文字列は大文字16進数である必要があります");
    }
    digest[index] = static_cast<std::byte>((high << 4) | low);
  }
  return clonecore::Result<Sha256Digest>::success(digest);
}

void append_code_point_utf8(std::string& output, const std::uint32_t value) {
  if (value <= 0x7FU) {
    output.push_back(static_cast<char>(value));
  } else if (value <= 0x7FFU) {
    output.push_back(static_cast<char>(0xC0U | (value >> 6U)));
    output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  } else if (value <= 0xFFFFU) {
    output.push_back(static_cast<char>(0xE0U | (value >> 12U)));
    output.push_back(
        static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  } else {
    output.push_back(static_cast<char>(0xF0U | (value >> 18U)));
    output.push_back(
        static_cast<char>(0x80U | ((value >> 12U) & 0x3FU)));
    output.push_back(
        static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  }
}

class JsonCursor final {
 public:
  explicit JsonCursor(const std::string_view input) : input_(input) {}

  [[nodiscard]] std::size_t position() const noexcept {
    return position_;
  }

  [[nodiscard]] bool at_end() const noexcept {
    return position_ == input_.size();
  }

  [[nodiscard]] bool consume(const std::string_view literal) {
    if (position_ > input_.size() ||
        input_.size() - position_ < literal.size() ||
        input_.substr(position_, literal.size()) != literal) {
      return false;
    }
    position_ += literal.size();
    return true;
  }

  [[nodiscard]] clonecore::Result<std::uint64_t> parse_uint64() {
    if (position_ >= input_.size() ||
        input_[position_] < '0' || input_[position_] > '9') {
      return syntax_failure<std::uint64_t>(L"整数が必要です");
    }
    const std::size_t start = position_;
    if (input_[position_] == '0') {
      ++position_;
      if (position_ < input_.size() &&
          input_[position_] >= '0' && input_[position_] <= '9') {
        return syntax_failure<std::uint64_t>(
            L"整数の先頭ゼロは許可されません");
      }
    } else {
      while (position_ < input_.size() &&
             input_[position_] >= '0' && input_[position_] <= '9') {
        ++position_;
      }
    }
    std::uint64_t value{};
    const auto converted = std::from_chars(
        input_.data() + start,
        input_.data() + position_,
        value);
    if (converted.ec != std::errc{} ||
        converted.ptr != input_.data() + position_) {
      return syntax_failure<std::uint64_t>(
          L"整数が64ビット範囲を超えています");
    }
    return clonecore::Result<std::uint64_t>::success(value);
  }

  [[nodiscard]] clonecore::Result<bool> parse_bool() {
    if (consume("true")) {
      return clonecore::Result<bool>::success(true);
    }
    if (consume("false")) {
      return clonecore::Result<bool>::success(false);
    }
    return syntax_failure<bool>(L"真偽値が必要です");
  }

  [[nodiscard]] clonecore::Result<std::string> parse_string() {
    if (!consume("\"")) {
      return syntax_failure<std::string>(L"文字列が必要です");
    }
    std::string result;
    while (position_ < input_.size()) {
      const unsigned char character =
          static_cast<unsigned char>(input_[position_++]);
      if (character == '"') {
        return clonecore::Result<std::string>::success(
            std::move(result));
      }
      if (character < 0x20U) {
        return syntax_failure<std::string>(
            L"文字列に未エスケープの制御文字があります");
      }
      if (character != '\\') {
        result.push_back(static_cast<char>(character));
        continue;
      }
      if (position_ >= input_.size()) {
        return syntax_failure<std::string>(
            L"文字列のエスケープが途中で終わっています");
      }
      const char escaped = input_[position_++];
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          result.push_back(escaped);
          break;
        case 'b':
          result.push_back('\b');
          break;
        case 'f':
          result.push_back('\f');
          break;
        case 'n':
          result.push_back('\n');
          break;
        case 'r':
          result.push_back('\r');
          break;
        case 't':
          result.push_back('\t');
          break;
        case 'u': {
          const auto code_unit = parse_hex_code_unit();
          if (!code_unit) {
            return clonecore::Result<std::string>::failure(
                code_unit.error());
          }
          std::uint32_t code_point = code_unit.value();
          if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
            if (!consume("\\u")) {
              return syntax_failure<std::string>(
                  L"上位サロゲートに下位サロゲートがありません");
            }
            const auto low = parse_hex_code_unit();
            if (!low) {
              return clonecore::Result<std::string>::failure(low.error());
            }
            if (low.value() < 0xDC00U || low.value() > 0xDFFFU) {
              return syntax_failure<std::string>(
                  L"下位サロゲートが不正です");
            }
            code_point = 0x10000U +
                         ((code_point - 0xD800U) << 10U) +
                         (low.value() - 0xDC00U);
          } else if (code_point >= 0xDC00U &&
                     code_point <= 0xDFFFU) {
            return syntax_failure<std::string>(
                L"単独の下位サロゲートは許可されません");
          }
          append_code_point_utf8(result, code_point);
          break;
        }
        default:
          return syntax_failure<std::string>(
              L"未対応の文字列エスケープです");
      }
    }
    return syntax_failure<std::string>(
        L"文字列の終端引用符がありません");
  }

 private:
  template <typename T>
  [[nodiscard]] clonecore::Result<T> syntax_failure(
      std::wstring message) const {
    message.append(L" 位置: ");
    message.append(std::to_wstring(position_));
    return job_failure<T>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSON構文解析",
        std::move(message));
  }

  [[nodiscard]] clonecore::Result<std::uint32_t> parse_hex_code_unit() {
    if (position_ > input_.size() ||
        input_.size() - position_ < 4) {
      return syntax_failure<std::uint32_t>(
          L"Unicodeエスケープが短すぎます");
    }
    std::uint32_t value{};
    for (std::size_t index = 0; index < 4; ++index) {
      const char character = input_[position_++];
      int digit = hex_value(character);
      if (digit < 0 && character >= 'a' && character <= 'f') {
        digit = character - 'a' + 10;
      }
      if (digit < 0) {
        return syntax_failure<std::uint32_t>(
            L"Unicodeエスケープが不正です");
      }
      value = (value << 4U) | static_cast<std::uint32_t>(digit);
    }
    return clonecore::Result<std::uint32_t>::success(value);
  }

  std::string_view input_;
  std::size_t position_{};
};

clonecore::Result<clonecore::StableDiskIdentity> parse_identity(
    JsonCursor& cursor) {
  if (!cursor.consume("{\"diskNumber\":")) {
    return job_failure<clonecore::StableDiskIdentity>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSONディスク識別解析",
        L"diskNumberがありません");
  }
  const auto disk_number = cursor.parse_uint64();
  if (!disk_number ||
      disk_number.value() > std::numeric_limits<std::uint32_t>::max() ||
      !cursor.consume(",\"model\":")) {
    return job_failure<clonecore::StableDiskIdentity>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSONディスク識別解析",
        L"diskNumberまたはmodelが不正です");
  }
  const auto model_utf8 = cursor.parse_string();
  if (!model_utf8 || !cursor.consume(",\"sizeBytes\":")) {
    return job_failure<clonecore::StableDiskIdentity>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSONディスク識別解析",
        L"modelまたはsizeBytesが不正です");
  }
  const auto size_bytes = cursor.parse_uint64();
  if (!size_bytes || !cursor.consume(",\"logicalSectorSize\":")) {
    return job_failure<clonecore::StableDiskIdentity>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSONディスク識別解析",
        L"sizeBytesまたはlogicalSectorSizeが不正です");
  }
  const auto sector_size = cursor.parse_uint64();
  if (!sector_size ||
      sector_size.value() > std::numeric_limits<std::uint32_t>::max() ||
      !cursor.consume(",\"serialSuffix\":")) {
    return job_failure<clonecore::StableDiskIdentity>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSONディスク識別解析",
        L"logicalSectorSizeまたはserialSuffixが不正です");
  }
  const auto serial = cursor.parse_string();
  if (!serial || !cursor.consume(",\"deviceInstanceId\":")) {
    return job_failure<clonecore::StableDiskIdentity>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSONディスク識別解析",
        L"serialSuffixまたはdeviceInstanceIdが不正です");
  }
  const auto device_id_utf8 = cursor.parse_string();
  if (!device_id_utf8 || !cursor.consume(",\"isSystemDisk\":")) {
    return job_failure<clonecore::StableDiskIdentity>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSONディスク識別解析",
        L"deviceInstanceIdまたはisSystemDiskが不正です");
  }
  const auto system_disk = cursor.parse_bool();
  if (!system_disk || !cursor.consume("}")) {
    return job_failure<clonecore::StableDiskIdentity>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSONディスク識別解析",
        L"isSystemDiskまたはオブジェクト終端が不正です");
  }
  const auto model = from_utf8(model_utf8.value());
  const auto device_id = from_utf8(device_id_utf8.value());
  if (!model) {
    return clonecore::Result<clonecore::StableDiskIdentity>::failure(
        model.error());
  }
  if (!device_id) {
    return clonecore::Result<clonecore::StableDiskIdentity>::failure(
        device_id.error());
  }
  return clonecore::Result<clonecore::StableDiskIdentity>::success(
      clonecore::StableDiskIdentity{
          .disk_number = static_cast<std::uint32_t>(disk_number.value()),
          .model = model.value(),
          .size_bytes = size_bytes.value(),
          .logical_sector_size =
              static_cast<std::uint32_t>(sector_size.value()),
          .serial_suffix = serial.value(),
          .device_instance_id = device_id.value(),
          .is_system_disk = system_disk.value()});
}

clonecore::Result<std::optional<clonecore::StableDiskIdentity>>
parse_optional_identity(JsonCursor& cursor) {
  if (cursor.consume("null")) {
    return clonecore::Result<
        std::optional<clonecore::StableDiskIdentity>>::success(std::nullopt);
  }
  auto identity = parse_identity(cursor);
  if (!identity) {
    return clonecore::Result<
        std::optional<clonecore::StableDiskIdentity>>::failure(
            identity.error());
  }
  return clonecore::Result<
      std::optional<clonecore::StableDiskIdentity>>::success(
          std::move(identity.value()));
}

clonecore::Result<std::optional<RestoreImageIdentity>>
parse_optional_restore_image_identity(JsonCursor& cursor) {
  if (cursor.consume("null")) {
    return clonecore::Result<
        std::optional<RestoreImageIdentity>>::success(std::nullopt);
  }
  if (!cursor.consume("{\"lengthBytes\":")) {
    return job_failure<std::optional<RestoreImageIdentity>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSON復元イメージ識別解析",
        L"lengthBytesがありません");
  }
  const auto length = cursor.parse_uint64();
  if (!length || !cursor.consume(",\"globalHashSha256\":")) {
    return job_failure<std::optional<RestoreImageIdentity>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSON復元イメージ識別解析",
        L"lengthBytesまたはglobalHashSha256が不正です");
  }
  const auto hash_text = cursor.parse_string();
  if (!hash_text || !cursor.consume("}")) {
    return job_failure<std::optional<RestoreImageIdentity>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSON復元イメージ識別解析",
        L"globalHashSha256またはオブジェクト終端が不正です");
  }
  const auto digest = digest_from_hex(hash_text.value());
  if (!digest) {
    return clonecore::Result<
        std::optional<RestoreImageIdentity>>::failure(digest.error());
  }
  return clonecore::Result<
      std::optional<RestoreImageIdentity>>::success(
      RestoreImageIdentity{
          .length_bytes = length.value(),
          .global_hash = digest.value(),
      });
}

clonecore::Result<JobType> parse_job_type(
    const std::string_view value) {
  if (value == "clone") {
    return clonecore::Result<JobType>::success(JobType::clone);
  }
  if (value == "create-image") {
    return clonecore::Result<JobType>::success(JobType::create_image);
  }
  if (value == "restore-image") {
    return clonecore::Result<JobType>::success(JobType::restore_image);
  }
  if (value == "mbr-to-gpt") {
    return clonecore::Result<JobType>::success(JobType::mbr_to_gpt);
  }
  return job_failure<JobType>(
      clonecore::ErrorCode::unsupported_layout,
      ERROR_NOT_SUPPORTED,
      L"ジョブJSON種別",
      L"未知のジョブ種別です");
}

clonecore::Result<RequestedConversion> parse_conversion(
    const std::string_view value) {
  if (value == "preserve") {
    return clonecore::Result<RequestedConversion>::success(
        RequestedConversion::preserve);
  }
  if (value == "mbr-to-gpt") {
    return clonecore::Result<RequestedConversion>::success(
        RequestedConversion::mbr_to_gpt);
  }
  return job_failure<RequestedConversion>(
      clonecore::ErrorCode::unsupported_layout,
      ERROR_NOT_SUPPORTED,
      L"ジョブJSON変換方式",
      L"未知の変換方式です");
}

clonecore::Result<JobExecutionMode> parse_execution_mode(
    const std::string_view value) {
  if (value == "review-required") {
    return clonecore::Result<JobExecutionMode>::success(
        JobExecutionMode::review_required);
  }
  if (value == "auto-once") {
    return clonecore::Result<JobExecutionMode>::success(
        JobExecutionMode::auto_once);
  }
  return job_failure<JobExecutionMode>(
      clonecore::ErrorCode::unsupported_layout,
      ERROR_NOT_SUPPORTED,
      L"ジョブJSON実行方式",
      L"未知のジョブ実行方式です");
}

clonecore::Result<JobManifest> parse_payload(JsonCursor& cursor) {
  if (!cursor.consume("{\"schemaVersion\":")) {
    return job_failure<JobManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSON payload解析",
        L"schemaVersionがありません");
  }
  const auto schema_version = cursor.parse_uint64();
  if (!schema_version ||
      schema_version.value() > std::numeric_limits<std::uint32_t>::max() ||
      !cursor.consume(",\"jobType\":")) {
    return job_failure<JobManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSON payload解析",
        L"schemaVersionまたはjobTypeが不正です");
  }
  if (schema_version.value() != kLegacyJobManifestSchemaVersion &&
      schema_version.value() != kJobManifestSchemaVersion) {
    return job_failure<JobManifest>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REVISION_MISMATCH,
        L"ジョブJSONスキーマ",
        L"対応していないジョブスキーマです");
  }
  const auto job_type_text = cursor.parse_string();
  if (!job_type_text || !cursor.consume(",\"source\":")) {
    return job_failure<JobManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSON payload解析",
        L"jobTypeまたはsourceが不正です");
  }
  const auto source = parse_optional_identity(cursor);
  if (!source || !cursor.consume(",\"target\":")) {
    return job_failure<JobManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSON payload解析",
        L"sourceまたはtargetが不正です");
  }
  const auto target = parse_optional_identity(cursor);
  if (!target || !cursor.consume(",\"imagePath\":")) {
    return job_failure<JobManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSON payload解析",
        L"targetまたはimagePathが不正です");
  }
  const auto image_path_utf8 = cursor.parse_string();
  if (!image_path_utf8 ||
      !cursor.consume(",\"restoreImageIdentity\":")) {
    return job_failure<JobManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSON payload解析",
        L"imagePathまたはrestoreImageIdentityが不正です");
  }
  const auto restore_image_identity =
      parse_optional_restore_image_identity(cursor);
  if (!restore_image_identity ||
      !cursor.consume(",\"requestedConversion\":")) {
    return job_failure<JobManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSON payload解析",
        L"restoreImageIdentityまたはrequestedConversionが不正です");
  }
  const auto conversion_text = cursor.parse_string();
  if (!conversion_text || !cursor.consume(",\"createdUtc\":")) {
    return job_failure<JobManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSON payload解析",
        L"requestedConversionまたはcreatedUtcが不正です");
  }
  const auto created_utc = cursor.parse_string();
  if (!created_utc || !cursor.consume(",\"appVersion\":")) {
    return job_failure<JobManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSON payload解析",
        L"createdUtcまたはappVersionが不正です");
  }
  const auto app_version = cursor.parse_string();
  if (!app_version) {
    return job_failure<JobManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSON payload解析",
        L"appVersionが不正です");
  }
  JobExecutionMode execution_mode = JobExecutionMode::review_required;
  if (schema_version.value() == kJobManifestSchemaVersion) {
    if (!cursor.consume(",\"executionMode\":")) {
      return job_failure<JobManifest>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"ジョブJSON payload解析",
          L"executionModeがありません");
    }
    const auto execution_mode_text = cursor.parse_string();
    if (!execution_mode_text) {
      return clonecore::Result<JobManifest>::failure(
          execution_mode_text.error());
    }
    const auto parsed_execution_mode =
        parse_execution_mode(execution_mode_text.value());
    if (!parsed_execution_mode) {
      return clonecore::Result<JobManifest>::failure(
          parsed_execution_mode.error());
    }
    execution_mode = parsed_execution_mode.value();
  }
  if (!cursor.consume(",\"destructiveTargetConfirmed\":")) {
    return job_failure<JobManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSON payload解析",
        L"確認状態がありません");
  }
  const auto confirmed = cursor.parse_bool();
  if (!confirmed || !cursor.consume("}")) {
    return job_failure<JobManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSON payload解析",
        L"確認状態またはpayload終端が不正です");
  }

  const auto job_type = parse_job_type(job_type_text.value());
  const auto conversion = parse_conversion(conversion_text.value());
  const auto image_path = from_utf8(image_path_utf8.value());
  if (!job_type) {
    return clonecore::Result<JobManifest>::failure(job_type.error());
  }
  if (!conversion) {
    return clonecore::Result<JobManifest>::failure(conversion.error());
  }
  if (!image_path) {
    return clonecore::Result<JobManifest>::failure(image_path.error());
  }
  return clonecore::Result<JobManifest>::success(JobManifest{
      .schema_version =
          static_cast<std::uint32_t>(schema_version.value()),
      .job_type = job_type.value(),
      .source = source.value(),
      .target = target.value(),
      .image_path = image_path.value(),
      .restore_image_identity = restore_image_identity.value(),
      .requested_conversion = conversion.value(),
      .created_utc = created_utc.value(),
      .app_version = app_version.value(),
      .execution_mode = execution_mode,
      .destructive_target_confirmed = confirmed.value()});
}

bool equal_bytes(
    const std::span<const std::byte> left,
    const std::span<const std::byte> right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin());
}

}  // namespace

std::string_view job_type_name(const JobType type) noexcept {
  switch (type) {
    case JobType::clone:
      return "clone";
    case JobType::create_image:
      return "create-image";
    case JobType::restore_image:
      return "restore-image";
    case JobType::mbr_to_gpt:
      return "mbr-to-gpt";
  }
  return "unknown";
}

std::string_view requested_conversion_name(
    const RequestedConversion conversion) noexcept {
  switch (conversion) {
    case RequestedConversion::preserve:
      return "preserve";
    case RequestedConversion::mbr_to_gpt:
      return "mbr-to-gpt";
  }
  return "unknown";
}

std::string_view job_execution_mode_name(
    const JobExecutionMode mode) noexcept {
  switch (mode) {
    case JobExecutionMode::review_required:
      return "review-required";
    case JobExecutionMode::auto_once:
      return "auto-once";
  }
  return "unknown";
}

clonecore::Result<std::vector<std::byte>>
serialize_hashed_job_manifest(const JobManifest& manifest) {
  const auto payload = serialize_payload(manifest);
  if (!payload) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        payload.error());
  }
  const auto payload_bytes = std::as_bytes(std::span<const char>(
      payload.value().data(), payload.value().size()));
  const auto digest = sha256(payload_bytes);
  if (!digest) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        digest.error());
  }
  std::string json;
  json.reserve(payload.value().size() + 100);
  json.append("{\"payload\":");
  json.append(payload.value());
  json.append(",\"jobHashSha256\":\"");
  json.append(digest_to_hex(digest.value()));
  json.append("\"}");
  if (json.size() > kMaximumJobManifestBytes) {
    return job_failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_FILE_TOO_LARGE,
        L"ジョブJSON生成",
        L"ジョブJSONが許容上限を超えています");
  }
  std::vector<std::byte> result(json.size());
  for (std::size_t index = 0; index < json.size(); ++index) {
    result[index] = static_cast<std::byte>(
        static_cast<unsigned char>(json[index]));
  }
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(result));
}

clonecore::Result<VerifiedJobManifest>
parse_and_verify_hashed_job_manifest(
    const std::span<const std::byte> json) {
  if (json.empty() || json.size() > kMaximumJobManifestBytes) {
    return job_failure<VerifiedJobManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_TOO_LARGE,
        L"ジョブJSON読込み",
        L"ジョブJSONが空か、許容上限を超えています");
  }
  std::string text(json.size(), '\0');
  for (std::size_t index = 0; index < json.size(); ++index) {
    text[index] = static_cast<char>(
        std::to_integer<unsigned char>(json[index]));
  }

  JsonCursor cursor(text);
  if (!cursor.consume("{\"payload\":")) {
    return job_failure<VerifiedJobManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSON読込み",
        L"payloadがありません");
  }
  const std::size_t payload_start = cursor.position();
  auto manifest = parse_payload(cursor);
  if (!manifest) {
    return clonecore::Result<VerifiedJobManifest>::failure(
        manifest.error());
  }
  const std::size_t payload_end = cursor.position();
  if (!cursor.consume(",\"jobHashSha256\":")) {
    return job_failure<VerifiedJobManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSON読込み",
        L"jobHashSha256がありません");
  }
  const auto hash_text = cursor.parse_string();
  if (!hash_text || !cursor.consume("}") || !cursor.at_end()) {
    return job_failure<VerifiedJobManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSON読込み",
        L"ハッシュまたはJSON終端が不正です");
  }
  const auto expected_digest = digest_from_hex(hash_text.value());
  if (!expected_digest) {
    return clonecore::Result<VerifiedJobManifest>::failure(
        expected_digest.error());
  }
  const auto payload_bytes = std::as_bytes(std::span<const char>(
      text.data() + payload_start, payload_end - payload_start));
  const auto actual_digest = sha256(payload_bytes);
  if (!actual_digest) {
    return clonecore::Result<VerifiedJobManifest>::failure(
        actual_digest.error());
  }
  if (actual_digest.value() != expected_digest.value()) {
    return job_failure<VerifiedJobManifest>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"ジョブJSON SHA-256検証",
        L"ジョブ内容が作成時から変更されています");
  }

  const auto valid = validate_manifest(manifest.value());
  if (!valid) {
    return clonecore::Result<VerifiedJobManifest>::failure(valid.error());
  }
  const auto canonical =
      serialize_hashed_job_manifest(manifest.value());
  if (!canonical) {
    return clonecore::Result<VerifiedJobManifest>::failure(
        canonical.error());
  }
  if (!equal_bytes(json, canonical.value())) {
    return job_failure<VerifiedJobManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ジョブJSON正規形検証",
        L"未知項目、空白、項目順、または文字エスケープが正規形と一致しません");
  }
  return clonecore::Result<VerifiedJobManifest>::success(
      VerifiedJobManifest{
          .manifest = std::move(manifest.value()),
          .payload_hash = actual_digest.value()});
}

}  // namespace ytec::imageformat
