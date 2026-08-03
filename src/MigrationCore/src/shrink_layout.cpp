#include "ytec/migrationcore/shrink_layout.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ytec::migrationcore {
namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;
constexpr std::uint64_t kAlignment = 1ULL * kMiB;
constexpr std::uint64_t kMbrMetadataReserve = 1ULL * kMiB;
constexpr std::uint64_t kGptMetadataReserve = 2ULL * kMiB;
constexpr std::uint64_t kMsrSize = 16ULL * kMiB;
constexpr std::uint64_t kRecoveryMinimum = 990ULL * kMiB;

clonecore::Error layout_error(
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
  return clonecore::Result<T>::failure(layout_error(
      code, native_code, std::move(operation), std::move(message)));
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

clonecore::Result<std::uint64_t> align_up(
    const std::uint64_t value,
    const std::uint64_t alignment) {
  if (alignment == 0U || value >
          std::numeric_limits<std::uint64_t>::max() - (alignment - 1U)) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"縮小移行の境界整列",
        L"容量の整列計算がオーバーフローしました");
  }
  return clonecore::Result<std::uint64_t>::success(
      ((value + alignment - 1U) / alignment) * alignment);
}

std::uint64_t esp_size(const std::uint32_t logical_sector_size) noexcept {
  return logical_sector_size == 4096U ? 300ULL * kMiB : 200ULL * kMiB;
}

clonecore::Result<std::uint64_t> content_minimum_size(
    const ShrinkSourcePartition& source) {
  if (source.used_bytes > source.source_size_bytes) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"縮小移行の使用容量",
        L"使用容量がコピー元パーティション容量を超えています");
  }
  const std::uint64_t percentage_reserve = source.used_bytes / 8U;
  const std::uint64_t fixed_reserve =
      source.role == MigrationPartitionRole::windows ? 10ULL * kGiB
                                                     : 1ULL * kGiB;
  const std::uint64_t reserve =
      (std::max)(percentage_reserve, fixed_reserve);
  std::uint64_t requested{};
  if (!checked_add(source.used_bytes, reserve, requested)) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"縮小移行の安全余白",
        L"使用容量と安全余白の合計がオーバーフローしました");
  }
  if (source.role == MigrationPartitionRole::recovery) {
    requested = (std::max)(requested, kRecoveryMinimum);
  }
  if (source.used_bytes == 0U) {
    requested = source.role == MigrationPartitionRole::recovery
        ? kRecoveryMinimum
        : 1ULL * kGiB;
  }
  return align_up(requested, kAlignment);
}

bool is_content_role(const MigrationPartitionRole role) noexcept {
  return role == MigrationPartitionRole::bios_system ||
      role == MigrationPartitionRole::windows ||
      role == MigrationPartitionRole::recovery ||
      role == MigrationPartitionRole::data;
}

}  // namespace

