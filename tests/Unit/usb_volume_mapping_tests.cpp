#include "ytec/windowsapp/usb_volume_mapping.h"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TestFailure final {
  std::string message;
};

void check(const bool condition, std::string message) {
  if (!condition) {
    throw TestFailure{std::move(message)};
  }
}

ytec::diskmodel::DiskInfo safe_usb() {
  constexpr std::uint64_t disk_size =
      32ULL * 1024ULL * 1024ULL * 1024ULL;
  constexpr std::uint64_t partition_offset =
      1024ULL * 1024ULL;
  ytec::diskmodel::DiskInfo disk;
  disk.disk_number = 7U;
  disk.device_instance_id =
      L"USB\\VID_1234&PID_5678\\MAPPING-MOCK";
  disk.model = L"Mock USB Memory";
  disk.size_bytes = disk_size;
  disk.logical_sector_size = 512U;
  disk.physical_sector_size = 4096U;
  disk.bus_type = L"USB";
  disk.serial_suffix = "A1B2C3D4";
  disk.partition_style = ytec::diskmodel::PartitionStyle::mbr;
  disk.offline = false;
  disk.read_only = false;
  disk.removable = true;
  disk.is_system_disk = false;
  disk.partitions.push_back({
      .number = 1U,
      .offset_bytes = partition_offset,
      .size_bytes = disk_size - partition_offset,
      .style = ytec::diskmodel::PartitionStyle::mbr,
  });
  return disk;
}

ytec::windowsapp::DriveLetterVolume target_volume(
    const wchar_t letter = L'E') {
  const auto disk = safe_usb();
  return {
      .drive_letter = letter,
      .extents =
          {{
              .disk_number = disk.disk_number,
              .starting_offset =
                  disk.partitions.front().offset_bytes,
              .length = disk.partitions.front().size_bytes,
          }},
  };
}

void test_unique_mapping_succeeds_without_write() {
  const auto disk = safe_usb();
  std::vector<ytec::windowsapp::DriveLetterVolume> volumes{
      {
          .drive_letter = L'C',
          .extents =
              {{
                  .disk_number = 0U,
                  .starting_offset = 1024ULL * 1024ULL,
                  .length = 100ULL * 1024ULL * 1024ULL,
              }},
      },
      target_volume(L'e'),
  };
  const auto result =
      ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, volumes);
  check(static_cast<bool>(result), "Unique USB mapping should pass");
  check(result.value().drive_letter == L'E',
        "Drive letter should be normalized to uppercase");
  check(result.value().root_path == L"E:\\",
        "Root path should be canonical");
  check(result.value().partition_number == 1U,
        "Mapped partition should be preserved");
  check(result.value().target_identity.disk_number == 7U,
        "Stable target identity should be preserved");
  check(!result.value().physical_write_started,
        "Read-only mapping must never start a physical write");
}

void test_spanned_volume_is_rejected() {
  const auto disk = safe_usb();
  auto volume = target_volume();
  volume.extents.push_back({
      .disk_number = 8U,
      .starting_offset = 1024ULL * 1024ULL,
      .length = 1024ULL * 1024ULL,
  });
  const auto result =
      ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, std::vector{volume});
  check(!result, "Spanned USB volume must fail closed");
  check(
      result.error().code ==
          ytec::clonecore::ErrorCode::unsupported_layout,
      "Spanned volume should report unsupported layout");
}

void test_multiple_drive_letters_are_rejected() {
  const auto disk = safe_usb();
  const std::vector volumes{
      target_volume(L'E'),
      target_volume(L'F'),
  };
  const auto result =
      ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, volumes);
  check(!result, "Ambiguous USB letters must fail closed");
  check(result.error().code ==
            ytec::clonecore::ErrorCode::identity_mismatch,
        "Ambiguous letters should report identity mismatch");
}

void test_out_of_range_extent_is_rejected() {
  const auto disk = safe_usb();
  auto volume = target_volume();
  volume.extents.front().starting_offset = disk.size_bytes - 100U;
  volume.extents.front().length = 200U;
  const auto result =
      ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, std::vector{volume});
  check(!result, "Out-of-range extent must fail closed");
}

void test_partition_mismatch_is_rejected() {
  const auto disk = safe_usb();
  auto volume = target_volume();
  volume.extents.front().starting_offset += 512U;
  volume.extents.front().length -= 512U;
  const auto result =
      ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, std::vector{volume});
  check(!result, "Extent not matching a partition must fail closed");
}

void test_no_target_mapping_is_rejected() {
  const auto disk = safe_usb();
  const std::vector<ytec::windowsapp::DriveLetterVolume> volumes{
      {
          .drive_letter = L'C',
          .extents =
              {{
                  .disk_number = 0U,
                  .starting_offset = 1024ULL * 1024ULL,
                  .length = 100ULL * 1024ULL * 1024ULL,
              }},
      },
  };
  const auto result =
      ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, volumes);
  check(!result, "Missing target letter must fail closed");
}

void test_unsafe_target_states_are_rejected() {
  auto disk = safe_usb();
  disk.is_system_disk = true;
  check(
      !ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, std::vector{target_volume()}),
      "System disk must be rejected");

  disk = safe_usb();
  disk.bus_type = L"NVMe";
  check(
      !ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, std::vector{target_volume()}),
      "Non-USB disk must be rejected");

  disk = safe_usb();
  disk.read_only = true;
  check(
      !ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, std::vector{target_volume()}),
      "Read-only USB must be rejected as a write target");

  disk = safe_usb();
  disk.offline = true;
  check(
      !ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, std::vector{target_volume()}),
      "Offline USB must be rejected");
}

void test_unstable_identity_is_rejected() {
  auto disk = safe_usb();
  disk.serial_suffix.clear();
  disk.device_instance_id.clear();
  const auto result =
      ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, std::vector{target_volume()});
  check(!result, "Unstable USB identity must fail closed");
}

}  // namespace

int main() {
  try {
    test_unique_mapping_succeeds_without_write();
    test_spanned_volume_is_rejected();
    test_multiple_drive_letters_are_rejected();
    test_out_of_range_extent_is_rejected();
    test_partition_mismatch_is_rejected();
    test_no_target_mapping_is_rejected();
    test_unsafe_target_states_are_rejected();
    test_unstable_identity_is_rejected();
    std::cout << "usb volume mapping tests passed\n";
    return 0;
  } catch (const TestFailure& failure) {
    std::cerr << failure.message << '\n';
    return 1;
  }
}
