#include "ytec/windowsapp/online_direct_clone.h"

#include "ytec/bootrepair/clone_boot_finalization.h"

#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/clonecore/offline_mbr_clone.h"
#include "ytec/clonecore/windows_volume_bitmap.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace ytec::windowsapp {
namespace {

bool all_zero(const imageformat::Sha256Digest& value) noexcept {
  return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
    return byte == std::byte{0};
  });
}

clonecore::Error direct_error(
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

bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  result = left + right;
  return true;
}

bool checked_multiply(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (left != 0 &&
      right > std::numeric_limits<std::uint64_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
}

bool equals_case_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) {
  return left.size() == right.size() &&
      std::equal(
          left.begin(),
          left.end(),
          right.begin(),
          [](const wchar_t lhs, const wchar_t rhs) {
            return std::towlower(lhs) == std::towlower(rhs);
          });
}

clonecore::Result<clonecore::ByteRange> make_partition_range(
    const std::uint64_t first_lba,
    const std::uint64_t sector_count,
    const std::uint32_t sector_size,
    const std::uint64_t disk_size,
    const std::wstring_view operation) {
  std::uint64_t offset{};
  std::uint64_t length{};
  std::uint64_t end{};
  if (sector_count == 0 ||
      !checked_multiply(first_lba, sector_size, offset) ||
      !checked_multiply(sector_count, sector_size, length) ||
      !checked_add(offset, length, end) || end > disk_size) {
    return clonecore::Result<clonecore::ByteRange>::failure(direct_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        std::wstring(operation),
        L"パーティション位置がオーバーフローまたはディスク境界外です"));
  }
  return clonecore::Result<clonecore::ByteRange>::success(
      clonecore::ByteRange{.offset = offset, .length = length});
}

clonecore::Status validate_boot_read(
    const clonecore::ISourceDiskReader& source,
    const clonecore::ByteRange& range,
    const bool ntfs) {
  const auto boot = source.read(
      range.offset, source.logical_sector_size());
  if (!boot) {
    return clonecore::Status::failure(boot.error());
  }
  if (boot.value().size() != source.logical_sector_size()) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::io_failed,
        ERROR_HANDLE_EOF,
        L"オンライン直接クローン ブートセクター読取り",
        L"コピー元から論理セクターを完全に読み取れませんでした"));
  }
  if (ntfs) {
    const auto geometry = clonecore::parse_ntfs_geometry(
        boot.value(), source.logical_sector_size(), range.length);
    return geometry ? clonecore::success_status()
                    : clonecore::Status::failure(geometry.error());
  }
  return clonecore::validate_fat32_boot_sector(
      boot.value(), source.logical_sector_size(), range.length);
}

clonecore::Result<std::size_t> find_binding(
    const std::uint32_t partition_index,
    const std::span<const clonecore::VolumeBitmapBinding> bindings,
    const std::span<const std::uint8_t> used) {
  std::size_t found = bindings.size();
  for (std::size_t index = 0; index < bindings.size(); ++index) {
    if (bindings[index].partition_entry_index != partition_index) {
      continue;
    }
    if (found != bindings.size() || used[index] != 0 ||
        bindings[index].volume_device_path.empty()) {
      return clonecore::Result<std::size_t>::failure(direct_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DUP_NAME,
          L"オンライン直接クローン Volume対応",
          L"NTFSパーティションへのVolume対応が空または重複しています"));
    }
    found = index;
  }
  if (found == bindings.size()) {
    return clonecore::Result<std::size_t>::failure(direct_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        L"オンライン直接クローン Volume対応",
        L"NTFSパーティションに対応するVolume GUIDがありません"));
  }
  return clonecore::Result<std::size_t>::success(found);
}

clonecore::Status reject_unused_or_duplicate_bindings(
    const std::span<const clonecore::VolumeBitmapBinding> bindings,
    const std::span<const std::uint8_t> used) {
  for (std::size_t index = 0; index < bindings.size(); ++index) {
    if (used[index] == 0) {
      return clonecore::Status::failure(direct_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"オンライン直接クローン Volume対応件数",
          L"パーティション計画にないVolume対応が含まれています"));
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (equals_case_insensitive(
              bindings[index].volume_device_path,
              bindings[previous].volume_device_path)) {
        return clonecore::Status::failure(direct_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"オンライン直接クローン Volume重複",
            L"複数パーティションが同じVolume GUIDを示しています"));
      }
    }
  }
  return clonecore::success_status();
}

bool range_less(
    const clonecore::ByteRange& left,
    const clonecore::ByteRange& right) noexcept {
  return left.offset < right.offset;
}

clonecore::Status validate_layout_ranges(OnlineDirectSourceLayout& layout) {
  if (layout.snapshot_partitions.empty()) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"オンライン直接クローン Snapshot対象",
        L"VSSで固定できるNTFSパーティションがありません"));
  }
  std::sort(
      layout.snapshot_partitions.begin(),
      layout.snapshot_partitions.end(),
      [](const auto& left, const auto& right) {
        return left.disk_offset < right.disk_offset;
      });
  std::sort(
      layout.static_physical_ranges.begin(),
      layout.static_physical_ranges.end(),
      range_less);
  std::uint64_t previous_end{};
  for (std::size_t index = 0;
       index < layout.snapshot_partitions.size(); ++index) {
    const auto& route = layout.snapshot_partitions[index];
    std::uint64_t end{};
    if (route.length == 0 || route.volume_guid_path.empty() ||
        !checked_add(route.disk_offset, route.length, end) ||
        (index != 0 && route.disk_offset < previous_end)) {
      return clonecore::Status::failure(direct_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"オンライン直接クローン Snapshot範囲",
          L"Snapshot対象範囲が空、重複、またはオーバーフローしています"));
    }
    previous_end = end;
  }
  previous_end = 0;
  for (std::size_t index = 0;
       index < layout.static_physical_ranges.size(); ++index) {
    const auto& range = layout.static_physical_ranges[index];
    std::uint64_t end{};
    if (range.length == 0 || !checked_add(range.offset, range.length, end) ||
        (index != 0 && range.offset < previous_end)) {
      return clonecore::Status::failure(direct_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"オンライン直接クローン 固定領域",
          L"固定領域が空、重複、またはオーバーフローしています"));
    }
    previous_end = end;
  }
  for (const auto& snapshot : layout.snapshot_partitions) {
    const std::uint64_t snapshot_end =
        snapshot.disk_offset + snapshot.length;
    for (const auto& fixed : layout.static_physical_ranges) {
      const std::uint64_t fixed_end = fixed.offset + fixed.length;
      if (snapshot.disk_offset < fixed_end && fixed.offset < snapshot_end) {
        return clonecore::Status::failure(direct_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"オンライン直接クローン 読取り経路分離",
            L"Snapshot領域と物理固定領域が重複しています"));
      }
    }
  }
  return clonecore::success_status();
}

