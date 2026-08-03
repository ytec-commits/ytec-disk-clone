#pragma once

#include "ytec/clonecore/result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ytec::migrationcore {

enum class MigrationPartitionStyle : std::uint8_t {
  mbr,
  gpt,
};

enum class MigrationFileSystem : std::uint8_t {
  none,
  ntfs,
  fat32,
};

enum class MigrationPartitionRole : std::uint8_t {
  efi_system,
  microsoft_reserved,
  bios_system,
  windows,
  recovery,
  data,
};

enum class MigrationPartitionAction : std::uint8_t {
  create_fat32,
  create_reserved,
  apply_file_image,
  create_empty_ntfs,
};

struct ShrinkSourcePartition final {
  std::uint32_t source_table_index{};
  MigrationPartitionRole role{MigrationPartitionRole::data};
  MigrationFileSystem file_system{MigrationFileSystem::ntfs};
  std::uint64_t source_size_bytes{};
  std::uint64_t used_bytes{};
  std::uint64_t cluster_size{};
  std::wstring label;
  bool active{};
};

struct ShrinkMigrationRequest final {
  MigrationPartitionStyle source_style{MigrationPartitionStyle::gpt};
  MigrationPartitionStyle target_style{MigrationPartitionStyle::gpt};
  std::uint64_t target_size_bytes{};
  std::uint32_t target_logical_sector_size{};
  bool source_is_windows_system{};
  bool windows_is_amd64{};
  bool bitlocker_fully_decrypted{};
  std::vector<ShrinkSourcePartition> source_partitions;
};

struct ShrinkPlannedPartition final {
  std::uint32_t target_number{};
  std::optional<std::uint32_t> source_table_index;
  MigrationPartitionRole role{MigrationPartitionRole::data};
  MigrationFileSystem file_system{MigrationFileSystem::ntfs};
  MigrationPartitionAction action{
      MigrationPartitionAction::apply_file_image};
  std::uint64_t offset_bytes{};
  std::uint64_t size_bytes{};
  std::uint64_t source_used_bytes{};
  std::wstring label;
  bool active{};
};

struct ShrinkMigrationPlan final {
  MigrationPartitionStyle target_style{MigrationPartitionStyle::gpt};
  std::uint64_t alignment_bytes{};
  std::uint64_t minimum_target_size_bytes{};
  std::uint64_t target_size_bytes{};
  std::uint64_t unallocated_tail_bytes{};
  bool source_remains_unchanged{true};
  bool boot_finalization_required{};
  std::vector<ShrinkPlannedPartition> target_partitions;
  std::vector<std::wstring> notes;
};

// Builds a target-only reconstruction plan. It performs no I/O and never
// proposes shrinking or modifying the source. Only basic NTFS content volumes
// are accepted; boot partitions are recreated on the target.
[[nodiscard]] clonecore::Result<ShrinkMigrationPlan>
plan_shrink_migration(const ShrinkMigrationRequest& request);

[[nodiscard]] std::wstring_view migration_partition_role_name(
    MigrationPartitionRole role) noexcept;

}  // namespace ytec::migrationcore
