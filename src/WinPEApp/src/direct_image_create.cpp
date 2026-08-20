#include "ytec/winpeapp/direct_image_create.h"

#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/imageformat/tsumugi.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ytec::winpeapp {
namespace {

constexpr std::uint32_t kDirectImageChunkBytes =
    imageformat::kImageChunkSize16MiB;

clonecore::Error image_error(
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
clonecore::Result<T> failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Result<T>::failure(image_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
    return false;
  }
  result = left + right;
  return true;
}

bool checked_multiply(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (left != 0U &&
      right > (std::numeric_limits<std::uint64_t>::max)() / left) {
    return false;
  }
  result = left * right;
  return true;
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
  for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void append_u64(std::vector<std::byte>& bytes, const std::uint64_t value) {
  for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void append_domain(
    std::vector<std::byte>& bytes,
    const std::string_view domain) {
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(domain.data()),
      reinterpret_cast<const std::byte*>(domain.data() + domain.size()));
  bytes.push_back(std::byte{0});
}

clonecore::Status append_ascii(
    std::vector<std::byte>& bytes,
    const std::string_view value,
    const std::wstring_view operation) {
  if (value.size() > (std::numeric_limits<std::uint32_t>::max)()) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_ARITHMETIC_OVERFLOW,
        std::wstring(operation),
        L"文字列が識別Hashの上限を超えています"));
  }
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(value.data()),
      reinterpret_cast<const std::byte*>(value.data() + value.size()));
  return clonecore::success_status();
}

clonecore::Status append_utf16(
    std::vector<std::byte>& bytes,
    const std::wstring_view value,
    const std::wstring_view operation) {
  static_assert(sizeof(wchar_t) == sizeof(std::uint16_t));
  if (value.size() > (std::numeric_limits<std::uint32_t>::max)()) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_ARITHMETIC_OVERFLOW,
        std::wstring(operation),
        L"文字列が識別Hashの上限を超えています"));
  }
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  for (const wchar_t code_unit : value) {
    const auto value16 = static_cast<std::uint16_t>(code_unit);
    bytes.push_back(static_cast<std::byte>(value16 & 0xffU));
    bytes.push_back(static_cast<std::byte>((value16 >> 8U) & 0xffU));
  }
  return clonecore::success_status();
}

bool ends_with_tsumugi(const std::wstring_view path) noexcept {
  constexpr std::wstring_view extension = L".tsumugi";
  return path.size() > extension.size() &&
      _wcsnicmp(
          path.data() + path.size() - extension.size(),
          extension.data(),
          extension.size()) == 0;
}

bool same_partition(
    const diskmodel::PartitionInfo& left,
    const diskmodel::PartitionInfo& right) {
  return left.number == right.number &&
      left.offset_bytes == right.offset_bytes &&
      left.size_bytes == right.size_bytes && left.style == right.style &&
      left.type == right.type && left.identifier == right.identifier &&
      left.name == right.name && left.bootable == right.bootable;
}

bool same_reviewed_layout(
    const diskmodel::DiskInfo& reviewed,
    const diskmodel::DiskInfo& observed) {
  if (reviewed.physical_sector_size != observed.physical_sector_size ||
      reviewed.partition_style != observed.partition_style ||
      reviewed.partitions.size() != observed.partitions.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < reviewed.partitions.size(); ++index) {
    if (!same_partition(reviewed.partitions[index], observed.partitions[index])) {
      return false;
    }
  }
  return true;
}

bool same_partition_geometry(
    std::vector<diskmodel::PartitionInfo> inventory,
    std::vector<std::pair<std::uint64_t, std::uint64_t>> parsed) {
  if (inventory.size() != parsed.size()) {
    return false;
  }
  std::sort(
      inventory.begin(), inventory.end(), [](const auto& left, const auto& right) {
        return left.offset_bytes < right.offset_bytes;
      });
  std::sort(parsed.begin(), parsed.end());
  for (std::size_t index = 0U; index < inventory.size(); ++index) {
    if (inventory[index].offset_bytes != parsed[index].first ||
        inventory[index].size_bytes != parsed[index].second) {
      return false;
    }
  }
  return true;
}

