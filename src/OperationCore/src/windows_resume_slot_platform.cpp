#include "ytec/operationcore/windows_resume_slot_platform.h"

#include "sha256_internal.h"
#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::operationcore {
namespace {

constexpr std::uint16_t kResumeSlotMajor = 1U;
constexpr std::uint16_t kResumeSlotMinor = 0U;
constexpr std::size_t kMaximumPathCharacters = 32U * 1024U;
constexpr std::array<std::byte, 8U> kResumeSlotMagic{
    std::byte{0x59}, std::byte{0x54}, std::byte{0x45}, std::byte{0x43},
    std::byte{0x52}, std::byte{0x53}, std::byte{0x31}, std::byte{0x00}};
constexpr std::string_view kPartialIdentityDomain =
    "YTEC-RESUME-PARTIAL-FILE-ID-V1";

clonecore::Error platform_error(
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

clonecore::Status platform_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(platform_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

template <typename T>
clonecore::Result<T> result_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Result<T>::failure(platform_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool equals_ordinal_ignore_case(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  if (left.size() != right.size() ||
      left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return false;
  }
  return CompareStringOrdinal(
             left.data(),
             static_cast<int>(left.size()),
             right.data(),
             static_cast<int>(right.size()),
             TRUE) == CSTR_EQUAL;
}

bool operation_id_equal(
    const OperationId& left,
    const OperationId& right) noexcept {
  unsigned int difference = 0U;
  for (std::size_t index = 0U; index < left.size(); ++index) {
    difference |= std::to_integer<unsigned int>(left[index] ^ right[index]);
  }
  return difference == 0U;
}

bool identities_equal(
    const ResumeIdentityBinding& left,
    const ResumeIdentityBinding& right) noexcept {
  return detail::digest_equal(
             left.source_identity_hash, right.source_identity_hash) &&
         detail::digest_equal(
             left.target_identity_hash, right.target_identity_hash) &&
         detail::digest_equal(
             left.output_identity_hash, right.output_identity_hash);
}

bool partial_bindings_equal(
    const ResumeOwnedPartialBinding& left,
    const ResumeOwnedPartialBinding& right) noexcept {
  return operation_id_equal(left.operation_id, right.operation_id) &&
         identities_equal(left.identities, right.identities) &&
         detail::digest_equal(
             left.file_object_identity_hash,
             right.file_object_identity_hash);
}

bool optional_partial_bindings_equal(
    const std::optional<ResumeOwnedPartialBinding>& left,
    const std::optional<ResumeOwnedPartialBinding>& right) noexcept {
  if (left.has_value() != right.has_value()) {
    return false;
  }
  return !left || partial_bindings_equal(*left, *right);
}

bool records_equal(
    const ResumeSlotRecord& left,
    const ResumeSlotRecord& right) noexcept {
  return left.capability == right.capability &&
         operation_id_equal(
             left.checkpoint.checkpoint.operation_id,
             right.checkpoint.checkpoint.operation_id) &&
         identities_equal(left.identities, right.identities) &&
         detail::digest_equal(
             left.checkpoint.record_hash,
             right.checkpoint.record_hash) &&
         optional_partial_bindings_equal(
             left.owned_partial, right.owned_partial);
}

bool binding_equal(
    const ResumeSlotBinding& left,
    const ResumeSlotBinding& right) noexcept {
  if (left.capability != right.capability ||
      !operation_id_equal(left.operation_id, right.operation_id) ||
      !identities_equal(left.identities, right.identities) ||
      !detail::digest_equal(
          left.checkpoint_record_hash,
          right.checkpoint_record_hash) ||
      left.partial_file_object_identity_hash.has_value() !=
          right.partial_file_object_identity_hash.has_value()) {
    return false;
  }
  return !left.partial_file_object_identity_hash ||
         detail::digest_equal(
             *left.partial_file_object_identity_hash,
             *right.partial_file_object_identity_hash);
}

bool known_capability(const std::uint8_t value) noexcept {
  switch (static_cast<ResumeCapability>(value)) {
    case ResumeCapability::persistent_exact_restore:
    case ResumeCapability::persistent_rescue_restore:
    case ResumeCapability::same_process_only_vss_image_create:
    case ResumeCapability::same_process_only_vss_clone:
    case ResumeCapability::same_process_only_pe_image_create:
    case ResumeCapability::same_process_only_pe_clone:
    case ResumeCapability::unsupported_shrink_migration:
    case ResumeCapability::unsupported_raw_rescue:
      return true;
  }
  return false;
}

clonecore::Result<std::wstring> canonical_local_path(
    const std::wstring& path,
    const std::wstring_view operation) {
  if (path.size() < 3U || path.size() >= kMaximumPathCharacters ||
      std::iswalpha(static_cast<wint_t>(path[0])) == 0 ||
      path[1] != L':' || path[2] != L'\\' ||
      path.find(L'/') != std::wstring::npos ||
      path.find(L':', 2U) != std::wstring::npos ||
      path.find(L'\0') != std::wstring::npos ||
      path.ends_with(L"\\") || path.ends_with(L" ") ||
      path.ends_with(L".")) {
    return result_failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_BAD_PATHNAME,
        std::wstring(operation),
        L"正規化済みのローカル絶対パスが必要です");
  }

  std::size_t component_begin = 3U;
  while (component_begin < path.size()) {
    const std::size_t separator = path.find(L'\\', component_begin);
    const std::size_t component_end =
        separator == std::wstring::npos ? path.size() : separator;
    const std::wstring_view component(path.data() + component_begin,
                                      component_end - component_begin);
    if (component.empty() || component == L"." || component == L".." ||
        component.ends_with(L" ") || component.ends_with(L".")) {
      return result_failure<std::wstring>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_BAD_PATHNAME,
          std::wstring(operation),
          L"空要素、相対要素、末尾空白または末尾dotを含むパスは使用できません");
    }
    if (separator == std::wstring::npos) {
      break;
    }
    component_begin = separator + 1U;
  }

  std::vector<wchar_t> resolved(kMaximumPathCharacters, L'\0');
  wchar_t* file_part{};
  const DWORD length = GetFullPathNameW(
      path.c_str(),
      static_cast<DWORD>(resolved.size()),
      resolved.data(),
      &file_part);
  if (length == 0U || length >= resolved.size()) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            std::wstring(operation),
            length == 0U ? GetLastError() : ERROR_FILENAME_EXCED_RANGE));
  }
  const std::wstring canonical(resolved.data(), length);
  if (!equals_ordinal_ignore_case(path, canonical)) {
    return result_failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_BAD_PATHNAME,
        std::wstring(operation),
        L"相対要素または正規化差分を含むパスは使用できません");
  }
  return clonecore::Result<std::wstring>::success(canonical);
}

clonecore::Result<std::wstring> parent_path(
    const std::wstring& path,
    const std::wstring_view operation) {
  const std::size_t separator = path.find_last_of(L'\\');
  if (separator == std::wstring::npos || separator < 2U) {
    return result_failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_BAD_PATHNAME,
        std::wstring(operation),
        L"安全な親ディレクトリを導出できません");
  }
  if (separator == 2U) {
    return clonecore::Result<std::wstring>::success(path.substr(0U, 3U));
  }
  return clonecore::Result<std::wstring>::success(path.substr(0U, separator));
}

clonecore::Result<std::wstring> child_path(
    const std::wstring& parent,
    const std::wstring_view child,
    const std::wstring_view operation) {
  const bool parent_is_root = parent.ends_with(L"\\");
  const std::size_t separator_size = parent_is_root ? 0U : 1U;
  if (parent.size() + separator_size + child.size() >=
      kMaximumPathCharacters) {
    return result_failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_FILENAME_EXCED_RANGE,
        std::wstring(operation),
        L"導出パスがWindows上限を超えます");
  }
  return clonecore::Result<std::wstring>::success(
      parent + (parent_is_root ? L"" : L"\\") + std::wstring(child));
}

std::wstring extended_path(const std::wstring_view path) {
  return L"\\\\?\\" + std::wstring(path);
}