bool same_range(
    const clonecore::ByteRange& left,
    const clonecore::ByteRange& right) noexcept {
  return left.offset == right.offset && left.length == right.length;
}

bool same_source_layout(
    const OnlineDirectSourceLayout& left,
    const OnlineDirectSourceLayout& right) {
  if (left.partition_style != right.partition_style ||
      left.snapshot_partitions.size() != right.snapshot_partitions.size() ||
      left.static_physical_ranges.size() !=
          right.static_physical_ranges.size()) {
    return false;
  }
  for (std::size_t index = 0;
       index < left.snapshot_partitions.size(); ++index) {
    const auto& lhs = left.snapshot_partitions[index];
    const auto& rhs = right.snapshot_partitions[index];
    if (lhs.partition_index != rhs.partition_index ||
        lhs.disk_offset != rhs.disk_offset || lhs.length != rhs.length ||
        !equals_case_insensitive(
            lhs.volume_guid_path, rhs.volume_guid_path)) {
      return false;
    }
  }
  for (std::size_t index = 0;
       index < left.static_physical_ranges.size(); ++index) {
    if (!same_range(
            left.static_physical_ranges[index],
            right.static_physical_ranges[index])) {
      return false;
    }
  }
  return true;
}

clonecore::Status validate_observation(
    const diskmodel::ReidentifiedPhysicalClone& observed) {
  if (!observed.source.offline.has_value() ||
      !observed.source.read_only.has_value() ||
      !observed.source.removable.has_value() ||
      !observed.target.offline.has_value() ||
      !observed.target.read_only.has_value() ||
      !observed.target.removable.has_value()) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"オンライン直接クローン ディスク属性",
        L"コピー元またはコピー先の安全属性を確定できません"));
  }
  if (observed.source.offline.value() ||
      observed.source.removable.value()) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"オンライン直接クローン コピー元状態",
        L"オンラインの固定基本ディスクだけをコピー元にできます"));
  }
  if (observed.target.read_only.value() ||
      observed.target.removable.value()) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::access_denied,
        observed.target.read_only.value()
            ? ERROR_WRITE_PROTECT
            : ERROR_NOT_SUPPORTED,
        L"オンライン直接クローン コピー先属性",
        L"読取り専用ディスクまたはremovable媒体はコピー先にできません"));
  }
  if (diskmodel::disk_health_operation_advice(
          observed.target.health, false) ==
      diskmodel::DiskHealthOperationAdvice::block_target) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_DEVICE_HARDWARE_ERROR,
        L"オンライン直接クローン コピー先健康状態",
        L"SMARTまたはNVMeが注意・異常を報告しているディスクはコピー先にできません"));
  }
  const auto style = diskmodel::normalize_disk_partition_style(
      observed.target.partition_style,
      observed.target.partitions.size());
  if (style != diskmodel::PartitionStyle::raw &&
      style != diskmodel::PartitionStyle::gpt &&
      style != diskmodel::PartitionStyle::mbr) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"オンライン直接クローン コピー先形式",
        L"RAW、GPT、MBRとして確定できないコピー先は消去できません"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_reviewed_layouts(
    const OnlineDirectCloneRequest& request,
    const diskmodel::ReidentifiedPhysicalClone& observed,
    const std::wstring_view phase) {
  auto source_hash = imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(observed.source);
  auto target_hash = imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(observed.target);
  if (!source_hash || !target_hash) {
    return clonecore::Status::failure(
        !source_hash ? source_hash.error() : target_hash.error());
  }
  if (source_hash.value() != request.expected_source_layout_hash ||
      target_hash.value() != request.expected_target_layout_hash) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        std::wstring(L"オンライン直接クローン ") +
            std::wstring(phase) + L"レイアウト照合",
        L"最終確認画面の後にコピー元またはコピー先のパーティション形式・配置が変化しました"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_dependencies(
    const OnlineDirectCloneRequest& request,
    const OnlineDirectCloneDependencies& dependencies) {
  if (!request.administrator) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::access_denied,
        ERROR_ELEVATION_REQUIRED,
        L"オンライン直接クローン 管理者確認",
        L"アプリ起動時の管理者権限が必要です"));
  }
  if (all_zero(request.expected_source_layout_hash) ||
      all_zero(request.expected_target_layout_hash)) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"オンライン直接クローン レビューHash",
        L"最終確認したコピー元・コピー先レイアウトHashがありません"));
  }
  if (request.maximum_chunk_bytes == 0 ||
      request.maximum_chunk_bytes > 16U * 1024U * 1024U) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"オンライン直接クローン 要求",
        L"コピー元または転送チャンク寸法が不正です"));
  }
  if (!dependencies.reidentify_clone ||
      !dependencies.open_read_only_source ||
      !dependencies.query_gpt_bindings ||
      !dependencies.query_mbr_bindings ||
      !dependencies.run_snapshot_workflow ||
      !dependencies.open_snapshot_reader ||
      !dependencies.make_snapshot_bitmap_provider ||
      !dependencies.set_clone_target_offline ||
      !dependencies.set_physical_target_offline ||
      !dependencies.open_offline_target ||
      !dependencies.collect_mbr_signatures ||
      !dependencies.execute_clone_engine ||
      !dependencies.finalize_target_boot) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"オンライン直接クローン 依存境界",
        L"再識別、VSS、Snapshot Reader、コピー先、またはClone Engineがありません"));
  }
  return clonecore::success_status();
}