clonecore::Status validate_gpt_inventory_geometry(
    const diskmodel::DiskInfo& inventory,
    const clonecore::GptDisk& layout) {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> parsed;
  parsed.reserve(layout.partitions.size());
  for (const auto& partition : layout.partitions) {
    std::uint64_t offset{};
    std::uint64_t sectors{};
    std::uint64_t length{};
    if (partition.last_lba < partition.first_lba ||
        !checked_multiply(
            partition.first_lba, layout.logical_sector_size, offset) ||
        !checked_add(
            partition.last_lba - partition.first_lba, 1U, sectors) ||
        !checked_multiply(sectors, layout.logical_sector_size, length)) {
      return clonecore::Status::failure(image_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"PE Tsumugi GPTレビュー照合",
          L"RAW解析したGPT範囲が不正です"));
    }
    parsed.emplace_back(offset, length);
  }
  if (!same_partition_geometry(inventory.partitions, std::move(parsed))) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"PE Tsumugi GPTレビュー照合",
        L"レビュー済み一覧とRAW解析したGPTパーティション範囲が一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_mbr_inventory_geometry(
    const diskmodel::DiskInfo& inventory,
    const clonecore::MbrDisk& layout) {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> parsed;
  parsed.reserve(layout.partitions.size());
  for (const auto& partition : layout.partitions) {
    std::uint64_t offset{};
    std::uint64_t length{};
    if (!checked_multiply(
            partition.first_lba, layout.logical_sector_size, offset) ||
        !checked_multiply(
            partition.sector_count, layout.logical_sector_size, length)) {
      return clonecore::Status::failure(image_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"PE Tsumugi MBRレビュー照合",
          L"RAW解析したMBR範囲が不正です"));
    }
    parsed.emplace_back(offset, length);
  }
  if (!same_partition_geometry(inventory.partitions, std::move(parsed))) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"PE Tsumugi MBRレビュー照合",
        L"レビュー済み一覧とRAW解析したMBRパーティション範囲が一致しません"));
  }
  return clonecore::success_status();
}

struct SourceIdentityHashes final {
  imageformat::Sha256Digest model{};
  imageformat::Sha256Digest serial{};
  imageformat::Sha256Digest state{};
};

clonecore::Result<imageformat::Sha256Digest> hash_source_model(
    const std::wstring_view model) {
  return imageformat::hash_tsumugi_source_model_v1(model);
}

clonecore::Result<imageformat::Sha256Digest> hash_source_serial(
    const std::string_view serial_suffix,
    const std::wstring_view device_instance_id) {
  return imageformat::hash_tsumugi_source_serial_v1(
      serial_suffix, device_instance_id);
}

clonecore::Result<SourceIdentityHashes> make_source_hashes(
    const clonecore::StableDiskIdentity& source,
    const std::uint32_t physical_sector_size,
    const std::span<const std::byte> partition_snapshot) {
  const auto identity = clonecore::validate_stable_identity(
      source, source, L"PE Tsumugiコピー元");
  if (!identity ||
      !imageformat::is_supported_sector_size_pair(
          source.logical_sector_size, physical_sector_size) ||
      partition_snapshot.empty()) {
    return clonecore::Result<SourceIdentityHashes>::failure(
        identity
            ? image_error(
                  clonecore::ErrorCode::invalid_argument,
                  ERROR_INVALID_PARAMETER,
                  L"PE Tsumugi Source状態Hash",
                  L"物理セクターまたはパーティションsnapshotがありません")
            : identity.error());
  }
  const auto snapshot = imageformat::inspect_partition_snapshot_v1(
      partition_snapshot);
  if (!snapshot || snapshot.value().source_disk_size != source.size_bytes ||
      snapshot.value().logical_sector_size != source.logical_sector_size) {
    return clonecore::Result<SourceIdentityHashes>::failure(
        snapshot
            ? image_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_INVALID_DATA,
                  L"PE Tsumugi Source状態Hash",
                  L"安定識別とpartition snapshotの寸法が一致しません")
            : snapshot.error());
  }
  auto model = hash_source_model(source.model);
  if (!model) {
    return clonecore::Result<SourceIdentityHashes>::failure(model.error());
  }
  auto serial = hash_source_serial(
      source.serial_suffix, source.device_instance_id);
  if (!serial) {
    return clonecore::Result<SourceIdentityHashes>::failure(serial.error());
  }

  std::vector<std::byte> material;
  material.reserve(128U + partition_snapshot.size());
  append_domain(material, "YTEC-TSUMUGI-LOCKED-SOURCE-STATE-V1");
  const auto model_appended = append_utf16(
      material, source.model, L"PE Tsumugi Source状態Hash");
  const auto serial_appended = append_ascii(
      material, source.serial_suffix, L"PE Tsumugi Source状態Hash");
  const auto instance_appended = append_utf16(
      material, source.device_instance_id, L"PE Tsumugi Source状態Hash");
  if (!model_appended || !serial_appended || !instance_appended) {
    return clonecore::Result<SourceIdentityHashes>::failure(
        !model_appended
            ? model_appended.error()
            : !serial_appended ? serial_appended.error()
                               : instance_appended.error());
  }
  append_u64(material, source.size_bytes);
  append_u32(material, source.logical_sector_size);
  append_u32(material, physical_sector_size);
  material.push_back(source.is_system_disk ? std::byte{1} : std::byte{0});
  append_u64(
      material, static_cast<std::uint64_t>(partition_snapshot.size()));
  material.insert(
      material.end(), partition_snapshot.begin(), partition_snapshot.end());
  auto state = imageformat::sha256(material);
  if (!state) {
    return clonecore::Result<SourceIdentityHashes>::failure(state.error());
  }
  return clonecore::Result<SourceIdentityHashes>::success({
      .model = model.take_value(),
      .serial = serial.take_value(),
      .state = state.take_value(),
  });
}

