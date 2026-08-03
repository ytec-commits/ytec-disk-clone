#include "ytec/migrationcore/shrink_layout.h"

#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

ytec::migrationcore::ShrinkMigrationRequest gpt_windows_request(
    const std::uint64_t target_size = 500ULL * kGiB) {
  using namespace ytec::migrationcore;
  return ShrinkMigrationRequest{
      .source_style = MigrationPartitionStyle::gpt,
      .target_style = MigrationPartitionStyle::gpt,
      .target_size_bytes = target_size,
      .target_logical_sector_size = 512,
      .source_is_windows_system = true,
      .windows_is_amd64 = true,
      .bitlocker_fully_decrypted = true,
      .source_partitions = {
          ShrinkSourcePartition{
              .source_table_index = 0,
              .role = MigrationPartitionRole::efi_system,
              .file_system = MigrationFileSystem::fat32,
              .source_size_bytes = 200ULL * kMiB,
          },
          ShrinkSourcePartition{
              .source_table_index = 1,
              .role = MigrationPartitionRole::microsoft_reserved,
              .file_system = MigrationFileSystem::none,
              .source_size_bytes = 16ULL * kMiB,
          },
          ShrinkSourcePartition{
              .source_table_index = 2,
              .role = MigrationPartitionRole::windows,
              .file_system = MigrationFileSystem::ntfs,
              .source_size_bytes = 998ULL * kGiB,
              .used_bytes = 300ULL * kGiB,
              .cluster_size = 4096,
              .label = L"Windows",
          },
          ShrinkSourcePartition{
              .source_table_index = 3,
              .role = MigrationPartitionRole::recovery,
              .file_system = MigrationFileSystem::ntfs,
              .source_size_bytes = 1ULL * kGiB,
              .used_bytes = 600ULL * kMiB,
              .cluster_size = 4096,
              .label = L"Recovery",
          },
      },
  };
}

void test_one_tb_300_used_fits_500_gib_target() {
  using namespace ytec::migrationcore;
  const auto result = plan_shrink_migration(gpt_windows_request());
  check(result.has_value(), "1 TB / 300 GiB used should fit a 500 GiB target");
  check(
      result.value().source_remains_unchanged &&
          result.value().boot_finalization_required &&
          result.value().minimum_target_size_bytes < 500ULL * kGiB &&
          result.value().target_partitions.size() == 4,
      "Windows GPT plan should recreate ESP/MSR and retain Windows/recovery");
  const auto& partitions = result.value().target_partitions;
  check(
      partitions[0].role == MigrationPartitionRole::efi_system &&
          partitions[1].role == MigrationPartitionRole::microsoft_reserved &&
          partitions[2].role == MigrationPartitionRole::windows &&
          partitions[3].role == MigrationPartitionRole::recovery &&
          partitions[2].size_bytes > 300ULL * kGiB,
      "Windows target order and free-space reserve should be deterministic");
}

void test_target_below_used_plus_reserve_fails_closed() {
  const auto result = ytec::migrationcore::plan_shrink_migration(
      gpt_windows_request(305ULL * kGiB));
  check(!result.has_value(), "A target below used bytes plus reserve must fail");
  check(
      result.error().native_code == ERROR_DISK_FULL,
      "Capacity failure should report disk full before any I/O");
}