class OnlineDirectCompositeReader final
    : public clonecore::ISourceDiskReader {
 public:
  OnlineDirectCompositeReader(
      const clonecore::ISourceDiskReader* physical,
      std::vector<OnlineDirectSnapshotReader> snapshots,
      std::vector<clonecore::ByteRange> fixed)
      : physical_(physical),
        snapshots_(std::move(snapshots)),
        fixed_(std::move(fixed)) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return physical_->size_bytes();
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return physical_->logical_sector_size();
  }

  [[nodiscard]] clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    std::uint64_t end{};
    if (length == 0 || !checked_add(offset, length, end) ||
        end > size_bytes()) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          direct_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_PARAMETER,
              L"オンラインSnapshot合成Reader範囲",
              L"読取り要求が空、オーバーフロー、またはディスク境界外です"));
    }
    for (const auto& snapshot : snapshots_) {
      const std::uint64_t route_end =
          snapshot.disk_offset + snapshot.length;
      if (offset >= snapshot.disk_offset && end <= route_end) {
        auto bytes = snapshot.reader->read(
            offset - snapshot.disk_offset, length);
        if (!bytes || bytes.value().size() != length) {
          return bytes
              ? clonecore::Result<std::vector<std::byte>>::failure(
                    direct_error(
                        clonecore::ErrorCode::io_failed,
                        ERROR_HANDLE_EOF,
                        L"オンラインSnapshot合成Reader",
                        L"Snapshotから要求長を完全に読み取れませんでした"))
              : clonecore::Result<std::vector<std::byte>>::failure(
                    bytes.error());
        }
        return bytes;
      }
      if (offset < route_end && snapshot.disk_offset < end) {
        return clonecore::Result<std::vector<std::byte>>::failure(
            direct_error(
                clonecore::ErrorCode::unsupported_layout,
                ERROR_NOT_SUPPORTED,
                L"オンラインSnapshot合成Reader境界",
                L"1回の読取りがSnapshot境界をまたいでいます"));
      }
    }
    for (const auto& fixed : fixed_) {
      const std::uint64_t route_end = fixed.offset + fixed.length;
      if (offset >= fixed.offset && end <= route_end) {
        auto bytes = physical_->read(offset, length);
        if (!bytes || bytes.value().size() != length) {
          return bytes
              ? clonecore::Result<std::vector<std::byte>>::failure(
                    direct_error(
                        clonecore::ErrorCode::io_failed,
                        ERROR_HANDLE_EOF,
                        L"オンライン固定領域Reader",
                        L"物理ディスクから要求長を完全に読み取れませんでした"))
              : clonecore::Result<std::vector<std::byte>>::failure(
                    bytes.error());
        }
        return bytes;
      }
    }
    return clonecore::Result<std::vector<std::byte>>::failure(direct_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"オンラインSnapshot合成Reader経路",
        L"Snapshotまたは検証済み固定領域に含まれない読取りを拒否しました"));
  }

 private:
  const clonecore::ISourceDiskReader* physical_{};
  std::vector<OnlineDirectSnapshotReader> snapshots_;
  std::vector<clonecore::ByteRange> fixed_;
};

clonecore::Error append_offline_failure(
    clonecore::Error primary,
    const clonecore::Status& offline) {
  if (!offline) {
    primary.message +=
        L"。コピー先offline状態の再確認にも失敗しました: " +
        offline.error().operation;
  }
  return primary;
}

clonecore::Status reprotect_target_offline_after_exception(
    const OnlineDirectCloneRequest& request,
    const OnlineDirectCloneDependencies& dependencies) {
  try {
    return dependencies.set_physical_target_offline(
        request.expected_target, request.confirmation, true);
  } catch (...) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"オンライン直接クローン 例外後offline保護",
        L"例外後のコピー先offline化でも例外が発生し、状態を確認できません"));
  }
}

clonecore::Result<std::vector<std::uint32_t>>
collect_connected_mbr_signatures_with_windows_apis(
    const clonecore::StableDiskIdentity& expected_source,
    const clonecore::MbrDisk& source_mbr) {
  auto inventory = diskmodel::make_windows_disk_inventory_provider();
  const auto report = inventory->enumerate();
  if (!report) {
    return clonecore::Result<std::vector<std::uint32_t>>::failure(
        report.error());
  }
  if (!report.value().issues.empty()) {
    return clonecore::Result<std::vector<std::uint32_t>>::failure(
        direct_error(
            clonecore::ErrorCode::query_failed,
            ERROR_INVALID_DATA,
            L"オンラインMBR署名 全ディスク列挙",
            L"未解決の列挙診断があるため署名衝突を確認できません"));
  }
  std::vector<std::uint32_t> signatures{source_mbr.disk_signature};
  for (const auto& disk : report.value().disks) {
    if (diskmodel::normalize_disk_partition_style(
            disk.partition_style, disk.partitions.size()) !=
        diskmodel::PartitionStyle::mbr) {
      continue;
    }
    const auto identity = diskmodel::make_stable_disk_identity(
        disk, disk.is_system_disk);
    if (!identity) {
      return clonecore::Result<std::vector<std::uint32_t>>::failure(
          identity.error());
    }
    if (clonecore::validate_stable_identity(
            expected_source, identity.value(), L"オンラインMBRコピー元")) {
      continue;
    }
    auto handle =
        diskmodel::open_verified_read_only_physical_disk_with_windows_apis(
            identity.value());
    if (!handle) {
      return clonecore::Result<std::vector<std::uint32_t>>::failure(
          handle.error());
    }
    const auto mbr = clonecore::parse_mbr(*handle.value().reader);
    if (!mbr) {
      return clonecore::Result<std::vector<std::uint32_t>>::failure(
          mbr.error());
    }
    signatures.push_back(mbr.value().disk_signature);
  }
  std::sort(signatures.begin(), signatures.end());
  signatures.erase(
      std::unique(signatures.begin(), signatures.end()),
      signatures.end());
  return clonecore::Result<std::vector<std::uint32_t>>::success(
      std::move(signatures));
}

