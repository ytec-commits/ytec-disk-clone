#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/imageformat/restore_image_inspection.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace ytec::windowsapp {

using RestoreImagePreflightReport =
    imageformat::RestoreImageInspectionReport;
using RestoreImagePreflightOptions =
    imageformat::RestoreImageInspectionOptions;

enum class RestoreTargetSelectionIssue : std::uint8_t {
  image_unavailable,
  inventory_loading,
  inventory_unavailable,
  inventory_has_issues,
  target_not_selected,
  target_is_original_source,
  target_is_system,
  target_state_unknown,
  target_is_read_only,
  target_is_removable,
  target_style_unknown,
  target_too_small,
  logical_sector_mismatch,
  unstable_identity,
  ready_for_confirmation,
};

struct RestoreTargetSelectionView final {
  RestoreTargetSelectionIssue issue{
      RestoreTargetSelectionIssue::image_unavailable};
  bool ready_for_confirmation{};
  bool restore_execution_enabled{};
  std::optional<clonecore::StableDiskIdentity> target_identity;
  std::wstring message;
};

[[nodiscard]] clonecore::Result<RestoreImagePreflightReport>
inspect_restore_image_reader(
    std::wstring canonical_path,
    std::uint64_t image_length,
    const imageformat::Sha256ReadCallback& reader,
    const RestoreImagePreflightOptions& options = {});

// Opens only a local drive-letter .dcimg regular file, rejects reparse points,
// and performs complete read-only verification. It never opens a target disk.
[[nodiscard]] clonecore::Result<RestoreImagePreflightReport>
inspect_restore_image_file(
    const std::wstring& requested_path,
    const RestoreImagePreflightOptions& options = {});

// Evaluates only information already obtained through read-only inventory.
// Passing this gate does not authorize opening or writing a target disk.
[[nodiscard]] RestoreTargetSelectionView
evaluate_restore_target_selection(
    const RestoreImagePreflightReport* image,
    const diskmodel::InventoryReport* inventory,
    std::optional<std::size_t> target_index,
    bool inventory_loading);

}  // namespace ytec::windowsapp
