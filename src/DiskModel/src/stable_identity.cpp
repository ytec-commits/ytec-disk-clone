#include "ytec/diskmodel/disk_inventory.h"

#include <Windows.h>

namespace ytec::diskmodel {

clonecore::Result<clonecore::StableDiskIdentity> make_stable_disk_identity(
    const DiskInfo& disk,
    const bool is_system_disk) {
  if (disk.model.empty() || disk.model == L"未取得" || disk.size_bytes == 0 ||
      disk.logical_sector_size == 0 ||
      (disk.serial_suffix.empty() && disk.device_instance_id.empty())) {
    return clonecore::Result<clonecore::StableDiskIdentity>::failure(
        clonecore::Error{
            .code = clonecore::ErrorCode::invalid_data,
            .native_code = ERROR_INVALID_DATA,
            .operation = L"安定ディスク識別情報の作成",
            .message =
                L"モデル、容量、セクターサイズ、シリアル末尾またはデバイス識別子が不足しています",
        });
  }
  return clonecore::Result<clonecore::StableDiskIdentity>::success(
      clonecore::StableDiskIdentity{
          .disk_number = disk.disk_number,
          .model = disk.model,
          .size_bytes = disk.size_bytes,
          .logical_sector_size = disk.logical_sector_size,
          .serial_suffix = disk.serial_suffix,
          .device_instance_id = disk.device_instance_id,
          .is_system_disk = is_system_disk,
      });
}

}  // namespace ytec::diskmodel