class LockedSourceSession final
    : public imageformat::ITsumugiImageSourceSession {
 public:
  LockedSourceSession(
      std::unique_ptr<clonecore::ISourceDiskReader> reader,
      SourceIdentityHashes hashes) noexcept
      : reader_(std::move(reader)), hashes_(std::move(hashes)) {}

  std::uint64_t size_bytes() const noexcept override {
    return reader_ ? reader_->size_bytes() : 0U;
  }

  std::uint32_t logical_sector_size() const noexcept override {
    return reader_ ? reader_->logical_sector_size() : 0U;
  }

  clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (!reader_) {
      return failure<std::vector<std::byte>>(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_HANDLE,
          L"PE Tsumugi Source読取り",
          L"読取り専用Sourceセッションがありません");
    }
    return reader_->read(offset, length);
  }

  imageformat::Sha256Digest source_model_hash() const noexcept override {
    return hashes_.model;
  }

  imageformat::Sha256Digest source_serial_hash() const noexcept override {
    return hashes_.serial;
  }

  imageformat::Sha256Digest source_state_hash() const noexcept override {
    return hashes_.state;
  }

 private:
  std::unique_ptr<clonecore::ISourceDiskReader> reader_;
  SourceIdentityHashes hashes_;
};

imageformat::TsumugiManifestPartitionRole gpt_role(
    const clonecore::GptGuid& type) noexcept {
  if (type == clonecore::gpt_type_efi_system()) {
    return imageformat::TsumugiManifestPartitionRole::efi_system;
  }
  if (type == clonecore::gpt_type_microsoft_reserved()) {
    return imageformat::TsumugiManifestPartitionRole::microsoft_reserved;
  }
  if (type == clonecore::gpt_type_windows_recovery()) {
    return imageformat::TsumugiManifestPartitionRole::recovery;
  }
  if (type == clonecore::gpt_type_basic_data()) {
    return imageformat::TsumugiManifestPartitionRole::data;
  }
  return imageformat::TsumugiManifestPartitionRole::other;
}

imageformat::TsumugiManifestFileSystem gpt_file_system(
    const clonecore::GptGuid& type) noexcept {
  if (type == clonecore::gpt_type_microsoft_reserved()) {
    return imageformat::TsumugiManifestFileSystem::none;
  }
  // A partition type does not prove the on-disk filesystem. Exact mode reads
  // the full raw range, so keep it unknown until a separate bounded
  // filesystem probe has positively identified it.
  return imageformat::TsumugiManifestFileSystem::unknown;
}

clonecore::Status append_partition_chunks(
    std::vector<imageformat::TsumugiStreamBuildChunk>& chunks,
    const std::uint64_t offset,
    const std::uint64_t length,
    const imageformat::ITsumugiImageSourceSession& source) {
  std::uint64_t position = 0U;
  while (position < length) {
    if (chunks.size() >= imageformat::kTsumugiMaximumChunkCount) {
      return clonecore::Status::failure(image_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_TOO_MANY_OPEN_FILES,
          L"PE Tsumugiチャンク計画",
          L"画像チャンク数が形式上限を超えます"));
    }
    const auto amount = (std::min)(
        static_cast<std::uint64_t>(kDirectImageChunkBytes),
        length - position);
    std::uint64_t current{};
    if (!checked_add(offset, position, current)) {
      return clonecore::Status::failure(image_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"PE Tsumugiチャンク範囲",
          L"コピー元範囲が64bit上限を超えます"));
    }
    chunks.push_back(imageformat::TsumugiStreamBuildChunk{
        .logical_offset = current,
        .logical_length = amount,
        .source_offset = current,
        .flags = imageformat::TsumugiChunkFlags::none,
        .source = &source,
    });
    position += amount;
  }
  return clonecore::success_status();
}

struct PreparedExactImage final {
  imageformat::TsumugiManifest manifest;
  std::vector<imageformat::TsumugiStreamBuildChunk> chunks;
  std::uint64_t logical_payload_bytes{};
};

