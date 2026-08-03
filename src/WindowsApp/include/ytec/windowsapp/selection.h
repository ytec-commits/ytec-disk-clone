#pragma once

#include "ytec/diskmodel/disk_inventory.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace ytec::windowsapp {

enum class CloneSelectionIssue : std::uint8_t {
  loading,
  inventory_unavailable,
  disk_not_selected,
  same_disk,
  target_is_system,
  target_is_read_only,
  target_state_unknown,
  target_layout_unsupported,
  target_too_small,
  ready,
};

struct CloneSelectionView final {
  CloneSelectionIssue issue{CloneSelectionIssue::inventory_unavailable};
  bool ready{};
  bool target_requires_initialization{};
  std::wstring message;
};

[[nodiscard]] CloneSelectionView evaluate_clone_selection(
    const diskmodel::InventoryReport* inventory,
    std::optional<std::size_t> source_index,
    std::optional<std::size_t> target_index,
    bool inventory_loading,
    bool require_target_same_or_larger = true);

}  // namespace ytec::windowsapp
