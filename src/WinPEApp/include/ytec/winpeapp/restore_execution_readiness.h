#pragma once

#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/imageformat/restore_image_inspection.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ytec::winpeapp {

enum class RestoreSafetyState : std::uint8_t {
  unknown,
  passed,
  blocked,
};

enum class RestoreSafetyCheck : std::uint8_t {
  bitlocker_fully_decrypted,
  dynamic_disk_absent,
  storage_spaces_absent,
  file_system_layout_supported,
  stable_power,
  pending_restart_absent,
};

struct RestoreExecutionSafetyObservation final {
  RestoreSafetyState bitlocker_fully_decrypted{
      RestoreSafetyState::unknown};
  RestoreSafetyState dynamic_disk_absent{RestoreSafetyState::unknown};
  RestoreSafetyState storage_spaces_absent{RestoreSafetyState::unknown};
  RestoreSafetyState file_system_layout_supported{
      RestoreSafetyState::unknown};
  RestoreSafetyState stable_power{RestoreSafetyState::unknown};
  RestoreSafetyState pending_restart_absent{RestoreSafetyState::unknown};
};

struct RestoreSafetyFinding final {
  RestoreSafetyCheck check{
      RestoreSafetyCheck::bitlocker_fully_decrypted};
  RestoreSafetyState state{RestoreSafetyState::unknown};
  bool required{};
};

inline constexpr std::size_t kRestoreSafetyCheckCount = 6;

struct RestoreExecutionReadinessReport final {
  std::array<RestoreSafetyFinding, kRestoreSafetyCheckCount> checks{};
  bool required_checks_passed{};
  bool all_checks_passed{};
};

struct RestoreExecutionSafetyPolicy final {
  bool require_stable_power{true};
};

// Derives only facts available from a fully verified image, the fresh
// read-only inventory observation, and a separately obtained power state.
// Pending-restart remains advisory/unknown until an explicit probe supplies it.
[[nodiscard]] RestoreExecutionSafetyObservation
derive_restore_execution_safety_observation(
    const diskmodel::DiskInfo& target,
    const imageformat::RestoreImageInspectionReport& image,
    RestoreSafetyState power_state) noexcept;

// Pure fail-closed evaluation. This function performs no I/O and an unknown
// required observation is never treated as safe. Pending restart is an
// advisory warning per the top-level specification.
[[nodiscard]] RestoreExecutionReadinessReport
evaluate_restore_execution_readiness(
    const RestoreExecutionSafetyObservation& observation,
    const RestoreExecutionSafetyPolicy& policy = {}) noexcept;

[[nodiscard]] std::string_view restore_safety_state_name(
    RestoreSafetyState state) noexcept;

[[nodiscard]] std::string_view restore_safety_check_id(
    RestoreSafetyCheck check) noexcept;

[[nodiscard]] std::string_view restore_safety_check_label_ja(
    RestoreSafetyCheck check) noexcept;

}  // namespace ytec::winpeapp