clonecore::Result<PreparedExactImage> prepare_gpt_image(
    const DirectImageCreateRequest& request,
    const diskmodel::ReadOnlyPhysicalDiskHandle& source,
    const clonecore::GptDisk& layout,
    std::vector<std::byte> partition_snapshot,
    const SourceIdentityHashes& hashes,
    const imageformat::ITsumugiImageSourceSession& session) {
  imageformat::TsumugiManifest manifest{
      .mode = imageformat::TsumugiManifestMode::exact,
      .partition_style = imageformat::TsumugiManifestPartitionStyle::gpt,
      .flags = imageformat::TsumugiManifestFlags::none,
      .source_disk_size = session.size_bytes(),
      .logical_sector_size = session.logical_sector_size(),
      .physical_sector_size = source.observed.observed.physical_sector_size,
      .source_model_hash = hashes.model,
      .source_serial_hash = hashes.serial,
      .source_state_hash = hashes.state,
      .created_utc = request.created_utc,
      .app_version = request.app_version,
      .partition_snapshot = std::move(partition_snapshot),
  };
  auto partitions = layout.partitions;
  std::sort(
      partitions.begin(), partitions.end(), [](const auto& left, const auto& right) {
        return left.first_lba < right.first_lba;
      });
  PreparedExactImage result{.manifest = std::move(manifest)};
  result.manifest.partitions.reserve(partitions.size());
  for (const auto& item : partitions) {
    if (item.entry_index == (std::numeric_limits<std::uint32_t>::max)() ||
        item.last_lba < item.first_lba) {
      return failure<PreparedExactImage>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"PE Tsumugi GPTパーティション",
          L"GPTパーティション番号または範囲が不正です");
    }
    std::uint64_t offset{};
    std::uint64_t sectors{};
    std::uint64_t length{};
    if (!checked_multiply(
            item.first_lba, layout.logical_sector_size, offset) ||
        !checked_add(item.last_lba - item.first_lba, 1U, sectors) ||
        !checked_multiply(sectors, layout.logical_sector_size, length) ||
        !checked_add(result.logical_payload_bytes, length,
                     result.logical_payload_bytes)) {
      return failure<PreparedExactImage>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"PE Tsumugi GPTパーティション範囲",
          L"GPTパーティション範囲が64bit上限を超えます");
    }
    auto flags = imageformat::TsumugiManifestPartitionFlags::selected;
    imageformat::TsumugiManifestPartition partition{
        .source_table_index = item.entry_index + 1U,
        .source_partition_number = item.entry_index + 1U,
        .role = gpt_role(item.type_guid),
        .file_system = gpt_file_system(item.type_guid),
        .flags = flags,
        .source_offset = offset,
        .source_size = length,
        .used_bytes = length,
        .minimum_target_bytes = length,
        .planned_target_bytes = length,
        .payload_logical_offset = offset,
        .payload_logical_length = length,
        .type_id = item.type_guid.bytes,
        .unique_id = item.unique_guid.bytes,
    };
    result.manifest.partitions.push_back(std::move(partition));
    const auto appended = append_partition_chunks(
        result.chunks, offset, length, session);
    if (!appended) {
      return clonecore::Result<PreparedExactImage>::failure(
          appended.error());
    }
  }
  return clonecore::Result<PreparedExactImage>::success(std::move(result));
}

clonecore::Result<PreparedExactImage> prepare_mbr_image(
    const DirectImageCreateRequest& request,
    const diskmodel::ReadOnlyPhysicalDiskHandle& source,
    const clonecore::MbrDisk& layout,
    std::vector<std::byte> partition_snapshot,
    const SourceIdentityHashes& hashes,
    const imageformat::ITsumugiImageSourceSession& session) {
  imageformat::TsumugiManifest manifest{
      .mode = imageformat::TsumugiManifestMode::exact,
      .partition_style = imageformat::TsumugiManifestPartitionStyle::mbr,
      .flags = imageformat::TsumugiManifestFlags::none,
      .source_disk_size = session.size_bytes(),
      .logical_sector_size = session.logical_sector_size(),
      .physical_sector_size = source.observed.observed.physical_sector_size,
      .source_model_hash = hashes.model,
      .source_serial_hash = hashes.serial,
      .source_state_hash = hashes.state,
      .created_utc = request.created_utc,
      .app_version = request.app_version,
      .partition_snapshot = std::move(partition_snapshot),
  };
  auto partitions = layout.partitions;
  std::sort(
      partitions.begin(), partitions.end(), [](const auto& left, const auto& right) {
        return left.first_lba < right.first_lba;
      });
  PreparedExactImage result{.manifest = std::move(manifest)};
  result.manifest.partitions.reserve(partitions.size());
  for (const auto& item : partitions) {
    std::uint64_t offset{};
    std::uint64_t length{};
    if (!checked_multiply(
            item.first_lba, layout.logical_sector_size, offset) ||
        !checked_multiply(
            item.sector_count, layout.logical_sector_size, length) ||
        !checked_add(result.logical_payload_bytes, length,
                     result.logical_payload_bytes)) {
      return failure<PreparedExactImage>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"PE Tsumugi MBRパーティション範囲",
          L"MBRパーティション範囲が64bit上限を超えます");
    }
    auto flags = imageformat::TsumugiManifestPartitionFlags::selected;
    if (item.active) {
      flags = flags | imageformat::TsumugiManifestPartitionFlags::active;
    }
    imageformat::TsumugiManifestPartition partition{
        .source_table_index = static_cast<std::uint32_t>(item.table_index) + 1U,
        .source_partition_number =
            static_cast<std::uint32_t>(item.table_index) + 1U,
        .role = imageformat::TsumugiManifestPartitionRole::data,
        .file_system = imageformat::TsumugiManifestFileSystem::unknown,
        .flags = flags,
        .source_offset = offset,
        .source_size = length,
        .used_bytes = length,
        .minimum_target_bytes = length,
        .planned_target_bytes = length,
        .payload_logical_offset = offset,
        .payload_logical_length = length,
    };
    partition.type_id[0] = static_cast<std::byte>(item.type);
    result.manifest.partitions.push_back(std::move(partition));
    const auto appended = append_partition_chunks(
        result.chunks, offset, length, session);
    if (!appended) {
      return clonecore::Result<PreparedExactImage>::failure(
          appended.error());
    }
  }
  return clonecore::Result<PreparedExactImage>::success(std::move(result));
}