clonecore::Result<OnlineDirectCloneEngineReport>
execute_clone_engine_with_native_core(
    const OnlineDirectCloneEngineContext& context) {
  if (context.request == nullptr || context.observed_source == nullptr ||
      context.observed_target == nullptr || context.source == nullptr ||
      context.target == nullptr ||
      context.snapshot_bitmap_provider == nullptr) {
    return clonecore::Result<OnlineDirectCloneEngineReport>::failure(
        direct_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"オンラインClone Engine接続",
            L"要求、識別情報、Reader、Writer、またはBitmap Providerがありません"));
  }
  if (context.partition_style ==
      OnlineDirectClonePartitionStyle::gpt) {
    auto guid_generator = clonecore::make_windows_guid_generator();
    const auto report = clonecore::execute_offline_gpt_clone(
        clonecore::OfflineGptCloneRequest{
            .expected_source = context.request->expected_source,
            .observed_source = *context.observed_source,
            .expected_target = context.request->expected_target,
            .observed_target = *context.observed_target,
            .confirmation = context.request->confirmation,
            .maximum_chunk_bytes =
                context.request->maximum_chunk_bytes,
            .callbacks = context.request->callbacks,
        },
        *context.source,
        *context.target,
        *context.snapshot_bitmap_provider,
        *guid_generator);
    if (!report) {
      return clonecore::Result<OnlineDirectCloneEngineReport>::failure(
          report.error());
    }
    return clonecore::Result<OnlineDirectCloneEngineReport>::success(
        OnlineDirectCloneEngineReport{
            .copied_data_bytes = report.value().copied_data_bytes,
            .copied_partition_count =
                report.value().copied_partition_count,
            .recreated_partition_count =
                report.value().recreated_partition_count,
            .verified_write_digest =
                report.value().verified_write_digest,
            .read_back_verified = report.value().read_back_verified,
            .partition_table_committed =
                report.value().primary_gpt_committed,
        });
  }

  auto signature_generator =
      clonecore::make_windows_mbr_signature_generator();
  const auto report = clonecore::execute_offline_mbr_clone(
      clonecore::OfflineMbrCloneRequest{
          .expected_source = context.request->expected_source,
          .observed_source = *context.observed_source,
          .expected_target = context.request->expected_target,
          .observed_target = *context.observed_target,
          .confirmation = context.request->confirmation,
          .maximum_chunk_bytes =
              context.request->maximum_chunk_bytes,
          .connected_mbr_signatures =
              std::vector<std::uint32_t>(
                  context.connected_mbr_signatures.begin(),
                  context.connected_mbr_signatures.end()),
          .callbacks = context.request->callbacks,
      },
      *context.source,
      *context.target,
      *context.snapshot_bitmap_provider,
      *signature_generator);
  if (!report) {
    return clonecore::Result<OnlineDirectCloneEngineReport>::failure(
        report.error());
  }
  return clonecore::Result<OnlineDirectCloneEngineReport>::success(
      OnlineDirectCloneEngineReport{
          .copied_data_bytes = report.value().copied_data_bytes,
          .copied_partition_count =
              report.value().copied_partition_count,
          .recreated_partition_count = 0,
          .verified_write_digest =
              report.value().verified_write_digest,
          .read_back_verified = report.value().read_back_verified,
          .partition_table_committed =
              report.value().target_mbr_committed,
      });
}

}  // namespace

clonecore::Result<OnlineDirectSourceLayout>
build_online_direct_source_layout(
    const diskmodel::DiskInfo& observed_source,
    const clonecore::ISourceDiskReader& read_only_source,
    const std::span<const clonecore::VolumeBitmapBinding> volume_bindings) {
  if (observed_source.size_bytes != read_only_source.size_bytes() ||
      observed_source.logical_sector_size !=
          read_only_source.logical_sector_size() ||
      observed_source.size_bytes == 0 || volume_bindings.empty()) {
    return clonecore::Result<OnlineDirectSourceLayout>::failure(
        direct_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"オンライン直接クローン コピー元寸法",
        L"ディスク、Reader寸法、またはVolume対応が一致しません"));
  }

  OnlineDirectSourceLayout layout;
  std::vector<std::uint8_t> used(volume_bindings.size(), 0);
  if (observed_source.partition_style ==
      diskmodel::PartitionStyle::gpt) {
    const auto gpt = clonecore::parse_gpt(read_only_source);
    if (!gpt) {
      return clonecore::Result<OnlineDirectSourceLayout>::failure(
          gpt.error());
    }
    layout.partition_style = OnlineDirectClonePartitionStyle::gpt;
    const auto leading = make_partition_range(
        0,
        gpt.value().first_usable_lba,
        gpt.value().logical_sector_size,
        read_only_source.size_bytes(),
        L"オンラインGPT先頭メタデータ");
    const auto trailing = make_partition_range(
        gpt.value().last_usable_lba + 1,
        gpt.value().sector_count - gpt.value().last_usable_lba - 1,
        gpt.value().logical_sector_size,
        read_only_source.size_bytes(),
        L"オンラインGPT末尾メタデータ");
    if (!leading || !trailing) {
      return clonecore::Result<OnlineDirectSourceLayout>::failure(
          leading ? trailing.error() : leading.error());
    }
    layout.static_physical_ranges.push_back(leading.value());
    layout.static_physical_ranges.push_back(trailing.value());

    for (const auto& partition : gpt.value().partitions) {
      const auto range = make_partition_range(
          partition.first_lba,
          partition.last_lba - partition.first_lba + 1,
          gpt.value().logical_sector_size,
          read_only_source.size_bytes(),
          L"オンラインGPTパーティション");
      if (!range) {
        return clonecore::Result<OnlineDirectSourceLayout>::failure(
            range.error());
      }
      if (partition.type_guid == clonecore::gpt_type_basic_data()) {
        const auto ntfs = validate_boot_read(
            read_only_source, range.value(), true);
        if (!ntfs) {
          return clonecore::Result<OnlineDirectSourceLayout>::failure(
              ntfs.error());
        }
        const auto binding = find_binding(
            partition.entry_index, volume_bindings, used);
        if (!binding) {
          return clonecore::Result<OnlineDirectSourceLayout>::failure(
              binding.error());
        }
        used[binding.value()] = 1;
        layout.snapshot_partitions.push_back(
            OnlineDirectSnapshotPartition{
                .partition_index = partition.entry_index,
                .disk_offset = range.value().offset,
                .length = range.value().length,
                .volume_guid_path =
                    volume_bindings[binding.value()].volume_device_path,
            });
      } else if (
          partition.type_guid == clonecore::gpt_type_efi_system()) {
        const auto fat = validate_boot_read(
            read_only_source, range.value(), false);
        if (!fat) {
          return clonecore::Result<OnlineDirectSourceLayout>::failure(
              fat.error());
        }
        layout.static_physical_ranges.push_back(range.value());
      } else if (
          partition.type_guid ==
          clonecore::gpt_type_windows_recovery()) {
        const auto ntfs = validate_boot_read(
            read_only_source, range.value(), true);
        if (!ntfs) {
          return clonecore::Result<OnlineDirectSourceLayout>::failure(
              ntfs.error());
        }
        layout.static_physical_ranges.push_back(range.value());
      } else if (
          partition.type_guid !=
          clonecore::gpt_type_microsoft_reserved()) {
        return clonecore::Result<OnlineDirectSourceLayout>::failure(
            direct_error(
                clonecore::ErrorCode::unsupported_layout,
                ERROR_NOT_SUPPORTED,
                L"オンラインGPTパーティション種別",
                L"Snapshot整合性を保証できないGPTパーティションがあります"));
      }
    }
  } else if (
      observed_source.partition_style == diskmodel::PartitionStyle::mbr) {
    const auto mbr = clonecore::parse_mbr(read_only_source);
    if (!mbr) {
      return clonecore::Result<OnlineDirectSourceLayout>::failure(
          mbr.error());
    }
    layout.partition_style = OnlineDirectClonePartitionStyle::mbr;
    layout.static_physical_ranges.push_back(
        clonecore::ByteRange{.offset = 0, .length = 512});
    for (const auto& partition : mbr.value().partitions) {
      const auto range = make_partition_range(
          partition.first_lba,
          partition.sector_count,
          mbr.value().logical_sector_size,
          read_only_source.size_bytes(),
          L"オンラインMBRパーティション");
      if (!range) {
        return clonecore::Result<OnlineDirectSourceLayout>::failure(
            range.error());
      }
      if (partition.type == 0x07) {
        const auto ntfs = validate_boot_read(
            read_only_source, range.value(), true);
        if (!ntfs) {
          return clonecore::Result<OnlineDirectSourceLayout>::failure(
              ntfs.error());
        }
        const auto binding = find_binding(
            partition.table_index, volume_bindings, used);
        if (!binding) {
          return clonecore::Result<OnlineDirectSourceLayout>::failure(
              binding.error());
        }
        used[binding.value()] = 1;
        layout.snapshot_partitions.push_back(
            OnlineDirectSnapshotPartition{
                .partition_index = partition.table_index,
                .disk_offset = range.value().offset,
                .length = range.value().length,
                .volume_guid_path =
                    volume_bindings[binding.value()].volume_device_path,
            });
      } else if (partition.type == 0x27 && !partition.active) {
        const auto ntfs = validate_boot_read(
            read_only_source, range.value(), true);
        if (!ntfs) {
          return clonecore::Result<OnlineDirectSourceLayout>::failure(
              ntfs.error());
        }
        layout.static_physical_ranges.push_back(range.value());
      } else if (
          (partition.type == 0x0B || partition.type == 0x0C) &&
          partition.active) {
        const auto fat = validate_boot_read(
            read_only_source, range.value(), false);
        if (!fat) {
          return clonecore::Result<OnlineDirectSourceLayout>::failure(
              fat.error());
        }
        layout.static_physical_ranges.push_back(range.value());
      } else {
        return clonecore::Result<OnlineDirectSourceLayout>::failure(
            direct_error(
                clonecore::ErrorCode::unsupported_layout,
                ERROR_NOT_SUPPORTED,
                L"オンラインMBRパーティション種別",
                L"Snapshot整合性を保証できないMBRパーティションがあります"));
      }
    }
  } else {
    return clonecore::Result<OnlineDirectSourceLayout>::failure(
        direct_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"オンライン直接クローン コピー元形式",
        L"GPTまたはMBRの基本ディスクだけを扱えます"));
  }

  const auto all_used = reject_unused_or_duplicate_bindings(
      volume_bindings, used);
  if (!all_used) {
    return clonecore::Result<OnlineDirectSourceLayout>::failure(
        all_used.error());
  }
  const auto ranges = validate_layout_ranges(layout);
  if (!ranges) {
    return clonecore::Result<OnlineDirectSourceLayout>::failure(
        ranges.error());
  }
  return clonecore::Result<OnlineDirectSourceLayout>::success(
      std::move(layout));
}