clonecore::Status verify_opened_path(
    const HANDLE handle,
    const std::wstring& expected,
    const std::wstring_view operation) {
  std::vector<wchar_t> actual(kMaximumPathCharacters, L'\0');
  const DWORD length = GetFinalPathNameByHandleW(
      handle,
      actual.data(),
      static_cast<DWORD>(actual.size()),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (length == 0U || length >= actual.size()) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::identity_mismatch,
        std::wstring(operation),
        length == 0U ? GetLastError() : ERROR_FILENAME_EXCED_RANGE));
  }
  if (!equals_ordinal_ignore_case(
          std::wstring_view(actual.data(), length),
          extended_path(expected))) {
    return platform_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_NAME,
        std::wstring(operation),
        L"opened handleの実体パスが固定パスと一致しません");
  }
  return clonecore::success_status();
}

clonecore::Status verify_regular_directory(
    const std::wstring& path,
    const std::wstring_view operation) {
  clonecore::UniqueHandle directory(CreateFileW(
      extended_path(path).c_str(),
      FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!directory) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        std::wstring(operation),
        GetLastError()));
  }
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!GetFileInformationByHandleEx(
          directory.get(),
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes))) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        std::wstring(operation),
        GetLastError()));
  }
  if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return platform_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REPARSE_TAG_INVALID,
        std::wstring(operation),
        L"通常の非reparseディレクトリではありません");
  }
  return verify_opened_path(directory.get(), path, operation);
}

clonecore::Status verify_directory_chain(
    const std::wstring& directory,
    const std::wstring_view operation) {
  std::size_t separator = directory.find(L'\\', 3U);
  while (separator != std::wstring::npos) {
    const auto status = verify_regular_directory(
        directory.substr(0U, separator), operation);
    if (!status) {
      return status;
    }
    separator = directory.find(L'\\', separator + 1U);
  }
  return verify_regular_directory(directory, operation);
}

clonecore::Status verify_regular_executable(
    const std::wstring& path) {
  clonecore::UniqueHandle executable(CreateFileW(
      extended_path(path).c_str(),
      FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!executable) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        L"Resume Slot EXE配置確認",
        GetLastError()));
  }
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!GetFileInformationByHandleEx(
          executable.get(),
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes))) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        L"Resume Slot EXE属性確認",
        GetLastError()));
  }
  if ((attributes.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
    return platform_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REPARSE_TAG_INVALID,
        L"Resume Slot EXE配置確認",
        L"通常の非reparseファイルではありません");
  }
  return verify_opened_path(
      executable.get(), path, L"Resume Slot EXE実体パス確認");
}

struct FileObservation final {
  std::uint64_t volume_serial{};
  std::array<std::byte, 16U> file_id{};
  std::uint64_t file_size{};
  std::uint64_t allocation_size{};
  std::uint32_t link_count{};
  LARGE_INTEGER last_write{};
  LARGE_INTEGER change_time{};
};

clonecore::Result<FileObservation> observe_regular_single_link_file(
    const HANDLE handle,
    const std::wstring_view operation) {
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  FILE_ID_INFO identifier{};
  FILE_STANDARD_INFO standard{};
  FILE_BASIC_INFO basic{};
  if (!GetFileInformationByHandleEx(
          handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
      !GetFileInformationByHandleEx(
          handle, FileIdInfo, &identifier, sizeof(identifier)) ||
      !GetFileInformationByHandleEx(
          handle, FileStandardInfo, &standard, sizeof(standard)) ||
      !GetFileInformationByHandleEx(
          handle, FileBasicInfo, &basic, sizeof(basic))) {
    return clonecore::Result<FileObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            std::wstring(operation),
            GetLastError()));
  }
  if ((attributes.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U ||
      standard.EndOfFile.QuadPart < 0 ||
      standard.AllocationSize.QuadPart < 0 ||
      standard.NumberOfLinks != 1U) {
    return result_failure<FileObservation>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REPARSE_TAG_INVALID,
        std::wstring(operation),
        L"通常の非reparse単一linkファイルとして識別できません");
  }
  FileObservation result{
      .volume_serial = identifier.VolumeSerialNumber,
      .file_size = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart),
      .allocation_size =
          static_cast<std::uint64_t>(standard.AllocationSize.QuadPart),
      .link_count = standard.NumberOfLinks,
      .last_write = basic.LastWriteTime,
      .change_time = basic.ChangeTime,
  };
  static_assert(sizeof(identifier.FileId.Identifier) == 16U);
  std::memcpy(
      result.file_id.data(),
      identifier.FileId.Identifier,
      result.file_id.size());
  return clonecore::Result<FileObservation>::success(result);
}

bool same_file_object(
    const FileObservation& left,
    const FileObservation& right) noexcept {
  return left.volume_serial == right.volume_serial &&
         left.file_id == right.file_id;
}

bool same_complete_observation(
    const FileObservation& left,
    const FileObservation& right) noexcept {
  return same_file_object(left, right) &&
         left.file_size == right.file_size &&
         left.allocation_size == right.allocation_size &&
         left.link_count == right.link_count &&
         left.last_write.QuadPart == right.last_write.QuadPart &&
         left.change_time.QuadPart == right.change_time.QuadPart;
}

void append_u8(std::vector<std::byte>& bytes, const std::uint8_t value) {
  bytes.push_back(static_cast<std::byte>(value));
}

void append_u16(std::vector<std::byte>& bytes, const std::uint16_t value) {
  bytes.push_back(static_cast<std::byte>(value & 0xffU));
  bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void append_u64(std::vector<std::byte>& bytes, const std::uint64_t value) {
  for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

template <std::size_t Size>
void append_array(
    std::vector<std::byte>& bytes,
    const std::array<std::byte, Size>& value) {
  bytes.insert(bytes.end(), value.begin(), value.end());
}

void set_u32(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    bytes[offset + (shift / 8U)] =
        static_cast<std::byte>((value >> shift) & 0xffU);
  }
}

void append_wstring(
    std::vector<std::byte>& bytes,
    const std::wstring& value) {
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  for (const wchar_t character : value) {
    append_u16(bytes, static_cast<std::uint16_t>(character));
  }
}

class Reader final {
 public:
  explicit Reader(const std::span<const std::byte> bytes) noexcept
      : bytes_(bytes) {}

  [[nodiscard]] bool read_u8(std::uint8_t& value) noexcept {
    if (!has(1U)) {
      return false;
    }
    value = std::to_integer<std::uint8_t>(bytes_[offset_++]);
    return true;
  }

  [[nodiscard]] bool read_u16(std::uint16_t& value) noexcept {
    if (!has(2U)) {
      return false;
    }
    value = static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes_[offset_]) |
        (std::to_integer<std::uint16_t>(bytes_[offset_ + 1U]) << 8U));
    offset_ += 2U;
    return true;
  }

  [[nodiscard]] bool read_u32(std::uint32_t& value) noexcept {
    if (!has(4U)) {
      return false;
    }
    value = 0U;
    for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
      value |= std::to_integer<std::uint32_t>(
                   bytes_[offset_ + (shift / 8U)])
               << shift;
    }
    offset_ += 4U;
    return true;
  }

  template <std::size_t Size>
  [[nodiscard]] bool read_array(
      std::array<std::byte, Size>& value) noexcept {
    if (!has(Size)) {
      return false;
    }
    std::copy_n(
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
        Size,
        value.begin());
    offset_ += Size;
    return true;
  }

  [[nodiscard]] bool read_span(
      const std::size_t size,
      std::span<const std::byte>& value) noexcept {
    if (!has(size)) {
      return false;
    }
    value = bytes_.subspan(offset_, size);
    offset_ += size;
    return true;
  }

  [[nodiscard]] bool read_wstring(
      std::wstring& value,
      const std::size_t maximum_characters) {
    std::uint32_t count{};
    if (!read_u32(count) || count == 0U ||
        count > maximum_characters ||
        static_cast<std::size_t>(count) >
            ((std::numeric_limits<std::size_t>::max)() / 2U) ||
        !has(static_cast<std::size_t>(count) * 2U)) {
      return false;
    }
    value.clear();
    value.reserve(count);
    for (std::uint32_t index = 0U; index < count; ++index) {
      std::uint16_t character{};
      if (!read_u16(character) || character == 0U) {
        return false;
      }
      value.push_back(static_cast<wchar_t>(character));
    }
    return true;
  }

  [[nodiscard]] bool at_end() const noexcept {
    return offset_ == bytes_.size();
  }

 private:
  [[nodiscard]] bool has(const std::size_t amount) const noexcept {
    return offset_ <= bytes_.size() && amount <= bytes_.size() - offset_;
  }

  std::span<const std::byte> bytes_;
  std::size_t offset_{};
};