clonecore::Result<std::uint64_t> maximum_image_bytes(
    const PreparedExactImage& plan,
    const bool rescue_mode) {
  auto manifest = imageformat::build_tsumugi_manifest_v1(plan.manifest);
  if (!manifest) {
    return clonecore::Result<std::uint64_t>::failure(manifest.error());
  }
  std::uint64_t records{};
  std::uint64_t total = imageformat::kTsumugiHeaderSize;
  const std::uint64_t record_count = rescue_mode
      ? imageformat::kTsumugiMaximumChunkCount
      : static_cast<std::uint64_t>(plan.chunks.size());
  if (!checked_multiply(
          record_count, imageformat::kTsumugiChunkRecordSize,
          records) ||
      !checked_add(total, imageformat::kTsumugiMetadataHeaderSize, total) ||
      !checked_add(total, manifest.value().size(), total) ||
      !checked_add(total, records, total) ||
      !checked_add(total, plan.logical_payload_bytes, total) ||
      !checked_add(total, imageformat::kTsumugiFooterSize, total)) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"PE Tsumugi最大画像寸法",
        L"画像の最大寸法が64bit上限を超えます");
  }
  return clonecore::Result<std::uint64_t>::success(total);
}

clonecore::Status validate_request(
    const DirectImageCreateRequest& request,
    const DirectImageCreateDependencies& dependencies) {
  if (!ends_with_tsumugi(request.final_path) ||
      request.created_utc.empty() || request.app_version.empty() ||
      !imageformat::is_supported_tsumugi_create_verification_mode(
          request.verification_mode) ||
      (request.selected_source.partition_style !=
           diskmodel::PartitionStyle::gpt &&
       request.selected_source.partition_style !=
           diskmodel::PartitionStyle::mbr)) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"PE直接Tsumugi作成要求",
        L"GPT/MBRコピー元、絶対.tsumugiパス、作成日時、アプリ版が必要です"));
  }
  if (request.rescue_mode &&
      request.selected_source.logical_sector_size != 512U) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE救出Tsumugiの論理セクター",
        L"実媒体検証が完了するまで、救出イメージ作成は512バイト論理セクターだけに限定します"));
  }
  if (!dependencies.set_source_read_only ||
      !dependencies.open_read_only_source ||
      !dependencies.query_destination_file_system ||
      !dependencies.validate_destination ||
      (request.rescue_mode && !dependencies.make_rescue_staging)) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"PE直接Tsumugi依存境界",
        L"コピー元保護、読取り専用Source、または保存先検証境界がありません"));
  }
  return clonecore::success_status();
}

clonecore::DiskOperationProgress translate_rescue_progress(
    const clonecore::RescueCopyProgress& progress) {
  clonecore::DiskOperationStage stage =
      clonecore::DiskOperationStage::copying_data;
  if (progress.phase == clonecore::RescueCopyPhase::validating) {
    stage = clonecore::DiskOperationStage::verifying_source;
  } else if (progress.phase == clonecore::RescueCopyPhase::flushing) {
    stage = clonecore::DiskOperationStage::flushing_data;
  }
  return clonecore::DiskOperationProgress{
      .stage = stage,
      .partition_index = std::nullopt,
      .total_read_bytes = progress.source_extent_bytes,
      .total_write_bytes = progress.source_extent_bytes,
      .total_verify_bytes = progress.source_extent_bytes,
      .read_bytes = progress.settled_target_bytes,
      .written_bytes = progress.settled_target_bytes,
      .verified_bytes = progress.settled_target_bytes,
      .cancellation_allowed = progress.cancellation_allowed,
      .pause_allowed = progress.pause_allowed,
  };
}

