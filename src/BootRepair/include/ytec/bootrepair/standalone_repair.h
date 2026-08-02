#pragma once

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"

#include <cstdint>
#include <memory>
#include <string>

namespace ytec::bootrepair {

struct BootRepairTargetRequest final {
  std::uint32_t disk_number{};
  std::wstring windows_root;
  std::wstring system_root;
  BcdBootFirmware firmware{BcdBootFirmware::uefi};
  BcdBootStorePolicy store_policy{
      BcdBootStorePolicy::preserve_existing};
  bool auto_mount_system_partition{};
};

struct BootRepairVolumeLocation final {
  std::uint32_t disk_number{};
  std::uint64_t starting_offset{};
  std::uint64_t extent_length{};
  std::wstring file_system;
};

struct BootRepairTargetSelection final {
  diskmodel::DiskInfo disk;
  clonecore::StableDiskIdentity identity;
  diskmodel::PartitionInfo windows_partition;
  diskmodel::PartitionInfo system_partition;
};

struct StandaloneBootRepairExecutionRequest final {
  BootRepairTargetRequest target;
  BootRepairTargetSelection expected;
  clonecore::TargetConfirmation confirmation;
};

struct StandaloneBootRepairReport final {
  BootRepairTargetSelection repaired;
  BcdBootReport bcdboot;
  bool boot_store_verified{};
  bool system_partition_temporarily_mounted{};
  bool temporary_mount_released{};
};

[[nodiscard]] clonecore::Result<BootRepairTargetSelection>
evaluate_boot_repair_target(
    const BootRepairTargetRequest& request,
    const diskmodel::InventoryReport& inventory,
    const BootRepairVolumeLocation& windows_volume,
    const BootRepairVolumeLocation& system_volume);

[[nodiscard]] std::wstring make_boot_repair_confirmation_token(
    const clonecore::StableDiskIdentity& identity,
    BcdBootFirmware firmware);

[[nodiscard]] clonecore::Status validate_boot_repair_selection(
    const BootRepairTargetSelection& expected,
    const BootRepairTargetSelection& observed,
    BcdBootFirmware firmware,
    const clonecore::TargetConfirmation& confirmation);

class IStandaloneBootRepairService {
 public:
  virtual ~IStandaloneBootRepairService() = default;

  [[nodiscard]] virtual clonecore::Result<BootRepairTargetSelection> inspect(
      const BootRepairTargetRequest& request) = 0;

  [[nodiscard]] virtual clonecore::Result<StandaloneBootRepairReport> execute(
      const StandaloneBootRepairExecutionRequest& request) = 0;
};

[[nodiscard]] std::unique_ptr<IStandaloneBootRepairService>
make_windows_standalone_boot_repair_service(
    diskmodel::IDiskInventoryProvider& inventory);

}  // namespace ytec::bootrepair
