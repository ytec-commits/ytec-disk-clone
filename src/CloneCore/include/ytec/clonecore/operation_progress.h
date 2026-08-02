#pragma once

#include <cstdint>
#include <functional>
#include <optional>

namespace ytec::clonecore {

enum class DiskOperationStage : std::uint8_t {
  planning,
  verifying_source,
  invalidating_target,
  copying_data,
  flushing_data,
  staging_partition_table,
  committing_partition_table,
  validating_conversion,
  converting_partition_style,
  rebuilding_boot,
  verifying_final,
  completed,
};

struct DiskOperationProgress final {
  DiskOperationStage stage{DiskOperationStage::planning};
  std::optional<std::uint32_t> partition_index;
  std::uint64_t total_read_bytes{};
  std::uint64_t total_write_bytes{};
  std::uint64_t total_verify_bytes{};
  std::uint64_t read_bytes{};
  std::uint64_t written_bytes{};
  std::uint64_t verified_bytes{};
  bool cancellation_allowed{};
};

struct DiskOperationCallbacks final {
  std::function<void(const DiskOperationProgress&)> progress;
  std::function<bool()> cancellation_requested;
};

inline void report_disk_operation_progress(
    const DiskOperationCallbacks& callbacks,
    const DiskOperationProgress& progress) noexcept {
  if (!callbacks.progress) {
    return;
  }
  try {
    callbacks.progress(progress);
  } catch (...) {
    // Progress observation must never unwind through a destructive engine.
  }
}

[[nodiscard]] inline bool disk_operation_cancellation_requested(
    const DiskOperationCallbacks& callbacks) noexcept {
  if (!callbacks.cancellation_requested) {
    return false;
  }
  try {
    return callbacks.cancellation_requested();
  } catch (...) {
    // A broken cancellation observer is treated as a fail-closed request.
    return true;
  }
}

}  // namespace ytec::clonecore