clonecore::Result<Sha256Digest> partial_identity_hash(
    const FileObservation& observation) {
  std::vector<std::byte> bytes;
  bytes.reserve(kPartialIdentityDomain.size() + sizeof(std::uint64_t) +
                observation.file_id.size());
  for (const char character : kPartialIdentityDomain) {
    bytes.push_back(static_cast<std::byte>(character));
  }
  append_u64(bytes, observation.volume_serial);
  append_array(bytes, observation.file_id);
  return detail::sha256(bytes);
}

struct StoredSlot final {
  ResumeSlotRecord record;
  std::optional<std::wstring> partial_path;
  Sha256Digest envelope_hash{};
};

clonecore::Result<StoredSlot> parse_slot_envelope(
    const std::span<const std::byte> bytes) {
  constexpr std::size_t kHeaderBytes = 16U;
  constexpr std::size_t kMinimumPayloadBytes =
      1U + 1U + 2U + (3U * Sha256Digest{}.size()) + 4U;
  constexpr std::size_t kMinimumBytes =
      kHeaderBytes + kMinimumPayloadBytes + Sha256Digest{}.size();
  if (bytes.size() < kMinimumBytes ||
      bytes.size() > kMaximumWindowsResumeSlotBytes) {
    return result_failure<StoredSlot>(
        clonecore::ErrorCode::invalid_data,
        bytes.size() > kMaximumWindowsResumeSlotBytes
            ? ERROR_FILE_TOO_LARGE
            : ERROR_INVALID_DATA,
        L"Resume Slot envelope寸法",
        L"slot寸法が有界形式の範囲外です");
  }

  const std::span<const std::byte> authenticated =
      bytes.first(bytes.size() - Sha256Digest{}.size());
  Sha256Digest stored_hash{};
  std::copy_n(
      bytes.end() - static_cast<std::ptrdiff_t>(stored_hash.size()),
      stored_hash.size(),
      stored_hash.begin());
  const auto calculated_hash = detail::sha256(authenticated);
  if (!calculated_hash ||
      !detail::digest_equal(calculated_hash.value(), stored_hash)) {
    return result_failure<StoredSlot>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"Resume Slot envelope Hash",
        L"slot全体のSHA-256が一致しません");
  }

  Reader reader(authenticated);
  std::array<std::byte, kResumeSlotMagic.size()> magic{};
  std::uint16_t major{};
  std::uint16_t minor{};
  std::uint32_t total_size{};
  std::uint8_t capability{};
  std::uint8_t flags{};
  std::uint16_t reserved{};
  ResumeIdentityBinding identities{};
  std::uint32_t checkpoint_size{};
  std::span<const std::byte> checkpoint_bytes;
  if (!reader.read_array(magic) || magic != kResumeSlotMagic ||
      !reader.read_u16(major) || !reader.read_u16(minor) ||
      !reader.read_u32(total_size) || major != kResumeSlotMajor ||
      minor != kResumeSlotMinor || total_size != bytes.size() ||
      !reader.read_u8(capability) || !known_capability(capability) ||
      !reader.read_u8(flags) || (flags & ~0x01U) != 0U ||
      !reader.read_u16(reserved) || reserved != 0U ||
      !reader.read_array(identities.source_identity_hash) ||
      !reader.read_array(identities.target_identity_hash) ||
      !reader.read_array(identities.output_identity_hash) ||
      !reader.read_u32(checkpoint_size) || checkpoint_size == 0U ||
      checkpoint_size > kMaximumCheckpointBytes ||
      !reader.read_span(checkpoint_size, checkpoint_bytes)) {
    return result_failure<StoredSlot>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Resume Slot envelope構造",
        L"版、固定フィールド、flagsまたはcheckpoint寸法が不正です");
  }

  auto checkpoint = parse_checkpoint(checkpoint_bytes);
  if (!checkpoint) {
    return clonecore::Result<StoredSlot>::failure(checkpoint.error());
  }
  ResumeSlotRecord record{
      .capability = static_cast<ResumeCapability>(capability),
      .checkpoint = checkpoint.take_value(),
      .identities = identities,
      .owned_partial = std::nullopt,
  };
  std::optional<std::wstring> partial_path_value;
  if ((flags & 0x01U) != 0U) {
    ResumeOwnedPartialBinding partial{};
    std::wstring partial_path;
    if (!reader.read_array(partial.operation_id) ||
        !reader.read_array(partial.identities.source_identity_hash) ||
        !reader.read_array(partial.identities.target_identity_hash) ||
        !reader.read_array(partial.identities.output_identity_hash) ||
        !reader.read_array(partial.file_object_identity_hash) ||
        !reader.read_wstring(partial_path, kMaximumPathCharacters - 1U)) {
      return result_failure<StoredSlot>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Resume Slot owned partial構造",
          L"owned partialのbindingまたはpathが不正です");
    }
    auto canonical = canonical_local_path(
        partial_path, L"Resume Slot owned partial path");
    if (!canonical ||
        canonical.value().size() < 8U ||
        !equals_ordinal_ignore_case(
            std::wstring_view(canonical.value()).substr(
                canonical.value().size() - 8U),
            L".partial")) {
      return canonical
          ? result_failure<StoredSlot>(
                clonecore::ErrorCode::invalid_data,
                ERROR_BAD_PATHNAME,
                L"Resume Slot owned partial path",
                L"所有対象は.partial拡張子でなければなりません")
          : clonecore::Result<StoredSlot>::failure(canonical.error());
    }
    record.owned_partial = partial;
    partial_path_value = canonical.take_value();
  }
  if (!reader.at_end()) {
    return result_failure<StoredSlot>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Resume Slot envelope終端",
        L"未解釈または余分なフィールドがあります");
  }
  const auto valid = validate_resume_slot_record(record);
  if (!valid) {
    return clonecore::Result<StoredSlot>::failure(valid.error());
  }
  return clonecore::Result<StoredSlot>::success(StoredSlot{
      .record = std::move(record),
      .partial_path = std::move(partial_path_value),
      .envelope_hash = stored_hash,
  });
}

clonecore::Result<std::vector<std::byte>> serialize_slot_envelope(
    const ResumeSlotRecord& record,
    const std::optional<std::wstring>& partial_path) {
  const auto valid = validate_resume_slot_record(record);
  if (!valid) {
    return clonecore::Result<std::vector<std::byte>>::failure(valid.error());
  }
  if (record.owned_partial.has_value() != partial_path.has_value()) {
    return result_failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"Resume Slot envelope生成",
        L"owned partial bindingとpathの有無が一致しません");
  }
  std::optional<std::wstring> canonical_partial;
  if (partial_path) {
    auto canonical = canonical_local_path(
        *partial_path, L"Resume Slot owned partial path");
    if (!canonical || canonical.value().size() < 8U ||
        !equals_ordinal_ignore_case(
            std::wstring_view(canonical.value()).substr(
                canonical.value().size() - 8U),
            L".partial")) {
      return canonical
          ? result_failure<std::vector<std::byte>>(
                clonecore::ErrorCode::invalid_argument,
                ERROR_BAD_PATHNAME,
                L"Resume Slot owned partial path",
                L"所有対象は.partial拡張子でなければなりません")
          : clonecore::Result<std::vector<std::byte>>::failure(
                canonical.error());
    }
    canonical_partial = canonical.take_value();
  }

  auto checkpoint = serialize_checkpoint(record.checkpoint.checkpoint);
  if (!checkpoint) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        checkpoint.error());
  }
  std::vector<std::byte> bytes;
  bytes.reserve(16U + 100U + checkpoint.value().size() +
                (canonical_partial ? canonical_partial->size() * 2U + 152U
                                   : 0U) +
                Sha256Digest{}.size());
  append_array(bytes, kResumeSlotMagic);
  append_u16(bytes, kResumeSlotMajor);
  append_u16(bytes, kResumeSlotMinor);
  constexpr std::size_t kTotalSizeOffset = 12U;
  append_u32(bytes, 0U);
  append_u8(bytes, static_cast<std::uint8_t>(record.capability));
  append_u8(bytes, record.owned_partial ? 0x01U : 0U);
  append_u16(bytes, 0U);
  append_array(bytes, record.identities.source_identity_hash);
  append_array(bytes, record.identities.target_identity_hash);
  append_array(bytes, record.identities.output_identity_hash);
  append_u32(
      bytes, static_cast<std::uint32_t>(checkpoint.value().size()));
  bytes.insert(
      bytes.end(), checkpoint.value().begin(), checkpoint.value().end());
  if (record.owned_partial) {
    append_array(bytes, record.owned_partial->operation_id);
    append_array(bytes, record.owned_partial->identities.source_identity_hash);
    append_array(bytes, record.owned_partial->identities.target_identity_hash);
    append_array(bytes, record.owned_partial->identities.output_identity_hash);
    append_array(bytes, record.owned_partial->file_object_identity_hash);
    append_wstring(bytes, *canonical_partial);
  }
  if (bytes.size() >
      kMaximumWindowsResumeSlotBytes - Sha256Digest{}.size()) {
    return result_failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_TOO_LARGE,
        L"Resume Slot envelope生成",
        L"slot内容が安全上限を超えます");
  }
  const std::size_t total_size = bytes.size() + Sha256Digest{}.size();
  set_u32(bytes, kTotalSizeOffset, static_cast<std::uint32_t>(total_size));
  const auto hash = detail::sha256(bytes);
  if (!hash) {
    return clonecore::Result<std::vector<std::byte>>::failure(hash.error());
  }
  append_array(bytes, hash.value());
  return clonecore::Result<std::vector<std::byte>>::success(std::move(bytes));
}

