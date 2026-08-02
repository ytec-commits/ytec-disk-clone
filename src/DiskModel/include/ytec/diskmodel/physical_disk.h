#pragma once

#include "ytec/clonecore/block_device.h"
#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/clonecore/windows_volume_bitmap.h"
#include "ytec/diskmodel/disk_inventory.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace ytec::diskmodel {

struct ReidentifiedPhysicalClone final {
  DiskInfo source;
  DiskInfo target;
  clonecore::StableDiskIdentity source_identity;
  clonecore::StableDiskIdentity target_identity;
};

struct PhysicalCloneHandles final {
  ReidentifiedPhysicalClone observed;
  std::unique_ptr<clonecore::ISourceDiskReader> source;
  std::unique_ptr<clonecore::ITargetDiskWriter> target;
};

struct ReidentifiedPhysicalTarget final {
  DiskInfo target;
  clonecore::StableDiskIdentity target_identity;
};

struct PhysicalTargetHandle final {
  ReidentifiedPhysicalTarget observed;
  std::unique_ptr<clonecore::ITargetDiskWriter> target;
};

struct ReidentifiedReadOnlyDisk final {
  DiskInfo observed;
  clonecore::StableDiskIdentity identity;
};

struct ReadOnlyPhysicalDiskHandle final {
  ReidentifiedReadOnlyDisk observed;
  std::unique_ptr<clonecore::ISourceDiskReader> reader;
};

// The concrete implementation of this backend is intentionally not exposed.
// Destructive access must go through the verified functions below.
class IWindowsPhysicalDiskBackend {
 public:
  virtual ~IWindowsPhysicalDiskBackend() = default;

  [[nodiscard]] virtual clonecore::Result<
      std::unique_ptr<clonecore::ISourceDiskReader>>
  open_source(const DiskInfo& disk) = 0;

  [[nodiscard]] virtual clonecore::Result<
      std::unique_ptr<clonecore::ITargetDiskWriter>>
  open_offline_target(const DiskInfo& disk) = 0;

  [[nodiscard]] virtual clonecore::Status set_target_offline(
      const DiskInfo& disk,
      bool offline) = 0;
};

[[nodiscard]] clonecore::Result<
    std::vector<clonecore::VolumeBitmapBinding>>
query_windows_volume_bitmap_bindings(
    const DiskInfo& source_disk,
    const clonecore::GptDisk& source_gpt);

[[nodiscard]] clonecore::Result<
    std::vector<clonecore::VolumeBitmapBinding>>
query_windows_volume_bitmap_bindings(
    const DiskInfo& source_disk,
    const clonecore::MbrDisk& source_mbr);

[[nodiscard]] clonecore::Result<ReidentifiedReadOnlyDisk>
reidentify_read_only_physical_disk(
    const clonecore::StableDiskIdentity& expected,
    IDiskInventoryProvider& inventory);

[[nodiscard]] clonecore::Result<ReadOnlyPhysicalDiskHandle>
open_verified_read_only_physical_disk(
    const clonecore::StableDiskIdentity& expected,
    IDiskInventoryProvider& inventory,
    IWindowsPhysicalDiskBackend& backend);

[[nodiscard]] clonecore::Result<ReadOnlyPhysicalDiskHandle>
open_verified_read_only_physical_disk_with_windows_apis(
    const clonecore::StableDiskIdentity& expected);

[[nodiscard]] clonecore::Result<ReidentifiedPhysicalClone>
reidentify_physical_clone(
    const clonecore::StableDiskIdentity& expected_source,
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    IDiskInventoryProvider& inventory);

[[nodiscard]] clonecore::Status set_verified_target_offline(
    const clonecore::StableDiskIdentity& expected_source,
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    bool offline,
    IDiskInventoryProvider& inventory,
    IWindowsPhysicalDiskBackend& backend);

[[nodiscard]] clonecore::Result<PhysicalCloneHandles>
open_verified_physical_clone(
    const clonecore::StableDiskIdentity& expected_source,
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    IDiskInventoryProvider& inventory,
    IWindowsPhysicalDiskBackend& backend);

// Target-only destructive boundary used by verified image restore. Every
// entry point performs a fresh full inventory scan, stable-identity check,
// system/removable/read-only/sector gate, and exact two-step confirmation.
[[nodiscard]] clonecore::Result<ReidentifiedPhysicalTarget>
reidentify_physical_target(
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    IDiskInventoryProvider& inventory);

[[nodiscard]] clonecore::Status
set_verified_physical_target_offline(
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    bool offline,
    IDiskInventoryProvider& inventory,
    IWindowsPhysicalDiskBackend& backend);

[[nodiscard]] clonecore::Result<PhysicalTargetHandle>
open_verified_physical_target(
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    IDiskInventoryProvider& inventory,
    IWindowsPhysicalDiskBackend& backend);

[[nodiscard]] clonecore::Status set_verified_target_offline_with_windows_apis(
    const clonecore::StableDiskIdentity& expected_source,
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    bool offline);

[[nodiscard]] clonecore::Result<PhysicalCloneHandles>
open_verified_physical_clone_with_windows_apis(
    const clonecore::StableDiskIdentity& expected_source,
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation);

[[nodiscard]] clonecore::Status
set_verified_physical_target_offline_with_windows_apis(
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    bool offline);

[[nodiscard]] clonecore::Result<PhysicalTargetHandle>
open_verified_physical_target_with_windows_apis(
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation);

}  // namespace ytec::diskmodel