clonecore::Result<ShrinkMigrationPlan> plan_shrink_migration(
    const ShrinkMigrationRequest& request) {
  if (request.target_size_bytes == 0U ||
      (request.source_style != MigrationPartitionStyle::mbr &&
       request.source_style != MigrationPartitionStyle::gpt) ||
      (request.target_style != MigrationPartitionStyle::mbr &&
       request.target_style != MigrationPartitionStyle::gpt) ||
      (request.target_logical_sector_size != 512U &&
       request.target_logical_sector_size != 4096U) ||
      request.target_size_bytes % request.target_logical_sector_size != 0U ||
      request.source_partitions.empty() ||
      request.source_partitions.size() > 128U ||
      !request.bitlocker_fully_decrypted) {
    return failure<ShrinkMigrationPlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"縮小移行の共通条件",
        L"コピー先寸法、論理セクター、BitLocker、またはパーティション数が対応条件外です");
  }
  if (request.source_style != request.target_style) {
    return failure<ShrinkMigrationPlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"縮小移行のパーティション形式",
        L"縮小移行ではMBR/GPT形式を維持します。形式変換は専用モードを使用してください");
  }
  if (request.source_is_windows_system && !request.windows_is_amd64) {
    return failure<ShrinkMigrationPlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"縮小移行のWindowsアーキテクチャ",
        L"起動可能な縮小移行はAMD64版Windowsだけを対象にします");
  }

  std::size_t windows_count = 0U;
  std::size_t bios_system_count = 0U;
  std::size_t efi_count = 0U;
  std::size_t msr_count = 0U;
  std::size_t active_count = 0U;
  std::vector<const ShrinkSourcePartition*> content;
  for (std::size_t index = 0; index < request.source_partitions.size(); ++index) {
    const auto& source = request.source_partitions[index];
    if (source.source_size_bytes == 0U ||
        source.source_size_bytes % request.target_logical_sector_size != 0U ||
        source.used_bytes > source.source_size_bytes ||
        source.label.size() > 32U) {
      return failure<ShrinkMigrationPlan>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"縮小移行のコピー元パーティション",
          L"容量、使用量、整列、またはラベルが不正です");
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (request.source_partitions[previous].source_table_index ==
          source.source_table_index) {
        return failure<ShrinkMigrationPlan>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"縮小移行のコピー元番号",
            L"パーティション表番号が重複しています");
      }
    }
    if (source.role == MigrationPartitionRole::efi_system) {
      ++efi_count;
      continue;
    }
    if (source.role == MigrationPartitionRole::microsoft_reserved) {
      ++msr_count;
      continue;
    }
    if (!is_content_role(source.role) ||
        source.file_system != MigrationFileSystem::ntfs ||
        source.cluster_size < request.target_logical_sector_size ||
        source.cluster_size > 2ULL * kMiB ||
        source.cluster_size % request.target_logical_sector_size != 0U) {
      return failure<ShrinkMigrationPlan>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"縮小移行のファイルシステム",
          L"基本NTFSパーティションだけをファイル単位で縮小移行できます");
    }
    if (source.role == MigrationPartitionRole::windows) {
      ++windows_count;
    }
    if (source.role == MigrationPartitionRole::bios_system) {
      ++bios_system_count;
    }
    if (source.active) {
      ++active_count;
    }
    content.push_back(&source);
  }

  const bool system_roles_invalid = request.source_is_windows_system &&
      (windows_count != 1U ||
       (request.source_style == MigrationPartitionStyle::gpt
            ? efi_count != 1U || msr_count != 1U ||
                  bios_system_count != 0U
            : efi_count != 0U || msr_count != 0U ||
                  bios_system_count > 1U || active_count != 1U));
  const bool data_roles_invalid = !request.source_is_windows_system &&
      (windows_count != 0U || bios_system_count != 0U ||
       efi_count != 0U || active_count != 0U ||
       (request.source_style == MigrationPartitionStyle::gpt
            ? msr_count > 1U
            : msr_count != 0U));
  if (content.empty() || system_roles_invalid || data_roles_invalid) {
    return failure<ShrinkMigrationPlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"縮小移行のディスク役割",
        L"Windows起動ディスクまたはデータ専用ディスクの役割を一意に確定できません");
  }

  ShrinkMigrationPlan plan{
      .target_style = request.target_style,
      .alignment_bytes = kAlignment,
      .target_size_bytes = request.target_size_bytes,
      .source_remains_unchanged = true,
      .boot_finalization_required = request.source_is_windows_system,
  };
  std::uint64_t cursor = request.target_style == MigrationPartitionStyle::gpt
      ? kGptMetadataReserve
      : kMbrMetadataReserve;
  std::uint32_t target_number = 1U;

  auto append_fixed = [&](
      const MigrationPartitionRole role,
      const MigrationFileSystem file_system,
      const MigrationPartitionAction action,
      const std::uint64_t size,
      const std::wstring& label) -> clonecore::Status {
    const auto aligned_cursor = align_up(cursor, kAlignment);
    if (!aligned_cursor ||
        !checked_add(aligned_cursor.value(), size, cursor)) {
      return clonecore::Status::failure(
          aligned_cursor ? layout_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_ARITHMETIC_OVERFLOW,
              L"縮小移行の固定領域配置",
              L"固定領域の終端がオーバーフローしました")
                         : aligned_cursor.error());
    }
    plan.target_partitions.push_back(ShrinkPlannedPartition{
        .target_number = target_number++,
        .source_table_index = std::nullopt,
        .role = role,
        .file_system = file_system,
        .action = action,
        .offset_bytes = aligned_cursor.value(),
        .size_bytes = size,
        .label = label,
    });
    return clonecore::success_status();
  };

  if (request.source_style == MigrationPartitionStyle::gpt &&
      request.source_is_windows_system) {
    auto status = append_fixed(
        MigrationPartitionRole::efi_system,
        MigrationFileSystem::fat32,
        MigrationPartitionAction::create_fat32,
        esp_size(request.target_logical_sector_size),
        L"SYSTEM");
    if (!status) {
      return clonecore::Result<ShrinkMigrationPlan>::failure(status.error());
    }
    status = append_fixed(
        MigrationPartitionRole::microsoft_reserved,
        MigrationFileSystem::none,
        MigrationPartitionAction::create_reserved,
        kMsrSize,
        L"");
    if (!status) {
      return clonecore::Result<ShrinkMigrationPlan>::failure(status.error());
    }
  } else if (request.source_style == MigrationPartitionStyle::gpt &&
             msr_count == 1U) {
    const auto status = append_fixed(
        MigrationPartitionRole::microsoft_reserved,
        MigrationFileSystem::none,
        MigrationPartitionAction::create_reserved,
        kMsrSize,
        L"");
    if (!status) {
      return clonecore::Result<ShrinkMigrationPlan>::failure(status.error());
    }
  }

  if (request.source_is_windows_system) {
    std::stable_sort(
        content.begin(),
        content.end(),
        [](const auto* left, const auto* right) {
          const auto rank = [](const MigrationPartitionRole role) {
            switch (role) {
              case MigrationPartitionRole::bios_system:
                return 0;
              case MigrationPartitionRole::windows:
                return 1;
              case MigrationPartitionRole::recovery:
                return 2;
              case MigrationPartitionRole::data:
                return 3;
              default:
                return 4;
            }
          };
          return rank(left->role) < rank(right->role);
        });
  }

  for (const auto* source : content) {
    const auto minimum = content_minimum_size(*source);
    const auto aligned_cursor = align_up(cursor, kAlignment);
    if (!minimum || !aligned_cursor ||
        !checked_add(aligned_cursor.value(), minimum.value(), cursor)) {
      return failure<ShrinkMigrationPlan>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"縮小移行の内容領域配置",
          L"内容領域の容量または終端を安全に計算できません");
    }
    plan.target_partitions.push_back(ShrinkPlannedPartition{
        .target_number = target_number++,
        .source_table_index = source->source_table_index,
        .role = source->role,
        .file_system = MigrationFileSystem::ntfs,
        .action = source->used_bytes == 0U
            ? MigrationPartitionAction::create_empty_ntfs
            : MigrationPartitionAction::apply_file_image,
        .offset_bytes = aligned_cursor.value(),
        .size_bytes = minimum.value(),
        .source_used_bytes = source->used_bytes,
        .label = source->label,
        .active = request.source_style == MigrationPartitionStyle::mbr &&
            source->active,
    });
  }

  const std::uint64_t tail_reserve =
      request.target_style == MigrationPartitionStyle::gpt
          ? kGptMetadataReserve
          : 0U;
  std::uint64_t minimum_target{};
  if (!checked_add(cursor, tail_reserve, minimum_target)) {
    return failure<ShrinkMigrationPlan>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"縮小移行の最小ディスク容量",
        L"最小ディスク容量がオーバーフローしました");
  }
  const auto aligned_minimum = align_up(minimum_target, kAlignment);
  if (!aligned_minimum) {
    return clonecore::Result<ShrinkMigrationPlan>::failure(
        aligned_minimum.error());
  }
  plan.minimum_target_size_bytes = aligned_minimum.value();
  if (request.target_size_bytes < plan.minimum_target_size_bytes) {
    return failure<ShrinkMigrationPlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_DISK_FULL,
        L"縮小移行のコピー先容量",
        L"実使用量、安全余白、および必須領域がコピー先に収まりません。必要最小容量: " +
            std::to_wstring(plan.minimum_target_size_bytes) + L" bytes");
  }

  const std::uint64_t extra =
      request.target_size_bytes - plan.minimum_target_size_bytes;
  if (extra >= kAlignment) {
    const std::uint64_t distributable = (extra / kAlignment) * kAlignment;
    auto expandable = std::find_if(
        plan.target_partitions.begin(),
        plan.target_partitions.end(),
        [](const ShrinkPlannedPartition& partition) {
          return partition.role == MigrationPartitionRole::windows;
        });
    if (expandable == plan.target_partitions.end()) {
      expandable = std::max_element(
          plan.target_partitions.begin(),
          plan.target_partitions.end(),
          [](const auto& left, const auto& right) {
            const bool left_data = left.role == MigrationPartitionRole::data;
            const bool right_data = right.role == MigrationPartitionRole::data;
            if (left_data != right_data) {
              return !left_data;
            }
            return left.source_used_bytes < right.source_used_bytes;
          });
    }
    if (expandable != plan.target_partitions.end() &&
        expandable->role != MigrationPartitionRole::efi_system &&
        expandable->role != MigrationPartitionRole::microsoft_reserved) {
      const std::uint64_t growth = distributable;
      expandable->size_bytes += growth;
      bool after_expandable = false;
      for (auto& partition : plan.target_partitions) {
        if (&partition == &*expandable) {
          after_expandable = true;
          continue;
        }
        if (after_expandable) {
          partition.offset_bytes += growth;
        }
      }
      cursor += growth;
    }
  }
  plan.unallocated_tail_bytes = request.target_size_bytes - cursor;
  plan.notes.push_back(
      L"コピー元は読み取り専用のまま、コピー先パーティションだけを新規作成します");
  plan.notes.push_back(
      L"NTFS内容はMicrosoft DISM/WIMのファイル単位イメージで移行します");
  if (plan.boot_finalization_required) {
    plan.notes.push_back(
        L"展開後にコピー先だけへBCDBootで起動構成を新規作成します");
  } else {
    plan.notes.push_back(
        L"データ専用ディスクのため起動構成は作成しません");
  }
  return clonecore::Result<ShrinkMigrationPlan>::success(std::move(plan));
}

std::wstring_view migration_partition_role_name(
    const MigrationPartitionRole role) noexcept {
  switch (role) {
    case MigrationPartitionRole::efi_system:
      return L"EFIシステム";
    case MigrationPartitionRole::microsoft_reserved:
      return L"Microsoft予約";
    case MigrationPartitionRole::bios_system:
      return L"BIOSシステム";
    case MigrationPartitionRole::windows:
      return L"Windows";
    case MigrationPartitionRole::recovery:
      return L"回復";
    case MigrationPartitionRole::data:
      return L"データ";
  }
  return L"不明";
}

}  // namespace ytec::migrationcore