clonecore::Result<imageformat::TsumugiImageStorageFileSystem>
query_destination_file_system_with_windows_apis(const std::wstring& path) {
  std::vector<wchar_t> full(32768U, L'\0');
  const DWORD length = GetFullPathNameW(
      path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
  if (length == 0U || length >= full.size()) {
    return clonecore::Result<
        imageformat::TsumugiImageStorageFileSystem>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"PE Tsumugi保存先絶対パス取得",
            length == 0U ? GetLastError() : ERROR_BUFFER_OVERFLOW));
  }
  std::array<wchar_t, MAX_PATH + 1U> root{};
  if (!GetVolumePathNameW(
          full.data(), root.data(), static_cast<DWORD>(root.size()))) {
    return clonecore::Result<
        imageformat::TsumugiImageStorageFileSystem>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"PE Tsumugi保存先Volume取得",
            GetLastError()));
  }
  std::array<wchar_t, 32U> file_system{};
  if (!GetVolumeInformationW(
          root.data(), nullptr, 0U, nullptr, nullptr, nullptr,
          file_system.data(), static_cast<DWORD>(file_system.size()))) {
    return clonecore::Result<
        imageformat::TsumugiImageStorageFileSystem>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"PE Tsumugi保存先ファイルシステム取得",
            GetLastError()));
  }
  if (_wcsicmp(file_system.data(), L"NTFS") == 0) {
    return clonecore::Result<
        imageformat::TsumugiImageStorageFileSystem>::success(
        imageformat::TsumugiImageStorageFileSystem::ntfs);
  }
  if (_wcsicmp(file_system.data(), L"exFAT") == 0) {
    return clonecore::Result<
        imageformat::TsumugiImageStorageFileSystem>::success(
        imageformat::TsumugiImageStorageFileSystem::exfat);
  }
  return failure<imageformat::TsumugiImageStorageFileSystem>(
      clonecore::ErrorCode::unsupported_layout,
      ERROR_NOT_SUPPORTED,
      L"PE Tsumugi保存先ファイルシステム",
      L"単一.tsumugiファイルはNTFSまたはexFATだけに保存できます");
}

}  // namespace