clonecore::Status seek_begin(
    const HANDLE handle,
    const std::wstring_view operation) {
  LARGE_INTEGER beginning{};
  if (!SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN)) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        std::wstring(operation),
        GetLastError()));
  }
  return clonecore::success_status();
}

clonecore::Result<std::vector<std::byte>> read_bounded_file(
    const HANDLE handle,
    const std::size_t maximum_bytes,
    const std::wstring_view operation) {
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(handle, &size)) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            std::wstring(operation),
            GetLastError()));
  }
  if (size.QuadPart <= 0 ||
      static_cast<unsigned long long>(size.QuadPart) > maximum_bytes) {
    return result_failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_data,
        size.QuadPart > 0 ? ERROR_FILE_TOO_LARGE : ERROR_INVALID_DATA,
        std::wstring(operation),
        L"ファイル寸法が安全上限外です");
  }
  const auto positioned = seek_begin(handle, operation);
  if (!positioned) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        positioned.error());
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
  std::size_t consumed = 0U;
  while (consumed < bytes.size()) {
    const DWORD amount = static_cast<DWORD>((std::min)(
        bytes.size() - consumed,
        static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
    DWORD read{};
    if (!ReadFile(
            handle,
            bytes.data() + consumed,
            amount,
            &read,
            nullptr) ||
        read == 0U) {
      const DWORD native_code = GetLastError();
      return clonecore::Result<std::vector<std::byte>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              std::wstring(operation),
              native_code == ERROR_SUCCESS ? ERROR_HANDLE_EOF : native_code));
    }
    consumed += read;
  }
  return clonecore::Result<std::vector<std::byte>>::success(std::move(bytes));
}

struct ReadSlot final {
  StoredSlot stored;
  FileObservation file;
};

clonecore::Result<std::optional<ReadSlot>> read_slot_file(
    const std::wstring& path) {
  clonecore::UniqueHandle file(CreateFileW(
      extended_path(path).c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  if (!file) {
    const DWORD native_code = GetLastError();
    if (native_code == ERROR_FILE_NOT_FOUND ||
        native_code == ERROR_PATH_NOT_FOUND) {
      return clonecore::Result<std::optional<ReadSlot>>::success(
          std::nullopt);
    }
    return clonecore::Result<std::optional<ReadSlot>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"Resume Slot checkpointを開く",
            native_code));
  }
  auto before = observe_regular_single_link_file(
      file.get(), L"Resume Slot checkpoint属性確認");
  if (!before) {
    return clonecore::Result<std::optional<ReadSlot>>::failure(
        before.error());
  }
  const auto path_matches = verify_opened_path(
      file.get(), path, L"Resume Slot checkpoint実体パス確認");
  if (!path_matches) {
    return clonecore::Result<std::optional<ReadSlot>>::failure(
        path_matches.error());
  }
  auto bytes = read_bounded_file(
      file.get(),
      kMaximumWindowsResumeSlotBytes,
      L"Resume Slot checkpoint有界読取り");
  if (!bytes) {
    return clonecore::Result<std::optional<ReadSlot>>::failure(bytes.error());
  }
  auto after = observe_regular_single_link_file(
      file.get(), L"Resume Slot checkpoint読取り後属性確認");
  if (!after || !same_complete_observation(before.value(), after.value())) {
    return after
        ? result_failure<std::optional<ReadSlot>>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"Resume Slot checkpoint読取り後再識別",
              L"File ID、寸法、link数または時刻が読取り中に変化しました")
        : clonecore::Result<std::optional<ReadSlot>>::failure(after.error());
  }
  auto stored = parse_slot_envelope(bytes.value());
  if (!stored) {
    return clonecore::Result<std::optional<ReadSlot>>::failure(
        stored.error());
  }
  return clonecore::Result<std::optional<ReadSlot>>::success(
      std::optional<ReadSlot>(ReadSlot{
          .stored = stored.take_value(),
          .file = after.value(),
      }));
}

struct PartialObservation final {
  WindowsResumeOwnedPartial partial;
  FileObservation file;
};

clonecore::Result<PartialObservation> observe_owned_partial(
    const std::wstring& canonical_path,
    const OperationId& operation_id,
    const ResumeIdentityBinding& identities) {
  auto parent = parent_path(
      canonical_path, L"Resume Slot owned partial親path");
  if (!parent) {
    return clonecore::Result<PartialObservation>::failure(parent.error());
  }
  const auto parents = verify_directory_chain(
      parent.value(), L"Resume Slot owned partial親chain確認");
  if (!parents) {
    return clonecore::Result<PartialObservation>::failure(parents.error());
  }

  clonecore::UniqueHandle file(CreateFileW(
      extended_path(canonical_path).c_str(),
      FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!file) {
    return clonecore::Result<PartialObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::identity_mismatch,
            L"Resume Slot owned partialを開く",
            GetLastError()));
  }
  auto before = observe_regular_single_link_file(
      file.get(), L"Resume Slot owned partial属性確認");
  if (!before) {
    return clonecore::Result<PartialObservation>::failure(before.error());
  }
  const auto actual_path = verify_opened_path(
      file.get(), canonical_path, L"Resume Slot owned partial実体パス確認");
  if (!actual_path) {
    return clonecore::Result<PartialObservation>::failure(actual_path.error());
  }
  auto after = observe_regular_single_link_file(
      file.get(), L"Resume Slot owned partial再識別");
  if (!after || !same_complete_observation(before.value(), after.value())) {
    return after
        ? result_failure<PartialObservation>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"Resume Slot owned partial再識別",
              L"File ID、寸法、link数または時刻が観測中に変化しました")
        : clonecore::Result<PartialObservation>::failure(after.error());
  }
  auto hash = partial_identity_hash(after.value());
  if (!hash) {
    return clonecore::Result<PartialObservation>::failure(hash.error());
  }
  const auto parents_after = verify_directory_chain(
      parent.value(), L"Resume Slot owned partial親chain再確認");
  if (!parents_after) {
    return clonecore::Result<PartialObservation>::failure(
        parents_after.error());
  }
  return clonecore::Result<PartialObservation>::success(PartialObservation{
      .partial = {
          .canonical_path = canonical_path,
          .binding = {
              .operation_id = operation_id,
              .identities = identities,
              .file_object_identity_hash = hash.value(),
          },
      },
      .file = after.value(),
  });
}

void set_delete_pending(const HANDLE handle, const bool pending) {
  FILE_DISPOSITION_INFO disposition{};
  disposition.DeleteFile = pending ? TRUE : FALSE;
  if (!SetFileInformationByHandle(
          handle,
          FileDispositionInfo,
          &disposition,
          sizeof(disposition))) {
    throw GetLastError();
  }
}

clonecore::Status mark_delete_pending(
    const HANDLE handle,
    const bool pending,
    const std::wstring_view operation) {
  try {
    set_delete_pending(handle, pending);
    return clonecore::success_status();
  } catch (const DWORD native_code) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        std::wstring(operation),
        native_code));
  }
}

struct StagedSlot final {
  FileObservation file;
  Sha256Digest envelope_hash{};
};

