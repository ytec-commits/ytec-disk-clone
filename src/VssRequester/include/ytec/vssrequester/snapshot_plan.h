#pragma once

#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/clonecore/windows_volume_bitmap.h"
#include "ytec/vssrequester/snapshot_copy.h"
#include "ytec/vssrequester/workflow.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ytec::vssrequester {

struct SnapshotImagePlanOptions final {
  bool administrator{};
  std::uint32_t physical_sector_size{};
  std::uint32_t chunk_size{imageformat::kDcimgChunkSize16MiB};
  imageformat::DcimgCompression compression{
      imageformat::DcimgCompression::none};
  std::size_t verification_block_bytes{1024U * 1024U};
  std::vector<std::byte> manifest;
  std::vector<std::byte> partition_table_snapshot;
};

struct PreparedSnapshotImagePlan final {
  WorkflowRequest workflow;
  SnapshotImageCopyRequest image_copy;
  std::size_t raw_partition_count{};
  std::size_t snapshot_partition_count{};
  std::size_t recreated_partition_count{};
};

// Builds an online image plan without writing. Basic-data/0x07 NTFS
// partitions are mapped to VSS. EFI FAT32 and Recovery partitions use the
// already identity-verified read-only source. MSR is represented by the
// partition table and recreated without copying contents.
[[nodiscard]] clonecore::Result<PreparedSnapshotImagePlan>
prepare_gpt_snapshot_image_plan(
    const clonecore::GptDisk& source_gpt,
    const clonecore::ISourceDiskReader& read_only_source,
    std::span<const clonecore::VolumeBitmapBinding> ntfs_bindings,
    const SnapshotImagePlanOptions& options);

[[nodiscard]] clonecore::Result<PreparedSnapshotImagePlan>
prepare_mbr_snapshot_image_plan(
    const clonecore::MbrDisk& source_mbr,
    const clonecore::ISourceDiskReader& read_only_source,
    std::span<const clonecore::VolumeBitmapBinding> ntfs_bindings,
    const SnapshotImagePlanOptions& options);

}  // namespace ytec::vssrequester