clonecore::Result<DirectImageCreateReport> execute_direct_image_create(
    const DirectImageCreateRequest& request,
    const DirectImageCreateDependencies& dependencies) {
  const auto valid = validate_request(request, dependencies);
  if (!valid) {
    return clonecore::Result<DirectImageCreateReport>::failure(valid.error());
  }
  if (clonecore::disk_operation_cancellation_requested(request.callbacks)) {
    return failure<DirectImageCreateReport>(
        clonecore::ErrorCode::cancelled,
        ERROR_CANCELLED,
        L"PE直接Tsumugi開始前",
        L"開始前に取り消されました");
  }
  auto expected = diskmodel::make_stable_disk_identity(
      request.selected_source, request.selected_source.is_system_disk);
  if (!expected) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        expected.error());
  }

  imageformat::WindowsTsumugiDestinationGuardRequest destination_guard{
      .final_path = request.final_path,
      .expected_source_disk = expected.value(),
      .required_available_bytes = 1U,
      .replace_existing = request.replace_existing,
  };
  auto destination = dependencies.validate_destination(destination_guard);
  if (!destination) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        destination.error());
  }
  auto storage = dependencies.query_destination_file_system(
      request.final_path);
  if (!storage) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        storage.error());
  }
  if (storage.value() != imageformat::TsumugiImageStorageFileSystem::ntfs &&
      storage.value() != imageformat::TsumugiImageStorageFileSystem::exfat) {
    return failure<DirectImageCreateReport>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE Tsumugi保存先ファイルシステム",
        L"単一.tsumugiファイルはNTFSまたはexFATだけに保存できます");
  }
  if (clonecore::disk_operation_cancellation_requested(request.callbacks)) {
    return failure<DirectImageCreateReport>(
        clonecore::ErrorCode::cancelled,
        ERROR_CANCELLED,
        L"PEコピー元read-only化前",
        L"コピー元を変更する前に取り消されました");
  }

  const auto protected_source = dependencies.set_source_read_only(
      expected.value(), true);
  if (!protected_source) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        protected_source.error());
  }
  auto source = dependencies.open_read_only_source(expected.value());
  if (!source) {
    return clonecore::Result<DirectImageCreateReport>::failure(source.error());
  }
  if (!source.value().reader ||
      !source.value().observed.observed.read_only.has_value() ||
      !source.value().observed.observed.read_only.value() ||
      !same_reviewed_layout(
          request.selected_source, source.value().observed.observed)) {
    return failure<DirectImageCreateReport>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"PE Tsumugiコピー元の実行直前再識別",
        L"コピー元のread-only状態、パーティション形式、またはレビュー済みレイアウトが一致しません");
  }
  const auto identity = clonecore::validate_stable_identity(
      expected.value(), source.value().observed.identity,
      L"PE Tsumugiコピー元");
  if (!identity) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        identity.error());
  }

  const auto table_style =
      source.value().observed.observed.partition_style ==
              diskmodel::PartitionStyle::gpt
          ? imageformat::PartitionTableStyle::gpt
          : imageformat::PartitionTableStyle::mbr;
  auto partition_snapshot = imageformat::capture_partition_snapshot_v1(
      *source.value().reader, table_style);
  if (!partition_snapshot) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        partition_snapshot.error());
  }
  auto hashes = make_source_hashes(
      source.value().observed.identity,
      source.value().observed.observed.physical_sector_size,
      partition_snapshot.value());
  if (!hashes) {
    return clonecore::Result<DirectImageCreateReport>::failure(hashes.error());
  }

  LockedSourceSession session(
      std::move(source.value().reader), hashes.value());
  clonecore::Result<PreparedExactImage> plan =
      failure<PreparedExactImage>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"PE Tsumugiパーティション形式",
          L"コピー元はGPTまたはMBRでなければなりません");
  if (table_style == imageformat::PartitionTableStyle::gpt) {
    auto parsed = clonecore::parse_gpt(session);
    if (!parsed) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          parsed.error());
    }
    const auto reviewed = validate_gpt_inventory_geometry(
        source.value().observed.observed, parsed.value());
    if (!reviewed) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          reviewed.error());
    }
    plan = prepare_gpt_image(
        request, source.value(), parsed.value(),
        partition_snapshot.value(), hashes.value(), session);
  } else {
    auto parsed = clonecore::parse_mbr(session);
    if (!parsed) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          parsed.error());
    }
    const auto reviewed = validate_mbr_inventory_geometry(
        source.value().observed.observed, parsed.value());
    if (!reviewed) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          reviewed.error());
    }
    plan = prepare_mbr_image(
        request, source.value(), parsed.value(),
        partition_snapshot.value(), hashes.value(), session);
  }
  if (!plan) {
    return clonecore::Result<DirectImageCreateReport>::failure(plan.error());
  }
  if (request.rescue_mode) {
    plan.value().manifest.mode = imageformat::TsumugiManifestMode::rescue;
  }
  auto maximum_bytes = maximum_image_bytes(
      plan.value(), request.rescue_mode);
  if (!maximum_bytes) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        maximum_bytes.error());
  }
  std::uint64_t required_available_bytes = maximum_bytes.value();
  if (request.rescue_mode &&
      !checked_add(
          required_available_bytes,
          source.value().observed.identity.size_bytes,
          required_available_bytes)) {
    return failure<DirectImageCreateReport>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"PE救出Tsumugi必要容量",
        L"RAW一時領域と最大画像寸法の合計が64bit上限を超えます");
  }
  destination_guard.required_available_bytes = required_available_bytes;
  destination = dependencies.validate_destination(destination_guard);
  if (!destination) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        destination.error());
  }

  if (request.rescue_mode) {
    auto staging = dependencies.make_rescue_staging(
        imageformat::WindowsTsumugiRescueStagingRequest{
            .final_path = request.final_path,
            .expected_source_disk = source.value().observed.identity,
            .source_disk_size = source.value().observed.identity.size_bytes,
            .logical_sector_size =
                source.value().observed.identity.logical_sector_size,
            .source_model_hash = hashes.value().model,
            .source_serial_hash = hashes.value().serial,
            .source_state_hash = hashes.value().state,
            .required_available_bytes = required_available_bytes,
            .replace_existing = request.replace_existing,
        });
    if (!staging) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          staging.error());
    }
    clonecore::RescueCopyCallbacks rescue_callbacks{
        .cancellation_requested = request.callbacks.cancellation_requested,
        .safe_boundary = request.callbacks.safe_boundary,
    };
    if (request.callbacks.progress) {
      rescue_callbacks.progress =
          [&callbacks = request.callbacks](
              const clonecore::RescueCopyProgress& progress) {
            clonecore::report_disk_operation_progress(
                callbacks, translate_rescue_progress(progress));
          };
    }
    imageformat::TsumugiImageCreateRequest rescue_image{
        .final_path = request.final_path,
        .storage_file_system = storage.value(),
        .manifest = plan.value().manifest,
        .compression = imageformat::ImageCompression::zstandard,
        .chunk_size = kDirectImageChunkBytes,
        .verification_block_bytes = 4U * 1024U * 1024U,
        .verification_mode = request.verification_mode,
        .replace_existing = request.replace_existing,
    };
    if (request.encryption_password.has_value()) {
      rescue_image.encryption =
          imageformat::TsumugiImageEncryptionRequest{
              .password = *request.encryption_password,
          };
    }
    auto created = imageformat::create_tsumugi_rescue_image_v1(
        imageformat::TsumugiRescueImageCreateRequest{
            .image = std::move(rescue_image),
            .rescue_copy = clonecore::RescueRawCopyRequest{
                .environment =
                    clonecore::RescueExecutionEnvironment::winpe,
                .source_kind = source.value().observed.identity.is_system_disk
                    ? clonecore::RescueSourceKind::system_disk
                    : clonecore::RescueSourceKind::data_disk,
                .rescue_mode_explicitly_confirmed = true,
                .large_block_bytes = 4U * 1024U * 1024U,
                .callbacks = std::move(rescue_callbacks),
            },
            .failing_source = &session,
            .staging = staging.value().get(),
        },
        request.callbacks);
    if (!created) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          created.error());
    }
    if (!imageformat::selected_tsumugi_creation_verification_passed(
            created.value().image) ||
        !created.value().image.stream.committed ||
        !created.value().staging_sealed_for_image_read ||
        !created.value().staging_discarded_before_final_commit ||
        !created.value()
             .staging_destination_revalidated_before_final_commit) {
      return failure<DirectImageCreateReport>(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"PE救出Tsumugi作成結果",
          L"救出一時領域の封印・破棄・保存先再識別、選択済み画像検証、または完成名確定を確認できません");
    }
    return clonecore::Result<DirectImageCreateReport>::success({
        .source_identity = source.value().observed.identity,
        .source_partition_style =
            source.value().observed.observed.partition_style,
        .imaged_partition_count = static_cast<std::uint32_t>(
            plan.value().manifest.partitions.size()),
        .logical_payload_bytes = plan.value().logical_payload_bytes,
        .source_read_only_verified = true,
        .source_left_read_only = true,
        .layout_revalidated_before_commit = false,
        .rescue_mode = true,
        .rescue = std::move(created.value().rescue),
        .image = std::move(created.value().image),
    });
  }

  imageformat::TsumugiImageCreateRequest create_request{
      .final_path = request.final_path,
      .storage_file_system = storage.value(),
      .manifest = plan.value().manifest,
      .chunks = plan.value().chunks,
      .compression = imageformat::ImageCompression::zstandard,
      .chunk_size = kDirectImageChunkBytes,
      .verification_block_bytes = 4U * 1024U * 1024U,
      .verification_mode = request.verification_mode,
      .replace_existing = request.replace_existing,
      .source_session = &session,
  };
  if (request.encryption_password.has_value()) {
    create_request.encryption = imageformat::TsumugiImageEncryptionRequest{
        .password = *request.encryption_password,
    };
  }
  auto staged = imageformat::prepare_tsumugi_image_v1(
      create_request, request.callbacks);
  if (!staged) {
    return clonecore::Result<DirectImageCreateReport>::failure(staged.error());
  }

  auto final_snapshot = imageformat::capture_partition_snapshot_v1(
      session, table_style);
  if (!final_snapshot || final_snapshot.value() != partition_snapshot.value()) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        final_snapshot
            ? image_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_CRC,
                  L"PE Tsumugiコピー元レイアウト最終確認",
                  L"read-only Sourceのパーティション表が作成開始時から変化しました")
            : final_snapshot.error());
  }

  destination_guard.phase = imageformat::
      WindowsTsumugiDestinationGuardPhase::before_commit_owned_partial;
  destination_guard.expected_owned_partial_bytes =
      staged.value().report().stream.image_length;
  destination = dependencies.validate_destination(destination_guard);
  if (!destination) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        destination.error());
  }
  auto committed = staged.value().commit_verified();
  if (!committed) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        committed.error());
  }
  const auto& image = committed.value();
  if (!imageformat::selected_tsumugi_creation_verification_passed(image) ||
      !image.stream.committed || !image.unreadable_ranges.empty() ||
      image.stream.chunk_count != plan.value().chunks.size()) {
    return failure<DirectImageCreateReport>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"PE Tsumugi作成結果",
        L"選択済み作成時検証、チャンク件数、欠損なし、または完成名確定を確認できません");
  }
  return clonecore::Result<DirectImageCreateReport>::success({
      .source_identity = source.value().observed.identity,
      .source_partition_style =
          source.value().observed.observed.partition_style,
      .imaged_partition_count = static_cast<std::uint32_t>(
          plan.value().manifest.partitions.size()),
      .logical_payload_bytes = plan.value().logical_payload_bytes,
      .source_read_only_verified = true,
      .source_left_read_only = true,
      .layout_revalidated_before_commit = true,
      .rescue_mode = false,
      .image = committed.take_value(),
  });
}

clonecore::Result<DirectImageCreateReport>
execute_direct_image_create_with_windows_apis(
    const DirectImageCreateRequest& request) {
  return execute_direct_image_create(
      request,
      DirectImageCreateDependencies{
          .set_source_read_only =
              [](const clonecore::StableDiskIdentity& source,
                 const bool read_only) {
                return diskmodel::
                    set_verified_source_read_only_with_windows_apis(
                        source, read_only);
              },
          .open_read_only_source =
              [](const clonecore::StableDiskIdentity& source) {
                return diskmodel::
                    open_verified_read_only_physical_disk_with_windows_apis(
                        source);
              },
          .query_destination_file_system =
              query_destination_file_system_with_windows_apis,
          .validate_destination =
              [](const imageformat::WindowsTsumugiDestinationGuardRequest&
                     guard) {
                return imageformat::validate_windows_tsumugi_destination(
                    guard);
              },
          .make_rescue_staging =
              imageformat::make_windows_tsumugi_rescue_staging_session,
      });
}

}  // namespace ytec::winpeapp