clonecore::Result<StagedSlot> write_verified_stage(
    const std::wstring& stage_path,
    const std::span<const std::byte> bytes,
    const ResumeSlotRecord& expected_record,
    const std::optional<std::wstring>& expected_partial_path) {
  clonecore::UniqueHandle stage(CreateFileW(
      extended_path(stage_path).c_str(),
      GENERIC_READ | GENERIC_WRITE | DELETE,
      0,
      nullptr,
      CREATE_NEW,
      FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_WRITE_THROUGH,
      nullptr));
  if (!stage) {
    return clonecore::Result<StagedSlot>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"Resume Slot stage CREATE_NEW",
            GetLastError()));
  }
  const auto fail_owned = [&stage](const clonecore::Error& error) {
    static_cast<void>(mark_delete_pending(
        stage.get(), true, L"Resume Slot stage失敗後破棄"));
    stage.reset();
    return clonecore::Result<StagedSlot>::failure(error);
  };
  const auto opened_path = verify_opened_path(
      stage.get(), stage_path, L"Resume Slot stage実体パス確認");
  if (!opened_path) {
    return fail_owned(opened_path.error());
  }
  auto initial = observe_regular_single_link_file(
      stage.get(), L"Resume Slot stage初期属性確認");
  if (!initial) {
    return fail_owned(initial.error());
  }

  std::size_t consumed = 0U;
  while (consumed < bytes.size()) {
    const DWORD amount = static_cast<DWORD>((std::min)(
        bytes.size() - consumed,
        static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
    DWORD written{};
    if (!WriteFile(
            stage.get(),
            bytes.data() + consumed,
            amount,
            &written,
            nullptr) ||
        written == 0U) {
      const DWORD native_code = GetLastError();
      return fail_owned(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"Resume Slot stage書込み",
          native_code == ERROR_SUCCESS ? ERROR_WRITE_FAULT : native_code));
    }
    consumed += written;
  }
  if (!FlushFileBuffers(stage.get())) {
    return fail_owned(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"Resume Slot stage flush",
        GetLastError()));
  }
  auto readback = read_bounded_file(
      stage.get(),
      kMaximumWindowsResumeSlotBytes,
      L"Resume Slot stage同一handle読戻し");
  if (!readback) {
    return fail_owned(readback.error());
  }
  auto parsed = parse_slot_envelope(readback.value());
  if (!parsed || !records_equal(parsed.value().record, expected_record) ||
      parsed.value().partial_path != expected_partial_path) {
    return fail_owned(parsed
        ? platform_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_CRC,
              L"Resume Slot stage読戻し",
              L"読戻したrecordまたはowned partial pathが一致しません")
        : parsed.error());
  }
  auto final = observe_regular_single_link_file(
      stage.get(), L"Resume Slot stage書込み後属性確認");
  if (!final || !same_file_object(initial.value(), final.value())) {
    return fail_owned(final
        ? platform_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"Resume Slot stage書込み後再識別",
              L"CREATE_NEWしたstageのFile IDが変化しました")
        : final.error());
  }
  const StagedSlot result{
      .file = final.value(),
      .envelope_hash = parsed.value().envelope_hash,
  };
  stage.reset();
  return clonecore::Result<StagedSlot>::success(result);
}

