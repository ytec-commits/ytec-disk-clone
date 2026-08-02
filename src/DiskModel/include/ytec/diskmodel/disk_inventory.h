#pragma once

#include "ytec/clonecore/log.h"
#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ytec::diskmodel {

enum class PartitionStyle : std::uint8_t {
  raw,
  mbr,
  gpt,
  unknown,
};

struct PartitionInfo final {
  std::uint32_t number{};
  std::uint64_t offset_bytes{};
  std::uint64_t size_bytes{};
  PartitionStyle style{PartitionStyle::unknown};
  std::wstring type;
  std::wstring identifier;
  std::wstring name;
  bool bootable{};
};

struct DiskInfo final {
  std::uint32_t disk_number{};
  std::wstring device_path;
  std::wstring device_instance_id;
  std::wstring model;
  std::uint64_t size_bytes{};
  std::uint64_t sector_count{};
  std::uint32_t logical_sector_size{};
  std::uint32_t physical_sector_size{};
  std::wstring bus_type;
  std::string serial_suffix;
  PartitionStyle partition_style{PartitionStyle::unknown};
  std::optional<bool> offline;
  std::optional<bool> read_only;
  std::optional<bool> removable;
  bool is_system_disk{};
  std::vector<PartitionInfo> partitions;
};

struct InventoryIssue final {
  std::wstring device;
  clonecore::Error error;
};

struct InventoryReport final {
  std::vector<DiskInfo> disks;
  std::vector<InventoryIssue> issues;
};

class IDiskInventoryProvider {
 public:
  virtual ~IDiskInventoryProvider() = default;

  [[nodiscard]] virtual clonecore::Result<InventoryReport> enumerate() = 0;
};

[[nodiscard]] std::unique_ptr<IDiskInventoryProvider>
make_windows_disk_inventory_provider(const clonecore::Logger* logger = nullptr);

[[nodiscard]] std::wstring_view partition_style_name(
    PartitionStyle style) noexcept;

[[nodiscard]] PartitionStyle normalize_disk_partition_style(
    PartitionStyle reported_style,
    std::size_t partition_count) noexcept;

[[nodiscard]] clonecore::Result<clonecore::StableDiskIdentity>
make_stable_disk_identity(const DiskInfo& disk, bool is_system_disk);

}  // namespace ytec::diskmodel