void test_gpt_data_disk_uses_space_without_boot_partitions() {
  using namespace ytec::migrationcore;
  const auto result = plan_shrink_migration(ShrinkMigrationRequest{
      .source_style = MigrationPartitionStyle::gpt,
      .target_style = MigrationPartitionStyle::gpt,
      .target_size_bytes = 250ULL * kGiB,
      .target_logical_sector_size = 512,
      .source_is_windows_system = false,
      .windows_is_amd64 = false,
      .bitlocker_fully_decrypted = true,
      .source_partitions = {
          ShrinkSourcePartition{
              .source_table_index = 0,
              .role = MigrationPartitionRole::microsoft_reserved,
              .file_system = MigrationFileSystem::none,
              .source_size_bytes = 16ULL * kMiB,
          },
          ShrinkSourcePartition{
              .source_table_index = 1,
              .role = MigrationPartitionRole::data,
              .file_system = MigrationFileSystem::ntfs,
              .source_size_bytes = 600ULL * kGiB,
              .used_bytes = 100ULL * kGiB,
              .cluster_size = 4096,
              .label = L"DATA-1",
          },
          ShrinkSourcePartition{
              .source_table_index = 2,
              .role = MigrationPartitionRole::data,
              .file_system = MigrationFileSystem::ntfs,
              .source_size_bytes = 400ULL * kGiB,
              .used_bytes = 50ULL * kGiB,
              .cluster_size = 4096,
              .label = L"DATA-2",
          },
      },
  });
  check(result.has_value(), "Two basic NTFS data volumes should be shrinkable");
  check(
      !result.value().boot_finalization_required &&
          result.value().target_partitions.size() == 3 &&
          result.value().target_partitions[0].role ==
              MigrationPartitionRole::microsoft_reserved &&
          result.value().target_partitions[1].role ==
              MigrationPartitionRole::data &&
          result.value().target_partitions[2].role ==
              MigrationPartitionRole::data,
      "Data-only GPT should preserve data/MSR roles and skip boot finalization");
}

void test_mbr_system_reserved_is_preserved_as_active_content() {
  using namespace ytec::migrationcore;
  const auto result = plan_shrink_migration(ShrinkMigrationRequest{
      .source_style = MigrationPartitionStyle::mbr,
      .target_style = MigrationPartitionStyle::mbr,
      .target_size_bytes = 200ULL * kGiB,
      .target_logical_sector_size = 512,
      .source_is_windows_system = true,
      .windows_is_amd64 = true,
      .bitlocker_fully_decrypted = true,
      .source_partitions = {
          ShrinkSourcePartition{
              .source_table_index = 1,
              .role = MigrationPartitionRole::windows,
              .file_system = MigrationFileSystem::ntfs,
              .source_size_bytes = 450ULL * kGiB,
              .used_bytes = 100ULL * kGiB,
              .cluster_size = 4096,
              .label = L"Windows",
          },
          ShrinkSourcePartition{
              .source_table_index = 0,
              .role = MigrationPartitionRole::bios_system,
              .file_system = MigrationFileSystem::ntfs,
              .source_size_bytes = 500ULL * kMiB,
              .used_bytes = 80ULL * kMiB,
              .cluster_size = 4096,
              .label = L"System Reserved",
              .active = true,
          },
      },
  });
  check(result.has_value(), "Common BIOS System Reserved layout should plan");
  check(
      result.value().target_partitions[0].role ==
              MigrationPartitionRole::bios_system &&
          result.value().target_partitions[0].active &&
          result.value().target_partitions[1].role ==
              MigrationPartitionRole::windows,
      "BIOS system partition should stay first and active");
}

void test_encrypted_or_cross_style_request_is_rejected() {
  auto request = gpt_windows_request();
  request.bitlocker_fully_decrypted = false;
  check(
      !ytec::migrationcore::plan_shrink_migration(request).has_value(),
      "BitLocker must be fully decrypted");
  request = gpt_windows_request();
  request.target_style = ytec::migrationcore::MigrationPartitionStyle::mbr;
  check(
      !ytec::migrationcore::plan_shrink_migration(request).has_value(),
      "Shrink mode must not silently combine format conversion");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"one_tb_300_used_fits_500_gib_target",
       test_one_tb_300_used_fits_500_gib_target},
      {"target_below_used_plus_reserve_fails_closed",
       test_target_below_used_plus_reserve_fails_closed},
      {"gpt_data_disk_uses_space_without_boot_partitions",
       test_gpt_data_disk_uses_space_without_boot_partitions},
      {"mbr_system_reserved_is_preserved_as_active_content",
       test_mbr_system_reserved_is_preserved_as_active_content},
      {"encrypted_or_cross_style_request_is_rejected",
       test_encrypted_or_cross_style_request_is_rejected},
  };
  int failures = 0;
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