clonecore::Result<std::unique_ptr<clonecore::ISourceDiskReader>>
make_online_direct_composite_reader(
    const clonecore::ISourceDiskReader* read_only_physical_source,
    std::vector<OnlineDirectSnapshotReader> snapshot_readers,
    std::vector<clonecore::ByteRange> static_physical_ranges) {
  if (read_only_physical_source == nullptr || snapshot_readers.empty() ||
      static_physical_ranges.empty()) {
    return clonecore::Result<
        std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
        direct_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"オンラインSnapshot合成Reader生成",
            L"物理Reader、Snapshot Reader、または固定領域がありません"));
  }
  std::sort(
      snapshot_readers.begin(),
      snapshot_readers.end(),
      [](const auto& left, const auto& right) {
        return left.disk_offset < right.disk_offset;
      });
  std::sort(
      static_physical_ranges.begin(),
      static_physical_ranges.end(),
      range_less);
  std::uint64_t previous_end{};
  for (std::size_t index = 0; index < snapshot_readers.size(); ++index) {
    const auto& route = snapshot_readers[index];
    std::uint64_t end{};
    if (!route.reader || route.length == 0 ||
        !checked_add(route.disk_offset, route.length, end) ||
        end > read_only_physical_source->size_bytes() ||
        (index != 0 && route.disk_offset < previous_end) ||
        route.reader->size_bytes() != route.length ||
        route.reader->logical_sector_size() !=
            read_only_physical_source->logical_sector_size()) {
      return clonecore::Result<
          std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
          direct_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_INVALID_DATA,
              L"オンラインSnapshot Reader再確認",
              L"Snapshot Readerの範囲、寸法、セクター、または重複が不正です"));
    }
    previous_end = end;
  }
  previous_end = 0;
  for (std::size_t index = 0;
       index < static_physical_ranges.size(); ++index) {
    const auto& fixed = static_physical_ranges[index];
    std::uint64_t end{};
    if (fixed.length == 0 || !checked_add(fixed.offset, fixed.length, end) ||
        end > read_only_physical_source->size_bytes() ||
        (index != 0 && fixed.offset < previous_end)) {
      return clonecore::Result<
          std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
          direct_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"オンライン固定領域Reader再確認",
              L"固定領域がディスク境界外または重複しています"));
    }
    previous_end = end;
  }
  for (const auto& snapshot : snapshot_readers) {
    const std::uint64_t snapshot_end =
        snapshot.disk_offset + snapshot.length;
    for (const auto& fixed : static_physical_ranges) {
      const std::uint64_t fixed_end = fixed.offset + fixed.length;
      if (snapshot.disk_offset < fixed_end && fixed.offset < snapshot_end) {
        return clonecore::Result<
            std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
            direct_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DATA,
                L"オンライン合成Reader経路分離",
                L"Snapshot領域と固定領域が重複しています"));
      }
    }
  }
  std::unique_ptr<clonecore::ISourceDiskReader> reader =
      std::make_unique<OnlineDirectCompositeReader>(
          read_only_physical_source,
          std::move(snapshot_readers),
          std::move(static_physical_ranges));
  return clonecore::Result<
      std::unique_ptr<clonecore::ISourceDiskReader>>::success(
      std::move(reader));
}