void discard_exact_stage(
    const std::wstring& stage_path,
    const StagedSlot& expected) noexcept {
  try {
    clonecore::UniqueHandle stage(CreateFileW(
        extended_path(stage_path).c_str(),
        GENERIC_READ | DELETE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!stage) {
      return;
    }
    auto observed = observe_regular_single_link_file(
        stage.get(), L"Resume Slot exact stage cleanup属性");
    if (!observed || !same_complete_observation(expected.file, observed.value())) {
      return;
    }
    auto bytes = read_bounded_file(
        stage.get(),
        kMaximumWindowsResumeSlotBytes,
        L"Resume Slot exact stage cleanup読取り");
    if (!bytes) {
      return;
    }
    auto parsed = parse_slot_envelope(bytes.value());
    if (!parsed ||
        !detail::digest_equal(
            parsed.value().envelope_hash, expected.envelope_hash)) {
      return;
    }
    static_cast<void>(mark_delete_pending(
        stage.get(), true, L"Resume Slot exact stage cleanup"));
  } catch (...) {
  }
}

struct ConfiguredPaths final {
  std::wstring executable;
  std::wstring application_directory;
  std::wstring data_directory;
  std::wstring checkpoint;
  std::wstring stage;
};

clonecore::Result<ConfiguredPaths> configure_paths(
    const std::wstring& executable_path) {
  auto executable = canonical_local_path(
      executable_path, L"Resume Slot EXE path");
  if (!executable) {
    return clonecore::Result<ConfiguredPaths>::failure(executable.error());
  }
  auto application = parent_path(
      executable.value(), L"Resume Slot application path");
  if (!application) {
    return clonecore::Result<ConfiguredPaths>::failure(application.error());
  }
  auto data = child_path(
      application.value(), L"data", L"Resume Slot data path");
  if (!data) {
    return clonecore::Result<ConfiguredPaths>::failure(data.error());
  }
  auto checkpoint = child_path(
      data.value(), kResumeSlotFileName, L"Resume Slot checkpoint path");
  if (!checkpoint) {
    return clonecore::Result<ConfiguredPaths>::failure(checkpoint.error());
  }
  auto stage = child_path(
      data.value(), L"active.checkpoint.new", L"Resume Slot stage path");
  if (!stage) {
    return clonecore::Result<ConfiguredPaths>::failure(stage.error());
  }
  const auto application_safe = verify_directory_chain(
      application.value(), L"Resume Slot application parent chain");
  if (!application_safe) {
    return clonecore::Result<ConfiguredPaths>::failure(
        application_safe.error());
  }
  const auto executable_safe = verify_regular_executable(executable.value());
  if (!executable_safe) {
    return clonecore::Result<ConfiguredPaths>::failure(executable_safe.error());
  }
  const auto data_safe = verify_directory_chain(
      data.value(), L"Resume Slot EXE隣data chain");
  if (!data_safe) {
    return clonecore::Result<ConfiguredPaths>::failure(data_safe.error());
  }
  return clonecore::Result<ConfiguredPaths>::success(ConfiguredPaths{
      .executable = executable.take_value(),
      .application_directory = application.take_value(),
      .data_directory = data.take_value(),
      .checkpoint = checkpoint.take_value(),
      .stage = stage.take_value(),
  });
}

struct PlatformState final {
  ResumeSlotObservation observation;
  std::optional<ReadSlot> checkpoint;
  std::optional<PartialObservation> partial;
  std::optional<std::wstring> partial_path;
};

class WindowsResumeSlotPlatform final : public IResumeSlotPlatform {
 public:
  WindowsResumeSlotPlatform(
      ConfiguredPaths paths,
      WindowsResumeDataBackingProbe backing_probe,
      std::optional<WindowsResumeOwnedPartial> create_partial)
      : paths_(std::move(paths)),
        backing_probe_(std::move(backing_probe)),
        create_partial_(std::move(create_partial)) {}

  [[nodiscard]] clonecore::Result<ResumeSlotObservation>
  observe_fixed_slot() override {
    auto state = load_state(false);
    if (!state) {
      return clonecore::Result<ResumeSlotObservation>::failure(state.error());
    }
    return clonecore::Result<ResumeSlotObservation>::success(
        std::move(state.value().observation));
  }

  [[nodiscard]] clonecore::Status create_fixed_slot(
      const ResumeSlotRecord& record) override {
    const auto valid = validate_resume_slot_record(record);
    if (!valid) {
      return valid;
    }
    auto before = load_state(true);
    if (!before) {
      return clonecore::Status::failure(before.error());
    }
    if (before.value().checkpoint) {
      return platform_failure(
          clonecore::ErrorCode::access_denied,
          ERROR_FILE_EXISTS,
          L"Resume Slot CREATE_NEW前確認",
          L"既存active.checkpointは上書きしません");
    }
    if (record.owned_partial.has_value() !=
        before.value().observation.observed_owned_partial.has_value() ||
        (record.owned_partial &&
         !partial_bindings_equal(
             *record.owned_partial,
             *before.value().observation.observed_owned_partial))) {
      return platform_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"Resume Slot CREATE_NEW owned partial確認",
          L"宣言したowned partialをopened handleで再確認できません");
    }
    const std::optional<std::wstring> partial_path = record.owned_partial
        ? std::optional<std::wstring>(
              before.value().partial->partial.canonical_path)
        : std::nullopt;
    auto bytes = serialize_slot_envelope(record, partial_path);
    if (!bytes) {
      return clonecore::Status::failure(bytes.error());
    }
    auto stage = write_verified_stage(
        paths_.stage, bytes.value(), record, partial_path);
    if (!stage) {
      return clonecore::Status::failure(stage.error());
    }
    auto last = load_state(true);
    if (!last || last.value().checkpoint ||
        (record.owned_partial &&
         (!last.value().partial ||
          !partial_bindings_equal(
              *record.owned_partial,
              last.value().partial->partial.binding)))) {
      discard_exact_stage(paths_.stage, stage.value());
      return last
          ? platform_failure(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"Resume Slot CREATE_NEW直前再識別",
                L"checkpointまたはowned partialが直前に変化しました")
          : clonecore::Status::failure(last.error());
    }
    if (!MoveFileExW(
            extended_path(paths_.stage).c_str(),
            extended_path(paths_.checkpoint).c_str(),
            MOVEFILE_WRITE_THROUGH)) {
      const DWORD native_code = GetLastError();
      discard_exact_stage(paths_.stage, stage.value());
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"Resume Slot CREATE_NEW確定",
          native_code));
    }
    auto committed = load_state(true);
    if (!committed || !committed.value().checkpoint ||
        !records_equal(committed.value().checkpoint->stored.record, record) ||
        committed.value().checkpoint->stored.partial_path != partial_path ||
        !same_file_object(
            committed.value().checkpoint->file, stage.value().file)) {
      return committed
          ? platform_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"Resume Slot CREATE_NEW確定後読戻し",
                L"確定したslotを完全一致で再確認できません")
          : clonecore::Status::failure(committed.error());
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status replace_fixed_slot(
      const Sha256Digest& expected_checkpoint_record_hash,
      const ResumeSlotRecord& next) override {
    const auto valid = validate_resume_slot_record(next);
    if (!valid) {
      return valid;
    }
    auto current = load_state(true);
    if (!current) {
      return clonecore::Status::failure(current.error());
    }
    const auto relationship = validate_replacement(
        current.value(), expected_checkpoint_record_hash, next);
    if (!relationship) {
      return relationship;
    }
    const std::optional<std::wstring> partial_path =
        current.value().checkpoint->stored.partial_path;
    auto bytes = serialize_slot_envelope(next, partial_path);
    if (!bytes) {
      return clonecore::Status::failure(bytes.error());
    }
    auto stage = write_verified_stage(
        paths_.stage, bytes.value(), next, partial_path);
    if (!stage) {
      return clonecore::Status::failure(stage.error());
    }
    auto last = load_state(true);
    if (!last) {
      discard_exact_stage(paths_.stage, stage.value());
      return clonecore::Status::failure(last.error());
    }
    const auto last_relationship = validate_replacement(
        last.value(), expected_checkpoint_record_hash, next);
    if (!last_relationship ||
        !same_complete_observation(
            current.value().checkpoint->file,
            last.value().checkpoint->file)) {
      discard_exact_stage(paths_.stage, stage.value());
      return last_relationship
          ? platform_failure(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"Resume Slot atomic replace直前再識別",
                L"checkpoint File ID、寸法、link数または時刻が変化しました")
          : last_relationship;
    }
    if (!ReplaceFileW(
            extended_path(paths_.checkpoint).c_str(),
            extended_path(paths_.stage).c_str(),
            nullptr,
            REPLACEFILE_WRITE_THROUGH,
            nullptr,
            nullptr)) {
      const DWORD native_code = GetLastError();
      discard_exact_stage(paths_.stage, stage.value());
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"Resume Slot atomic replace",
          native_code));
    }
    auto committed = load_state(true);
    if (!committed || !committed.value().checkpoint ||
        !records_equal(committed.value().checkpoint->stored.record, next) ||
        committed.value().checkpoint->stored.partial_path != partial_path ||
        !same_file_object(
            committed.value().checkpoint->file, stage.value().file)) {
      return committed
          ? platform_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"Resume Slot atomic replace後読戻し",
                L"置換したslotを完全一致で再確認できません")
          : clonecore::Status::failure(committed.error());
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status
  discard_fixed_slot_and_owned_partial(
      const ResumeSlotBinding& binding) override {
    auto state = load_state(false);
    if (!state) {
      return clonecore::Status::failure(state.error());
    }
    if (!state.value().checkpoint) {
      return platform_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_NOT_FOUND,
          L"Resume Slot guarded discard",
          L"拘束済みcheckpointが存在しません");
    }
    auto actual = make_resume_slot_binding(
        state.value().checkpoint->stored.record);
    if (!actual || !binding_equal(actual.value(), binding)) {
      return platform_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"Resume Slot guarded discard binding",
          L"checkpointの完全bindingがreview済みbindingと一致しません");
    }

    clonecore::UniqueHandle checkpoint(CreateFileW(
        extended_path(paths_.checkpoint).c_str(),
        GENERIC_READ | DELETE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!checkpoint) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::identity_mismatch,
          L"Resume Slot guarded discard checkpoint open",
          GetLastError()));
    }
    auto checkpoint_before = observe_regular_single_link_file(
        checkpoint.get(), L"Resume Slot guarded discard checkpoint属性");
    if (!checkpoint_before ||
        !same_complete_observation(
            state.value().checkpoint->file, checkpoint_before.value())) {
      return checkpoint_before
          ? platform_failure(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"Resume Slot guarded discard checkpoint再識別",
                L"checkpointがreview後に変化しました")
          : clonecore::Status::failure(checkpoint_before.error());
    }
    auto checkpoint_bytes = read_bounded_file(
        checkpoint.get(),
        kMaximumWindowsResumeSlotBytes,
        L"Resume Slot guarded discard checkpoint読取り");
    if (!checkpoint_bytes) {
      return clonecore::Status::failure(checkpoint_bytes.error());
    }
    auto checkpoint_record = parse_slot_envelope(checkpoint_bytes.value());
    if (!checkpoint_record) {
      return clonecore::Status::failure(checkpoint_record.error());
    }
    auto rebound = make_resume_slot_binding(checkpoint_record.value().record);
    if (!rebound || !binding_equal(rebound.value(), binding)) {
      return platform_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"Resume Slot guarded discard checkpoint再解析",
          L"削除handleのrecordがreview済みbindingと一致しません");
    }

    clonecore::UniqueHandle partial;
    std::optional<FileObservation> partial_before;
    if (binding.partial_file_object_identity_hash) {
      if (!checkpoint_record.value().partial_path ||
          !state.value().partial) {
        return platform_failure(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_FILE_NOT_FOUND,
            L"Resume Slot guarded discard partial",
            L"checkpointが所有するpartialを再openできません");
      }
      partial.reset(CreateFileW(
          extended_path(*checkpoint_record.value().partial_path).c_str(),
          FILE_READ_ATTRIBUTES | DELETE,
          FILE_SHARE_READ,
          nullptr,
          OPEN_EXISTING,
          FILE_FLAG_OPEN_REPARSE_POINT,
          nullptr));
      if (!partial) {
        return clonecore::Status::failure(clonecore::make_win32_error(
            clonecore::ErrorCode::identity_mismatch,
            L"Resume Slot guarded discard partial open",
            GetLastError()));
      }
      auto observed = observe_regular_single_link_file(
          partial.get(), L"Resume Slot guarded discard partial属性");
      if (!observed ||
          !same_complete_observation(
              state.value().partial->file, observed.value())) {
        return observed
            ? platform_failure(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_FILE_INVALID,
                  L"Resume Slot guarded discard partial再識別",
                  L"owned partialがreview後に変化しました")
            : clonecore::Status::failure(observed.error());
      }
      auto hash = partial_identity_hash(observed.value());
      if (!hash || !detail::digest_equal(
                       hash.value(),
                       *binding.partial_file_object_identity_hash)) {
        return hash
            ? platform_failure(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_FILE_INVALID,
                  L"Resume Slot guarded discard partial File ID",
                  L"owned partialのFile ID Hashが一致しません")
            : clonecore::Status::failure(hash.error());
      }
      partial_before = observed.value();
    }

    const auto paths_stable = verify_runtime_paths();
    if (!paths_stable) {
      return paths_stable;
    }
    const auto backing = require_backing_proof(
        checkpoint_record.value().record, false);
    if (!backing) {
      return backing;
    }
    auto checkpoint_after = observe_regular_single_link_file(
        checkpoint.get(), L"Resume Slot guarded discard直前checkpoint再識別");
    if (!checkpoint_after || !same_complete_observation(
                                 checkpoint_before.value(),
                                 checkpoint_after.value())) {
      return checkpoint_after
          ? platform_failure(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"Resume Slot guarded discard直前checkpoint再識別",
                L"checkpointが削除直前に変化しました")
          : clonecore::Status::failure(checkpoint_after.error());
    }
    if (partial) {
      auto partial_after = observe_regular_single_link_file(
          partial.get(), L"Resume Slot guarded discard直前partial再識別");
      if (!partial_after || !same_complete_observation(
                                *partial_before, partial_after.value())) {
        return partial_after
            ? platform_failure(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_FILE_INVALID,
                  L"Resume Slot guarded discard直前partial再識別",
                  L"owned partialが削除直前に変化しました")
            : clonecore::Status::failure(partial_after.error());
      }
    }

    const auto checkpoint_pending = mark_delete_pending(
        checkpoint.get(), true, L"Resume Slot checkpoint delete-pending");
    if (!checkpoint_pending) {
      return checkpoint_pending;
    }
    bool partial_pending = false;
    if (partial) {
      const auto pending = mark_delete_pending(
          partial.get(), true, L"Resume Slot owned partial delete-pending");
      if (!pending) {
        const auto rollback = mark_delete_pending(
            checkpoint.get(), false, L"Resume Slot checkpoint delete rollback");
        if (!rollback) {
          return platform_failure(
              clonecore::ErrorCode::io_failed,
              rollback.error().native_code,
              L"Resume Slot guarded discard rollback",
              pending.error().message + L" / " + rollback.error().message);
        }
        return pending;
      }
      partial_pending = true;
    }
    checkpoint.reset();
    partial.reset();
    if (partial_pending) {
      create_partial_.reset();
    }

    auto after = load_state(false);
    if (!after || after.value().checkpoint || after.value().partial) {
      return after
          ? platform_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_FILE_INVALID,
                L"Resume Slot guarded discard後確認",
                L"checkpointまたはowned partialが削除後も残っています")
          : clonecore::Status::failure(after.error());
    }
    return clonecore::success_status();
  }

 private:
  [[nodiscard]] clonecore::Status verify_runtime_paths() const {
    const auto application = verify_directory_chain(
        paths_.application_directory,
        L"Resume Slot application chain再確認");
    if (!application) {
      return application;
    }
    const auto executable = verify_regular_executable(paths_.executable);
    if (!executable) {
      return executable;
    }
    return verify_directory_chain(
        paths_.data_directory,
        L"Resume Slot EXE隣data chain再確認");
  }

  [[nodiscard]] clonecore::Status require_backing_proof(
      const std::optional<ResumeSlotRecord>& record,
      const bool require_source_separation) {
    clonecore::Result<WindowsResumeDataBackingProof> proof = [&]() {
      try {
        return backing_probe_(paths_.data_directory, record);
      } catch (...) {
        return result_failure<WindowsResumeDataBackingProof>(
            clonecore::ErrorCode::internal_error,
            ERROR_UNHANDLED_EXCEPTION,
            L"Resume Slot data backing proof",
            L"呼出側proof callbackが例外を送出しました");
      }
    }();
    if (!proof) {
      return clonecore::Status::failure(proof.error());
    }
    if (!proof.value().identity_from_open_handle ||
        (require_source_separation &&
         !proof.value().separated_from_source) ||
        detail::digest_is_zero(
            proof.value().backing_storage_identity_hash)) {
      return platform_failure(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Resume Slot data backing proof",
        require_source_separation
            ? L"opened handle由来の非ゼロ識別とsource backing分離を証明できません"
            : L"opened handle由来の非ゼロdata backing識別を証明できません");
    }
    if (bound_backing_identity_ &&
        !detail::digest_equal(
            *bound_backing_identity_,
            proof.value().backing_storage_identity_hash)) {
      return platform_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"Resume Slot data backing再識別",
          L"adapter初回観測後にdata backing identityが変化しました");
    }
    if (!bound_backing_identity_) {
      bound_backing_identity_ = proof.value().backing_storage_identity_hash;
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Result<PlatformState> load_state(
      const bool require_source_separation) {
    const auto before_paths = verify_runtime_paths();
    if (!before_paths) {
      return clonecore::Result<PlatformState>::failure(before_paths.error());
    }
    auto checkpoint = read_slot_file(paths_.checkpoint);
    if (!checkpoint) {
      return clonecore::Result<PlatformState>::failure(checkpoint.error());
    }
    const std::optional<ResumeSlotRecord> record = checkpoint.value()
        ? std::optional<ResumeSlotRecord>(checkpoint.value()->stored.record)
        : std::nullopt;
    const auto backing = require_backing_proof(
        record, require_source_separation);
    if (!backing) {
      return clonecore::Result<PlatformState>::failure(backing.error());
    }

    std::optional<PartialObservation> partial;
    std::optional<std::wstring> partial_path;
    if (checkpoint.value() && checkpoint.value()->stored.record.owned_partial) {
      if (!checkpoint.value()->stored.partial_path) {
        return result_failure<PlatformState>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"Resume Slot persisted partial path",
            L"owned partial bindingに対応するpathがありません");
      }
      if (create_partial_ &&
          (!equals_ordinal_ignore_case(
               create_partial_->canonical_path,
               *checkpoint.value()->stored.partial_path) ||
           !partial_bindings_equal(
               create_partial_->binding,
               *checkpoint.value()->stored.record.owned_partial))) {
        return result_failure<PlatformState>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_FILE_INVALID,
            L"Resume Slot persisted/create partial競合",
            L"persist済みpartialとcreate用partialが一致しません");
      }
      auto observed = observe_owned_partial(
          *checkpoint.value()->stored.partial_path,
          checkpoint.value()->stored.record.owned_partial->operation_id,
          checkpoint.value()->stored.record.owned_partial->identities);
      if (!observed) {
        return clonecore::Result<PlatformState>::failure(observed.error());
      }
      if (!partial_bindings_equal(
              observed.value().partial.binding,
              *checkpoint.value()->stored.record.owned_partial)) {
        return result_failure<PlatformState>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_FILE_INVALID,
            L"Resume Slot persisted partial File ID",
            L"persist済みowned partialが別file objectへ差し替えられました");
      }
      partial_path = checkpoint.value()->stored.partial_path;
      partial = observed.take_value();
    } else if (create_partial_) {
      auto observed = observe_owned_partial(
          create_partial_->canonical_path,
          create_partial_->binding.operation_id,
          create_partial_->binding.identities);
      if (!observed) {
        return clonecore::Result<PlatformState>::failure(observed.error());
      }
      if (!partial_bindings_equal(
              observed.value().partial.binding,
              create_partial_->binding)) {
        return result_failure<PlatformState>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_FILE_INVALID,
            L"Resume Slot create partial File ID",
            L"create用owned partialが別file objectへ差し替えられました");
      }
      partial_path = create_partial_->canonical_path;
      partial = observed.take_value();
    }

    if (partial_path &&
        (equals_ordinal_ignore_case(*partial_path, paths_.checkpoint) ||
         equals_ordinal_ignore_case(*partial_path, paths_.stage))) {
      return result_failure<PlatformState>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_INVALID_NAME,
          L"Resume Slot checkpoint/partial分離",
          L"checkpoint、stage、owned partialは別pathでなければなりません");
    }
    const auto after_paths = verify_runtime_paths();
    if (!after_paths) {
      return clonecore::Result<PlatformState>::failure(after_paths.error());
    }
    ResumeSlotObservation observation{
        .storage = {
            .checkpoint_path = paths_.checkpoint,
            .paths_are_canonical_local = true,
            .parent_chain_reparse_free = true,
            .placement_separated_from_source = true,
            .checkpoint_and_partial_paths_distinct = true,
            .checkpoint_file = {
                .exists = checkpoint.value().has_value(),
                .is_regular_file = checkpoint.value().has_value(),
                .is_reparse_free = checkpoint.value().has_value(),
                .hard_link_count = checkpoint.value() ? 1U : 0U,
            },
            .owned_partial_file = {
                .exists = partial.has_value(),
                .is_regular_file = partial.has_value(),
                .is_reparse_free = partial.has_value(),
                .hard_link_count = partial ? 1U : 0U,
            },
        },
        .slot = record,
        .observed_owned_partial = partial
            ? std::optional<ResumeOwnedPartialBinding>(partial->partial.binding)
            : std::nullopt,
    };
    return clonecore::Result<PlatformState>::success(PlatformState{
        .observation = std::move(observation),
        .checkpoint = checkpoint.take_value(),
        .partial = std::move(partial),
        .partial_path = std::move(partial_path),
    });
  }

  [[nodiscard]] clonecore::Status validate_replacement(
      const PlatformState& state,
      const Sha256Digest& expected_checkpoint_record_hash,
      const ResumeSlotRecord& next) const {
    if (!state.checkpoint ||
        !detail::digest_equal(
            state.checkpoint->stored.record.checkpoint.record_hash,
            expected_checkpoint_record_hash)) {
      return platform_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"Resume Slot replace前record Hash",
          L"既存checkpointがreview済みrecord Hashと一致しません");
    }
    const ResumeSlotRecord& current = state.checkpoint->stored.record;
    if (current.capability != next.capability ||
        !identities_equal(current.identities, next.identities) ||
        !optional_partial_bindings_equal(
            current.owned_partial, next.owned_partial)) {
      return platform_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"Resume Slot replace immutable binding",
          L"capability、source/target/outputまたはowned partialが変化しました");
    }
    return validate_checkpoint_transition(
        current.checkpoint.checkpoint,
        next.checkpoint.checkpoint);
  }

  ConfiguredPaths paths_;
  WindowsResumeDataBackingProbe backing_probe_;
  std::optional<WindowsResumeOwnedPartial> create_partial_;
  std::optional<Sha256Digest> bound_backing_identity_;
};

