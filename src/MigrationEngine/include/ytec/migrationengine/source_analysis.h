#pragma once

#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/imageformat/shrink_image_manifest.h"

#include <optional>
#include <string>
#include <vector>

namespace ytec::migrationengine {

struct WindowsSourceVersion final {
  std::uint32_t major{};
  std::uint32_t minor{};
  std::uint32_t build{};
  std::string architecture;
};

struct ShrinkSourceAnalysisContext final {
  clonecore::StableDiskIdentity source_identity;
  std::uint32_t physical_sector_size{};
  std::string created_utc;
  std::string app_version;
  std::optional<WindowsSourceVersion> known_windows_version;
};

struct AnalyzedShrinkVolume final {
  std::uint32_t source_table_index{};
  std::wstring volume_guid_path;
  std::string payload_file_name;
};

struct ShrinkSourceAnalysis final {
  imageformat::ShrinkImageManifest manifest;
  std::vector<AnalyzedShrinkVolume> content_volumes;
};

// These functions are read-only. They bind exact partition offsets to
// single-disk Volume GUID paths, require basic NTFS content, calculate used
// bytes from the volume, and reject BitLocker or ambiguous Windows layouts.
[[nodiscard]] clonecore::Result<ShrinkSourceAnalysis>
analyze_gpt_shrink_source_with_windows_apis(
    const diskmodel::DiskInfo& source_disk,
    const clonecore::ISourceDiskReader& read_only_source,
    const clonecore::GptDisk& layout,
    const ShrinkSourceAnalysisContext& context);

[[nodiscard]] clonecore::Result<ShrinkSourceAnalysis>
analyze_mbr_shrink_source_with_windows_apis(
    const diskmodel::DiskInfo& source_disk,
    const clonecore::ISourceDiskReader& read_only_source,
    const clonecore::MbrDisk& layout,
    const ShrinkSourceAnalysisContext& context);

}  // namespace ytec::migrationengine