clonecore::Result<OnlineDirectCloneReport>
execute_online_direct_clone(
    const OnlineDirectCloneRequest& request,
    const OnlineDirectCloneDependencies& dependencies) {
  const auto valid = validate_dependencies(request, dependencies);
  if (!valid) {
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        valid.error());
  }

  auto initial = dependencies.reidentify_clone(
      request.expected_source,
      request.expected_target,
      request.confirmation);
  if (!initial) {
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        initial.error());
  }
  const auto initial_safe = validate_observation(initial.value());
  if (!initial_safe) {
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        initial_safe.error());
  }
  const auto initial_layouts = validate_reviewed_layouts(
      request, initial.value(), L"開始時");
  if (!initial_layouts) {
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        initial_layouts.error());
  }

  auto source = dependencies.open_read_only_source(
      request.expected_source);
  if (!source) {
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        source.error());
  }
  if (!source.value().reader) {
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        direct_error(
            clonecore::ErrorCode::internal_error,
            ERROR_INVALID_HANDLE,
            L"オンライン直接クローン コピー元Reader",
            L"検証済みの読取り専用物理Readerがありません"));
  }
  const auto source_identity = clonecore::validate_stable_identity(
      request.expected_source,
      source.value().observed.identity,
      L"オンライン直接クローン コピー元");
  if (!source_identity) {
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        source_identity.error());
  }

  std::vector<clonecore::VolumeBitmapBinding> volume_bindings;
  std::vector<std::uint32_t> connected_mbr_signatures;
  if (source.value().observed.observed.partition_style ==
      diskmodel::PartitionStyle::gpt) {
    const auto gpt = clonecore::parse_gpt(*source.value().reader);
    if (!gpt) {
      return clonecore::Result<OnlineDirectCloneReport>::failure(
          gpt.error());
    }
    auto bindings = dependencies.query_gpt_bindings(
        source.value().observed.observed, gpt.value());
    if (!bindings) {
      return clonecore::Result<OnlineDirectCloneReport>::failure(
          bindings.error());
    }
    volume_bindings = bindings.take_value();
  } else if (
      source.value().observed.observed.partition_style ==
      diskmodel::PartitionStyle::mbr) {
    const auto mbr = clonecore::parse_mbr(*source.value().reader);
    if (!mbr) {
      return clonecore::Result<OnlineDirectCloneReport>::failure(
          mbr.error());
    }
    auto bindings = dependencies.query_mbr_bindings(
        source.value().observed.observed, mbr.value());
    if (!bindings) {
      return clonecore::Result<OnlineDirectCloneReport>::failure(
          bindings.error());
    }
    volume_bindings = bindings.take_value();
    auto signatures = dependencies.collect_mbr_signatures(
        request.expected_source, mbr.value());
    if (!signatures) {
      return clonecore::Result<OnlineDirectCloneReport>::failure(
          signatures.error());
    }
    connected_mbr_signatures = signatures.take_value();
  } else {
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        direct_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"オンライン直接クローン コピー元形式",
            L"GPTまたはMBRコピー元だけを扱えます"));
  }

  auto layout = build_online_direct_source_layout(
      source.value().observed.observed,
      *source.value().reader,
      volume_bindings);
  if (!layout) {
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        layout.error());
  }
  vssrequester::WorkflowRequest workflow{
      .administrator = request.administrator,
  };
  workflow.volumes.reserve(layout.value().snapshot_partitions.size());
  for (const auto& partition : layout.value().snapshot_partitions) {
    workflow.volumes.push_back(vssrequester::VolumeRequest{
        .volume_guid_path = partition.volume_guid_path,
        .file_system = L"NTFS",
    });
  }

  bool copy_callback_called = false;
  bool destructive_phase_started = false;
  bool target_offline_verified = false;
  const bool boot_finalization_required =
      request.expected_source.is_system_disk;
  bool boot_finalization_completed = false;
  std::optional<OnlineDirectCloneEngineReport> engine_report;
  const auto workflow_report = dependencies.run_snapshot_workflow(
      workflow,
      request.async_wait,
      request.logger,
      [&](const vssrequester::SnapshotCopyContext& snapshot_context) {
        try {
        const auto& mappings = snapshot_context.mappings;
        if (copy_callback_called ||
            snapshot_context.snapshot_set_id.empty() ||
            mappings.size() != layout.value().snapshot_partitions.size()) {
          return clonecore::Status::failure(direct_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_INVALID_DATA,
              L"オンライン直接クローン Snapshot対応件数",
              L"Snapshotコピーは1回だけ、計画と同じ件数でなければなりません"));
        }
        copy_callback_called = true;

        std::vector<OnlineDirectSnapshotReader> snapshot_readers;
        std::vector<clonecore::SnapshotVolumeBitmapBinding>
            bitmap_bindings;
        snapshot_readers.reserve(mappings.size());
        bitmap_bindings.reserve(mappings.size());
        for (std::size_t index = 0;
             index < mappings.size(); ++index) {
          const auto& partition =
              layout.value().snapshot_partitions[index];
          if (mappings[index].snapshot_id.empty()) {
            return clonecore::Status::failure(direct_error(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_INVALID_DATA,
                L"オンライン直接クローン Snapshot ID",
                L"Snapshot IDが空のためコピーを開始できません"));
          }
          auto reader = dependencies.open_snapshot_reader(
              vssrequester::SnapshotVolumeOpenRequest{
                  .snapshot_device_path =
                      mappings[index].snapshot_device_path,
                  .expected_size_bytes = partition.length,
                  .logical_sector_size =
                      request.expected_source.logical_sector_size,
              });
          if (!reader) {
            return clonecore::Status::failure(reader.error());
          }
          snapshot_readers.push_back(OnlineDirectSnapshotReader{
              .partition_index = partition.partition_index,
              .disk_offset = partition.disk_offset,
              .length = partition.length,
              .reader = reader.take_value(),
          });
          bitmap_bindings.push_back(
              clonecore::SnapshotVolumeBitmapBinding{
                  .partition_entry_index = partition.partition_index,
                  .snapshot_device_path =
                      mappings[index].snapshot_device_path,
              });
        }
        auto composite = make_online_direct_composite_reader(
            source.value().reader.get(),
            std::move(snapshot_readers),
            layout.value().static_physical_ranges);
        if (!composite) {
          return clonecore::Status::failure(composite.error());
        }
        auto bitmap_provider =
            dependencies.make_snapshot_bitmap_provider(
                std::move(bitmap_bindings));
        if (!bitmap_provider || !bitmap_provider.value()) {
          return clonecore::Status::failure(
              bitmap_provider
                  ? direct_error(
                        clonecore::ErrorCode::internal_error,
                        ERROR_INVALID_HANDLE,
                        L"オンラインSnapshot Bitmap Provider",
                        L"Bitmap Provider Factoryが空を返しました")
                  : bitmap_provider.error());
        }

        // This is the final source/target reidentification immediately before
        // the first target state change. Rebuild the source route plan from
        // the still-open physical reader to reject a layout change that the
        // stable device identity alone cannot detect.
        auto final_observation = dependencies.reidentify_clone(
            request.expected_source,
            request.expected_target,
            request.confirmation);
        if (!final_observation) {
          return clonecore::Status::failure(final_observation.error());
        }
        const auto final_safe =
            validate_observation(final_observation.value());
        if (!final_safe) {
          return final_safe;
        }
        const auto final_reviewed_layouts = validate_reviewed_layouts(
            request, final_observation.value(), L"書込み直前");
        if (!final_reviewed_layouts) {
          return final_reviewed_layouts;
        }
        auto current_layout = build_online_direct_source_layout(
            final_observation.value().source,
            *source.value().reader,
            volume_bindings);
        if (!current_layout ||
            !same_source_layout(layout.value(), current_layout.value())) {
          return clonecore::Status::failure(
              current_layout
                  ? direct_error(
                        clonecore::ErrorCode::identity_mismatch,
                        ERROR_INVALID_DATA,
                        L"オンライン直接クローン レイアウト再検査",
                        L"Snapshot取得後にコピー元レイアウトが変更されました")
                  : current_layout.error());
        }

        const auto offline = dependencies.set_clone_target_offline(
            request.expected_source,
            request.expected_target,
            request.confirmation,
            true);
        if (!offline) {
          return offline;
        }
        destructive_phase_started = true;
        auto target = dependencies.open_offline_target(
            request.expected_target, request.confirmation);
        if (!target) {
          const auto protected_offline =
              dependencies.set_physical_target_offline(
                  request.expected_target,
                  request.confirmation,
                  true);
          return clonecore::Status::failure(append_offline_failure(
              target.error(), protected_offline));
        }
        const auto identities = clonecore::validate_clone_identities(
            request.expected_source,
            final_observation.value().source_identity,
            request.expected_target,
            target.value().observed.target_identity,
            request.confirmation);
        if (!identities) {
          target.value().target.reset();
          const auto protected_offline =
              dependencies.set_physical_target_offline(
                  request.expected_target,
                  request.confirmation,
                  true);
          return clonecore::Status::failure(append_offline_failure(
              identities.error(), protected_offline));
        }
        auto opened_target_layout = imageformat::
            hash_tsumugi_physical_restore_target_layout_v1(
                target.value().observed.target);
        if (!opened_target_layout ||
            opened_target_layout.value() !=
                request.expected_target_layout_hash) {
          target.value().target.reset();
          const auto protected_offline =
              dependencies.set_physical_target_offline(
                  request.expected_target,
                  request.confirmation,
                  true);
          return clonecore::Status::failure(append_offline_failure(
              opened_target_layout
                  ? direct_error(
                        clonecore::ErrorCode::identity_mismatch,
                        ERROR_DEVICE_REINITIALIZATION_NEEDED,
                        L"オンライン直接クローン 開いたコピー先レイアウト照合",
                        L"同じ対象ハンドルのパーティション形式または配置が最終確認と一致しません")
                  : opened_target_layout.error(),
              protected_offline));
        }

        auto executed = dependencies.execute_clone_engine(
            OnlineDirectCloneEngineContext{
                .partition_style = layout.value().partition_style,
                .request = &request,
                .observed_source =
                    &final_observation.value().source_identity,
                .observed_target =
                    &target.value().observed.target_identity,
                .source = composite.value().get(),
                .target = target.value().target.get(),
                .snapshot_bitmap_provider =
                    bitmap_provider.value().get(),
                .connected_mbr_signatures =
                    connected_mbr_signatures,
            });
        target.value().target.reset();
        if (!executed) {
          const auto protected_offline =
              dependencies.set_physical_target_offline(
                  request.expected_target,
                  request.confirmation,
                  true);
          return clonecore::Status::failure(append_offline_failure(
              executed.error(), protected_offline));
        }
        const std::uint64_t accounted_partitions =
            static_cast<std::uint64_t>(
                executed.value().copied_partition_count) +
            static_cast<std::uint64_t>(
                executed.value().recreated_partition_count);
        if (executed.value().copied_data_bytes == 0U ||
            executed.value().copied_data_bytes >
                request.expected_source.size_bytes ||
            accounted_partitions !=
                final_observation.value().source.partitions.size() ||
            !executed.value().read_back_verified ||
            all_zero(executed.value().verified_write_digest) ||
            !executed.value().partition_table_committed) {
          const auto protected_offline =
              dependencies.set_physical_target_offline(
                  request.expected_target,
                  request.confirmation,
                  true);
          return clonecore::Status::failure(append_offline_failure(
              direct_error(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"オンライン直接クローン 完了検証",
                  L"転送量、全パーティション、書込み証跡、読戻し検証、またはパーティション表確定が完了していません"),
              protected_offline));
        }

        if (boot_finalization_required) {
          const auto brought_online =
              dependencies.set_physical_target_offline(
                  request.expected_target,
                  request.confirmation,
                  false);
          if (!brought_online) {
            const auto protected_offline =
                dependencies.set_physical_target_offline(
                    request.expected_target,
                    request.confirmation,
                    true);
            return clonecore::Status::failure(append_offline_failure(
                brought_online.error(), protected_offline));
          }
          const auto boot_finalized = dependencies.finalize_target_boot(
              request.expected_target, layout.value().partition_style);
          const auto protected_offline =
              dependencies.set_physical_target_offline(
                  request.expected_target,
                  request.confirmation,
                  true);
          if (!boot_finalized) {
            return clonecore::Status::failure(append_offline_failure(
                boot_finalized.error(), protected_offline));
          }
          if (!protected_offline) {
            return protected_offline;
          }
          boot_finalization_completed = true;
        } else {
          const auto protected_offline =
              dependencies.set_physical_target_offline(
                  request.expected_target,
                  request.confirmation,
                  true);
          if (!protected_offline) {
            return protected_offline;
          }
        }
        target_offline_verified = true;
        engine_report = executed.take_value();
        return clonecore::success_status();
        } catch (...) {
          auto error = direct_error(
              clonecore::ErrorCode::internal_error,
              ERROR_UNHANDLED_EXCEPTION,
              L"オンライン直接クローン 実行例外",
              L"依存処理が例外を送出したため安全側に停止しました");
          if (!destructive_phase_started) {
            return clonecore::Status::failure(std::move(error));
          }
          const auto protected_offline =
              reprotect_target_offline_after_exception(
                  request, dependencies);
          return clonecore::Status::failure(append_offline_failure(
              std::move(error), protected_offline));
        }
      });

  if (!workflow_report) {
    if (!destructive_phase_started) {
      return clonecore::Result<OnlineDirectCloneReport>::failure(
          workflow_report.error());
    }
    const auto protected_offline =
        reprotect_target_offline_after_exception(
            request, dependencies);
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        append_offline_failure(
            workflow_report.error(), protected_offline));
  }
  if (!copy_callback_called || !engine_report.has_value() ||
      !target_offline_verified ||
      (boot_finalization_required && !boot_finalization_completed) ||
      !workflow_report.value().snapshot_data_copied ||
      !workflow_report.value().backup_completed ||
      !workflow_report.value().snapshots_deleted) {
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        direct_error(
            clonecore::ErrorCode::verification_failed,
            ERROR_INVALID_STATE,
            L"オンライン直接クローン VSS完了状態",
            L"Clone Engine、BackupComplete、Snapshot削除、またはoffline検証が完了していません"));
  }
  return clonecore::Result<OnlineDirectCloneReport>::success(
      OnlineDirectCloneReport{
          .partition_style = layout.value().partition_style,
          .copied_data_bytes = engine_report->copied_data_bytes,
          .copied_partition_count =
              engine_report->copied_partition_count,
          .recreated_partition_count =
              engine_report->recreated_partition_count,
          .verified_write_digest =
              engine_report->verified_write_digest,
          .read_back_verified = engine_report->read_back_verified,
          .partition_table_committed =
              engine_report->partition_table_committed,
          .snapshot_backup_completed =
              workflow_report.value().backup_completed,
          .snapshots_deleted =
              workflow_report.value().snapshots_deleted,
          .target_left_offline = true,
          .boot_finalization_required = boot_finalization_required,
          .boot_finalization_completed = boot_finalization_completed,
      });
}