clonecore::Result<std::wstring> current_executable_path() {
  std::vector<wchar_t> buffer(1024U, L'\0');
  for (;;) {
    SetLastError(ERROR_SUCCESS);
    const DWORD copied = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (copied == 0U) {
      return clonecore::Result<std::wstring>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"Resume Slot current EXE path",
              GetLastError()));
    }
    if (copied < buffer.size()) {
      return clonecore::Result<std::wstring>::success(
          std::wstring(buffer.data(), copied));
    }
    if (buffer.size() >= kMaximumPathCharacters) {
      return result_failure<std::wstring>(
          clonecore::ErrorCode::query_failed,
          ERROR_INSUFFICIENT_BUFFER,
          L"Resume Slot current EXE path",
          L"現在のEXE完全pathを有界bufferで取得できません");
    }
    buffer.resize(
        (std::min)(buffer.size() * 2U, kMaximumPathCharacters), L'\0');
  }
}

}  // namespace

clonecore::Result<WindowsResumeOwnedPartial>
bind_windows_resume_owned_partial(
    const std::wstring& path,
    const OperationId& operation_id,
    const ResumeIdentityBinding& identities) {
  try {
    auto canonical = canonical_local_path(
        path, L"Resume Slot owned partial binding path");
    if (!canonical) {
      return clonecore::Result<WindowsResumeOwnedPartial>::failure(
          canonical.error());
    }
    if (canonical.value().size() < 8U ||
        !equals_ordinal_ignore_case(
            std::wstring_view(canonical.value()).substr(
                canonical.value().size() - 8U),
            L".partial")) {
      return result_failure<WindowsResumeOwnedPartial>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_BAD_PATHNAME,
          L"Resume Slot owned partial binding path",
          L"所有対象は.partial拡張子でなければなりません");
    }
    auto observed = observe_owned_partial(
        canonical.value(), operation_id, identities);
    if (!observed) {
      return clonecore::Result<WindowsResumeOwnedPartial>::failure(
          observed.error());
    }
    return clonecore::Result<WindowsResumeOwnedPartial>::success(
        std::move(observed.value().partial));
  } catch (const std::bad_alloc&) {
    return result_failure<WindowsResumeOwnedPartial>(
        clonecore::ErrorCode::io_failed,
        ERROR_NOT_ENOUGH_MEMORY,
        L"Resume Slot owned partial binding",
        L"有界識別に必要なメモリを確保できません");
  } catch (...) {
    return result_failure<WindowsResumeOwnedPartial>(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"Resume Slot owned partial binding",
        L"owned partialを安全に識別できません");
  }
}

