#pragma once

#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ytec::windowsapp {

struct DriveLetterDiskExtent final {
  std::uint32_t disk_number{};
  std::uint64_t starting_offset{};
  std::uint64_t length{};
};

struct DriveLetterVolume final {
  wchar_t drive_letter{};
  std::vector<DriveLetterDiskExtent> extents;
};

struct RescueUsbDriveLetterResolution final {
  clonecore::StableDiskIdentity target_identity;
  wchar_t drive_letter{};
  std::wstring root_path;
  std::uint32_t partition_number{};
  std::uint64_t extent_start{};
  std::uint64_t extent_length{};
  bool physical_write_started{};
};

// Resolves a previously inventoried USB disk to exactly one local drive
// letter. The supplied volume list is read-only evidence and is injectable for
// tests. Spanned, ambiguous, out-of-range, system, non-USB, read-only, offline
// and unstable targets fail closed. This function never opens a device.
[[nodiscard]] clonecore::Result<RescueUsbDriveLetterResolution>
resolve_rescue_usb_drive_letter(
    const diskmodel::DiskInfo& target,
    std::span<const DriveLetterVolume> volumes);

// Enumerates local fixed/removable drive letters and their physical extents
// with zero desired access. No lock, dismount, format or write IOCTL is sent.
[[nodiscard]] clonecore::Result<std::vector<DriveLetterVolume>>
enumerate_windows_drive_letter_volumes_read_only();

// Windows adapter for the pure resolver. It performs only the read-only
// enumeration above and returns physical_write_started=false.
[[nodiscard]] clonecore::Result<RescueUsbDriveLetterResolution>
resolve_windows_rescue_usb_drive_letter_read_only(
    const diskmodel::DiskInfo& target);

}  // namespace ytec::windowsapp