OnlineDirectCloneDependencies
make_online_direct_clone_windows_dependencies() {
  return OnlineDirectCloneDependencies{
          .reidentify_clone_selection =
              [](const clonecore::StableDiskIdentity& source,
                 const clonecore::StableDiskIdentity& target) {
                auto inventory =
                    diskmodel::make_windows_disk_inventory_provider();
                return diskmodel::reidentify_physical_clone_selection(
                    source, target, *inventory);
              },
          .reidentify_clone =
              [](const clonecore::StableDiskIdentity& source,
                 const clonecore::StableDiskIdentity& target,
                 const clonecore::TargetConfirmation& confirmation) {
                auto inventory =
                    diskmodel::make_windows_disk_inventory_provider();
                return diskmodel::reidentify_physical_clone(
                    source,
                    target,
                    confirmation,
                    *inventory);
              },
          .open_read_only_source =
              [](const clonecore::StableDiskIdentity& source) {
                return diskmodel::
                    open_verified_read_only_physical_disk_with_windows_apis(
                        source);
              },
          .query_gpt_bindings =
              [](const diskmodel::DiskInfo& disk,
                 const clonecore::GptDisk& gpt) {
                return diskmodel::query_windows_volume_bitmap_bindings(
                    disk, gpt);
              },
          .query_mbr_bindings =
              [](const diskmodel::DiskInfo& disk,
                 const clonecore::MbrDisk& mbr) {
                return diskmodel::query_windows_volume_bitmap_bindings(
                    disk, mbr);
              },
          .run_snapshot_workflow =
              [](const vssrequester::WorkflowRequest& workflow,
                 const vssrequester::AsyncWaitOptions& async_wait,
                 const clonecore::Logger* logger,
                 vssrequester::SnapshotCopyCallback callback) {
                vssrequester::WindowsVssBackend backend(
                    vssrequester::WindowsVssBackendOptions{
                        .async_wait = async_wait,
                        .copy_snapshot_data = std::move(callback),
                        .logger = logger,
                    });
                return vssrequester::execute_backup_workflow(
                    workflow, backend);
              },
          .open_snapshot_reader =
              [](const vssrequester::SnapshotVolumeOpenRequest& open) {
                return vssrequester::
                    open_snapshot_volume_reader_with_windows_apis(open);
              },
          .make_snapshot_bitmap_provider =
              [](std::vector<clonecore::SnapshotVolumeBitmapBinding>
                     bindings) {
                std::unique_ptr<clonecore::INtfsUsedRangeProvider> provider =
                    std::make_unique<
                        clonecore::WindowsSnapshotVolumeBitmapProvider>(
                        std::move(bindings));
                return clonecore::Result<std::unique_ptr<
                    clonecore::INtfsUsedRangeProvider>>::success(
                    std::move(provider));
              },
          .set_clone_target_offline =
              [](const clonecore::StableDiskIdentity& source,
                 const clonecore::StableDiskIdentity& target,
                 const clonecore::TargetConfirmation& confirmation,
                 const bool offline) {
                return diskmodel::
                    set_verified_target_offline_with_windows_apis(
                        source, target, confirmation, offline);
              },
          .set_physical_target_offline =
              [](const clonecore::StableDiskIdentity& target,
                 const clonecore::TargetConfirmation& confirmation,
                 const bool offline) {
                return diskmodel::
                    set_verified_physical_target_offline_with_windows_apis(
                        target, confirmation, offline);
              },
          .open_offline_target =
              [](const clonecore::StableDiskIdentity& target,
                 const clonecore::TargetConfirmation& confirmation) {
                return diskmodel::
                    open_verified_physical_target_with_windows_apis(
                        target, confirmation);
              },
          .collect_mbr_signatures =
              collect_connected_mbr_signatures_with_windows_apis,
          .execute_clone_engine = execute_clone_engine_with_native_core,
          .finalize_target_boot =
              [](const clonecore::StableDiskIdentity& target,
                 const OnlineDirectClonePartitionStyle style) {
                auto inventory =
                    diskmodel::make_windows_disk_inventory_provider();
                auto finalizer = bootrepair::
                    make_windows_clone_boot_finalization_service(*inventory);
                const auto finalized = finalizer->execute(
                    bootrepair::CloneBootFinalizationRequest{
                        .expected_target = target,
                        .expected_style =
                            style == OnlineDirectClonePartitionStyle::gpt
                            ? diskmodel::PartitionStyle::gpt
                            : diskmodel::PartitionStyle::mbr,
                    });
                if (!finalized) {
                  return clonecore::Status::failure(finalized.error());
                }
                return clonecore::success_status();
              },
      };
}

clonecore::Result<OnlineDirectCloneReport>
execute_online_direct_clone_with_windows_apis(
    const OnlineDirectCloneRequest& request) {
  return execute_online_direct_clone(
      request, make_online_direct_clone_windows_dependencies());
}

}  // namespace ytec::windowsapp