clonecore::Result<std::unique_ptr<IResumeSlotPlatform>>
make_windows_resume_slot_platform(
    WindowsResumeSlotPlatformOptions options) {
  try {
    if (!options.prove_data_backing_separation) {
      return result_failure<std::unique_ptr<IResumeSlotPlatform>>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"Resume Slot platform構成",
          L"data backing分離proof callbackが必要です");
    }
    auto paths = configure_paths(options.executable_path);
    if (!paths) {
      return clonecore::Result<std::unique_ptr<IResumeSlotPlatform>>::failure(
          paths.error());
    }
    if (options.owned_partial_for_create) {
      auto canonical = canonical_local_path(
          options.owned_partial_for_create->canonical_path,
          L"Resume Slot create partial path");
      if (!canonical) {
        return clonecore::Result<std::unique_ptr<IResumeSlotPlatform>>::failure(
            canonical.error());
      }
      options.owned_partial_for_create->canonical_path =
          canonical.take_value();
      auto observed = observe_owned_partial(
          options.owned_partial_for_create->canonical_path,
          options.owned_partial_for_create->binding.operation_id,
          options.owned_partial_for_create->binding.identities);
      if (!observed ||
          !partial_bindings_equal(
              observed.value().partial.binding,
              options.owned_partial_for_create->binding)) {
        return observed
            ? result_failure<std::unique_ptr<IResumeSlotPlatform>>(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_FILE_INVALID,
                  L"Resume Slot create partial初期再識別",
                  L"create用partialがbinding後に変化しました")
            : clonecore::Result<std::unique_ptr<IResumeSlotPlatform>>::failure(
                  observed.error());
      }
      if (equals_ordinal_ignore_case(
              options.owned_partial_for_create->canonical_path,
              paths.value().checkpoint) ||
          equals_ordinal_ignore_case(
              options.owned_partial_for_create->canonical_path,
              paths.value().stage)) {
        return result_failure<std::unique_ptr<IResumeSlotPlatform>>(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_INVALID_NAME,
            L"Resume Slot checkpoint/partial分離",
            L"checkpoint、stage、owned partialは別pathでなければなりません");
      }
    }
    std::unique_ptr<IResumeSlotPlatform> platform =
        std::make_unique<WindowsResumeSlotPlatform>(
            paths.take_value(),
            std::move(options.prove_data_backing_separation),
            std::move(options.owned_partial_for_create));
    return clonecore::Result<std::unique_ptr<IResumeSlotPlatform>>::success(
        std::move(platform));
  } catch (const std::bad_alloc&) {
    return result_failure<std::unique_ptr<IResumeSlotPlatform>>(
        clonecore::ErrorCode::io_failed,
        ERROR_NOT_ENOUGH_MEMORY,
        L"Resume Slot platform構成",
        L"adapter構成に必要なメモリを確保できません");
  } catch (...) {
    return result_failure<std::unique_ptr<IResumeSlotPlatform>>(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"Resume Slot platform構成",
        L"production adapterを安全に構成できません");
  }
}

clonecore::Result<std::unique_ptr<IResumeSlotPlatform>>
make_current_executable_windows_resume_slot_platform(
    WindowsResumeDataBackingProbe prove_data_backing_separation,
    std::optional<WindowsResumeOwnedPartial> owned_partial_for_create) {
  try {
    auto executable = current_executable_path();
    if (!executable) {
      return clonecore::Result<std::unique_ptr<IResumeSlotPlatform>>::failure(
          executable.error());
    }
    return make_windows_resume_slot_platform({
        .executable_path = executable.take_value(),
        .prove_data_backing_separation =
            std::move(prove_data_backing_separation),
        .owned_partial_for_create = std::move(owned_partial_for_create),
    });
  } catch (const std::bad_alloc&) {
    return result_failure<std::unique_ptr<IResumeSlotPlatform>>(
        clonecore::ErrorCode::io_failed,
        ERROR_NOT_ENOUGH_MEMORY,
        L"Resume Slot current EXE platform構成",
        L"adapter構成に必要なメモリを確保できません");
  } catch (...) {
    return result_failure<std::unique_ptr<IResumeSlotPlatform>>(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"Resume Slot current EXE platform構成",
        L"current EXE用adapterを安全に構成できません");
  }
}

}  // namespace ytec::operationcore
