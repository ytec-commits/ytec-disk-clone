#pragma once

#include "ytec/diskmodel/physical_disk.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/migrationcore/direct_clone_plan.h"
#include "ytec/operationcore/operation.h"
#include "ytec/vssrequester/windows_backend.h"
#include "ytec/windowsshrink/source_analysis.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ytec::windowsapp {

inline constexpr std::uint64_t
    kWindowsDirectShrinkStagingAlignmentBytes = 1024ULL * 1024ULL;
inline constexpr std::uint64_t
    kWindowsDirectShrinkStagingControlReserveBytes = 64ULL * 1024ULL *
    1024ULL;
// The target-owned archive is a real NTFS volume because signed DISM accepts
// a file path, not a raw byte stream.  Keep a conservative additional margin
// for NTFS metadata and DISM scratch.  The production adapter also checks the
// formatted volume's exact free space before capture.
inline constexpr std::uint64_t
    kWindowsDirectShrinkStagingFileSystemReserveBytes = 1024ULL * 1024ULL *
    1024ULL;

// The reviewed source extent remains evidence for the physical source-volume
// binding. It is deliberately not treated as a promised WIM size: neither
// used-byte counters nor compression ratios can prove the capture result.
struct WindowsDirectShrinkNtfsVolume final {
  std::uint32_t source_table_index{};
  std::uint64_t source_offset_bytes{};
  std::uint64_t source_size_bytes{};
  std::wstring original_volume_guid_path;
};

struct WindowsDirectShrinkPlanningRequest final {
  bool administrator{};
  bool bitlocker_fully_decrypted{};
  bool target_is_active_rescue_media{};
  diskmodel::DiskInfo reviewed_source;
  diskmodel::DiskInfo reviewed_target;
  clonecore::StableDiskIdentity expected_source;
  clonecore::StableDiskIdentity expected_target;
  imageformat::Sha256Digest expected_source_layout_hash{};
  imageformat::Sha256Digest expected_target_layout_hash{};
  operationcore::OperationId operation_id{};
  std::vector<WindowsDirectShrinkNtfsVolume> ntfs_volumes;
};

// Read-only product planning input used by the Windows UI before either
// destructive confirmation or VSS creation. The implementation reopens the
// reviewed source read-only, parses its exact GPT, binds every NTFS extent to
// one Volume GUID, and then builds the immutable direct-shrink plan.
//
// MBR is deliberately not routed through this request. MBR-to-GPT remains a
// separately reviewed WinPE workflow; silently treating it as preserve-style
// Windows direct shrink would bypass that conversion contract.
struct WindowsDirectShrinkProductPlanningRequest final {
  bool administrator{};
  bool target_is_active_rescue_media{};
  diskmodel::DiskInfo reviewed_source;
  diskmodel::DiskInfo reviewed_target;
  operationcore::OperationId operation_id{};
  migrationcore::ShrinkSurplusAllocation surplus_allocation{
      migrationcore::ShrinkSurplusAllocation::automatic_proportional};
  std::uint32_t windows_major{};
  std::uint32_t windows_minor{};
  std::uint32_t windows_build{};
  std::string windows_architecture;
  std::string analysis_created_utc;
  std::string app_version;
};

enum class WindowsDirectShrinkPartitionTaskKind : std::uint8_t {
  recreate_efi_system,
  recreate_microsoft_reserved,
  create_empty_ntfs,
  apply_ntfs_wim,
};

struct WindowsDirectShrinkPartitionTask final {
  WindowsDirectShrinkPartitionTaskKind kind{
      WindowsDirectShrinkPartitionTaskKind::apply_ntfs_wim};
  std::uint32_t target_number{};
  std::optional<std::uint32_t> source_table_index;
  migrationcore::MigrationPartitionRole role{
      migrationcore::MigrationPartitionRole::data};
  bool active{};
  // Reviewed physical source extent. Zero for generated partitions.
  std::uint64_t source_offset_bytes{};
  std::uint64_t target_offset_bytes{};
  // The temporary GPT exposes the final offset but only this reviewed minimum
  // length.  Automatic surplus is published later and the NTFS filesystem is
  // extended only after the target-owned WIM has been removed.
  std::uint64_t construction_size_bytes{};
  std::uint64_t target_size_bytes{};
  std::uint64_t source_size_bytes{};
  std::uint64_t source_used_bytes{};
  // apply_ntfs_wim only. Empty for generated/empty partitions.
  std::wstring original_volume_guid_path;
  // apply_ntfs_wim only. This is the exact target-owned archive capacity,
  // never a compression estimate or a promised successful WIM size. DISM
  // capacity exhaustion fails the operation and leaves invalid/offline target
  // metadata without publishing the final layout.
  std::uint64_t archive_upper_bound_bytes{};
};

struct WindowsDirectShrinkTargetOwnedStagingPlan final {
  std::uint64_t offset_bytes{};
  std::uint64_t length_bytes{};
  std::uint64_t control_reserve_bytes{};
  std::uint64_t archive_offset_bytes{};
  std::uint64_t archive_capacity_bytes{};
  // Set only when automatic surplus uses an NTFS growth extent for staging.
  // The exact extent is reclaimed by this partition after the archive and
  // checkpoint have been durably retired.
  std::optional<std::uint32_t> final_growth_owner_target_number;
};

// Immutable, normalized two-disk execution plan. Construction is restricted
// to build_windows_direct_shrink_clone_plan(), which performs no environment
// or disk I/O.
class WindowsDirectShrinkClonePlan final {
 public:
  WindowsDirectShrinkClonePlan(const WindowsDirectShrinkClonePlan&) = default;
  WindowsDirectShrinkClonePlan(WindowsDirectShrinkClonePlan&&) noexcept =
      default;
  WindowsDirectShrinkClonePlan& operator=(
      const WindowsDirectShrinkClonePlan&) = delete;
  WindowsDirectShrinkClonePlan& operator=(
      WindowsDirectShrinkClonePlan&&) = delete;

  [[nodiscard]] const operationcore::OperationPlan& operation_plan()
      const noexcept {
    return operation_plan_;
  }
  [[nodiscard]] const clonecore::StableDiskIdentity& expected_source()
      const noexcept {
    return expected_source_;
  }
  [[nodiscard]] const clonecore::StableDiskIdentity& expected_target()
      const noexcept {
    return expected_target_;
  }
  [[nodiscard]] const imageformat::Sha256Digest&
  expected_source_layout_hash() const noexcept {
    return expected_source_layout_hash_;
  }
  [[nodiscard]] const imageformat::Sha256Digest&
  expected_target_layout_hash() const noexcept {
    return expected_target_layout_hash_;
  }
  [[nodiscard]] migrationcore::MigrationPartitionStyle partition_style()
      const noexcept {
    return partition_style_;
  }
  [[nodiscard]] migrationcore::ShrinkSurplusAllocation surplus_allocation()
      const noexcept {
    return surplus_allocation_;
  }
  [[nodiscard]] const WindowsDirectShrinkTargetOwnedStagingPlan& staging()
      const noexcept {
    return staging_;
  }
  [[nodiscard]] std::span<const WindowsDirectShrinkPartitionTask> tasks()
      const noexcept {
    return tasks_;
  }
  [[nodiscard]] const vssrequester::WorkflowRequest& workflow() const noexcept {
    return workflow_;
  }
  [[nodiscard]] const imageformat::Sha256Digest& final_layout_hash()
      const noexcept {
    return final_layout_hash_;
  }
  [[nodiscard]] bool boot_finalization_required() const noexcept {
    return boot_finalization_required_;
  }
  [[nodiscard]] bool target_is_active_rescue_media() const noexcept {
    return target_is_active_rescue_media_;
  }
  [[nodiscard]] std::uint64_t archive_task_count() const noexcept {
    return archive_task_count_;
  }
  [[nodiscard]] std::uint64_t maximum_archive_upper_bound_bytes()
      const noexcept {
    return maximum_archive_upper_bound_bytes_;
  }
  [[nodiscard]] std::uint64_t ntfs_extension_task_count() const noexcept {
    return ntfs_extension_task_count_;
  }

 private:
  WindowsDirectShrinkClonePlan() = default;

  operationcore::OperationPlan operation_plan_;
  clonecore::StableDiskIdentity expected_source_;
  clonecore::StableDiskIdentity expected_target_;
  imageformat::Sha256Digest expected_source_layout_hash_{};
  imageformat::Sha256Digest expected_target_layout_hash_{};
  migrationcore::MigrationPartitionStyle partition_style_{
      migrationcore::MigrationPartitionStyle::gpt};
  migrationcore::ShrinkSurplusAllocation surplus_allocation_{
      migrationcore::ShrinkSurplusAllocation::leave_unallocated};
  WindowsDirectShrinkTargetOwnedStagingPlan staging_;
  std::vector<WindowsDirectShrinkPartitionTask> tasks_;
  vssrequester::WorkflowRequest workflow_;
  imageformat::Sha256Digest final_layout_hash_{};
  bool boot_finalization_required_{};
  bool target_is_active_rescue_media_{};
  std::uint64_t archive_task_count_{};
  std::uint64_t maximum_archive_upper_bound_bytes_{};
  std::uint64_t ntfs_extension_task_count_{};

  friend clonecore::Result<WindowsDirectShrinkClonePlan>
  build_windows_direct_shrink_clone_plan(
      const WindowsDirectShrinkPlanningRequest&,
      const migrationcore::DirectClonePlan&);
};

// Pure planner. v2 accepts preserve-style 512-byte-sector NTFS shrink plans.
// Staging is either in the reviewed unallocated tail or in one automatic NTFS
// growth extent. In both cases it remains disjoint from every construction
// partition and bounds one WIM at a time. If signed DISM cannot fit that WIM,
// execution safely aborts; planner acceptance never predicts compression.
[[nodiscard]] clonecore::Result<WindowsDirectShrinkClonePlan>
build_windows_direct_shrink_clone_plan(
    const WindowsDirectShrinkPlanningRequest& request,
    const migrationcore::DirectClonePlan& direct_plan);

// Pure conversion seam used by unit tests and by the Windows API adapter after
// its read-only source analysis. It performs no environment or disk I/O.
[[nodiscard]] clonecore::Result<WindowsDirectShrinkClonePlan>
build_windows_direct_shrink_clone_plan_from_analysis(
    const WindowsDirectShrinkProductPlanningRequest& request,
    const windowsshrink::ShrinkSourceAnalysis& analysis);

// Product read-only preflight. This function does not request elevation, open
// a writable target, create VSS snapshots, or mutate either disk. A successful
// result is the exact immutable plan that must be shown in the two-stage UI
// confirmation and later passed to execute_windows_direct_shrink_clone().
[[nodiscard]] clonecore::Result<WindowsDirectShrinkClonePlan>
plan_windows_direct_shrink_clone_with_windows_apis(
    const WindowsDirectShrinkProductPlanningRequest& request);

enum class WindowsDirectShrinkCheckpointPhase : std::uint8_t {
  prepared,
  applying,
  commit_ready,
};

struct WindowsDirectShrinkCheckpointEvidence final {
  WindowsDirectShrinkCheckpointPhase phase{
      WindowsDirectShrinkCheckpointPhase::prepared};
  std::uint64_t revision{};
  operationcore::Sha256Digest plan_hash{};
  imageformat::Sha256Digest staging_identity_hash{};
  imageformat::Sha256Digest record_hash{};
  imageformat::Sha256Digest aggregate_write_digest{};
  clonecore::StableDiskIdentity observed_target;
  std::uint64_t completed_task_count{};
  std::uint64_t verified_target_bytes{};
  bool durable{};
  bool flushed{};
  bool read_back_verified{};
  bool target_offline{};
  bool final_layout_committed{};
};

struct WindowsDirectShrinkTargetPreparationEvidence final {
  std::uint64_t prepared_task_count{};
  std::uint64_t verified_target_bytes{};
  imageformat::Sha256Digest write_digest{};
  bool every_write_flushed{};
  bool every_write_read_back{};
  bool target_offline{};
  bool final_layout_committed{};
};

struct WindowsDirectShrinkStagedArchiveEvidence final {
  std::uint32_t source_table_index{};
  std::uint32_t target_number{};
  std::wstring snapshot_id;
  std::wstring snapshot_device_path;
  std::uint64_t archive_length{};
  imageformat::Sha256Digest archive_hash{};
  bool sealed_no_write_delete_sharing{};
  bool flushed{};
  bool complete_read_back_hash_verified{};
  bool target_offline{};
};

struct WindowsDirectShrinkAppliedPartitionEvidence final {
  std::uint32_t source_table_index{};
  std::uint32_t target_number{};
  std::uint64_t verified_target_bytes{};
  imageformat::Sha256Digest archive_hash{};
  imageformat::Sha256Digest target_write_digest{};
  bool every_write_flushed{};
  bool every_write_read_back{};
  bool file_system_metadata_verified{};
  bool target_offline{};
};

struct WindowsDirectShrinkBootEvidence final {
  bool required{};
  bool completed{};
  bool boot_files_read_back_verified{};
  bool recovery_configuration_verified{};
  bool target_offline{};
};

struct WindowsDirectShrinkFinalCommitEvidence final {
  imageformat::Sha256Digest committed_layout_hash{};
  imageformat::Sha256Digest aggregate_write_digest{};
  bool target_reidentified{};
  bool staging_identity_reverified{};
  bool checkpoint_reverified{};
  bool staging_removed{};
  bool checkpoint_retired{};
  bool hidden_final_layout_published_and_read_back{};
  std::uint64_t extended_ntfs_partition_count{};
  bool every_required_ntfs_extension_verified{};
  bool every_write_flushed{};
  bool every_write_read_back{};
  bool primary_layout_committed_last{};
  bool target_offline{};
};

// Destructive platform seam. Implementations own the target Writer. No method
// receives a writable source object. begin_target_owned_staging() must first
// invalidate the target completion metadata and leave it offline. abort() is
// idempotent and must leave the target offline and visibly incomplete.
class IWindowsDirectShrinkClonePlatform {
 public:
  virtual ~IWindowsDirectShrinkClonePlatform() = default;

  [[nodiscard]] virtual clonecore::Result<
      WindowsDirectShrinkCheckpointEvidence>
  begin_target_owned_staging(
      const WindowsDirectShrinkClonePlan& plan,
      const operationcore::Sha256Digest& operation_plan_hash) = 0;

  [[nodiscard]] virtual clonecore::Result<
      WindowsDirectShrinkTargetPreparationEvidence>
  prepare_non_archive_partitions_and_verify(
      std::span<const WindowsDirectShrinkPartitionTask> tasks) = 0;

  [[nodiscard]] virtual clonecore::Result<
      WindowsDirectShrinkStagedArchiveEvidence>
  capture_ntfs_wim_to_owned_staging(
      const WindowsDirectShrinkPartitionTask& task,
      const vssrequester::SnapshotMapping& snapshot) = 0;

  [[nodiscard]] virtual clonecore::Result<
      WindowsDirectShrinkAppliedPartitionEvidence>
  apply_staged_ntfs_wim_and_verify(
      const WindowsDirectShrinkPartitionTask& task,
      const WindowsDirectShrinkStagedArchiveEvidence& archive) = 0;

  [[nodiscard]] virtual clonecore::Status discard_exact_staged_archive(
      const WindowsDirectShrinkStagedArchiveEvidence& archive) = 0;

  // Non-archive preparation completes before the first WIM task. It must be
  // durably represented before an archive/application can advance progress.
  [[nodiscard]] virtual clonecore::Result<
      WindowsDirectShrinkCheckpointEvidence>
  persist_prepared_partitions_checkpoint(
      const WindowsDirectShrinkCheckpointEvidence& previous,
      std::uint64_t completed_task_count,
      std::uint64_t verified_target_bytes,
      const imageformat::Sha256Digest& aggregate_write_digest) = 0;

  [[nodiscard]] virtual clonecore::Result<
      WindowsDirectShrinkCheckpointEvidence>
  persist_progress_checkpoint(
      const WindowsDirectShrinkCheckpointEvidence& previous,
      std::uint64_t completed_task_count,
      std::uint64_t verified_target_bytes,
      const imageformat::Sha256Digest& aggregate_write_digest) = 0;

  [[nodiscard]] virtual clonecore::Result<WindowsDirectShrinkBootEvidence>
  finalize_boot_from_staged_layout_and_verify(
      const WindowsDirectShrinkClonePlan& plan) = 0;

  [[nodiscard]] virtual clonecore::Result<
      WindowsDirectShrinkCheckpointEvidence>
  seal_commit_ready_checkpoint(
      const WindowsDirectShrinkCheckpointEvidence& previous,
      std::uint64_t completed_task_count,
      std::uint64_t verified_target_bytes,
      const imageformat::Sha256Digest& aggregate_write_digest) = 0;

  // Called only after BackupComplete and Snapshot deletion have succeeded.
  // Must freshly prove the stable target connection, staging identity, and
  // exact durable commit-ready checkpoint without changing target bytes.
  [[nodiscard]] virtual clonecore::Result<
      WindowsDirectShrinkCheckpointEvidence>
  revalidate_before_final_commit(
      const WindowsDirectShrinkClonePlan& plan,
      const WindowsDirectShrinkCheckpointEvidence& expected) = 0;

  [[nodiscard]] virtual clonecore::Result<
      WindowsDirectShrinkFinalCommitEvidence>
  commit_final_layout_last(
      const WindowsDirectShrinkClonePlan& plan,
      const WindowsDirectShrinkCheckpointEvidence& commit_ready) = 0;

  virtual void abort_keep_offline_incomplete() noexcept = 0;
};

using WindowsDirectShrinkSelectionReidentifier = std::function<
    clonecore::Result<diskmodel::ReidentifiedPhysicalClone>(
        const clonecore::StableDiskIdentity&,
        const clonecore::StableDiskIdentity&)>;

using WindowsDirectShrinkConfirmedReidentifier = std::function<
    clonecore::Result<diskmodel::ReidentifiedPhysicalClone>(
        const clonecore::StableDiskIdentity&,
        const clonecore::StableDiskIdentity&,
        const clonecore::TargetConfirmation&)>;

using WindowsDirectShrinkSnapshotWorkflowRunner = std::function<
    clonecore::Result<vssrequester::WorkflowReport>(
        const vssrequester::WorkflowRequest&,
        const vssrequester::AsyncWaitOptions&,
        const clonecore::Logger*,
        vssrequester::SnapshotCopyCallback)>;

// Construction is a read-only preflight and must perform no environment or
// disk I/O. The executor invokes it before creating the VSS Snapshot Set, then
// reidentifies again inside the snapshot callback before the platform's first
// destructive method.
using WindowsDirectShrinkPlatformFactory = std::function<clonecore::Result<
    std::unique_ptr<IWindowsDirectShrinkClonePlatform>>(
    const WindowsDirectShrinkClonePlan&,
    const diskmodel::ReidentifiedPhysicalClone&)>;

struct WindowsDirectShrinkCloneDependencies final {
  WindowsDirectShrinkSelectionReidentifier reidentify_selection;
  WindowsDirectShrinkConfirmedReidentifier reidentify_confirmed;
  WindowsDirectShrinkSnapshotWorkflowRunner run_snapshot_workflow;
  WindowsDirectShrinkPlatformFactory make_platform;
};

struct WindowsDirectShrinkCloneExecutionOptions final {
  vssrequester::AsyncWaitOptions async_wait;
  clonecore::TargetConfirmation confirmation;
  clonecore::DiskOperationCallbacks callbacks;
  const clonecore::Logger* logger{};
};

struct WindowsDirectShrinkCloneExecutionReport final {
  vssrequester::WorkflowReport workflow;
  std::uint64_t applied_archive_count{};
  std::uint64_t verified_target_bytes{};
  imageformat::Sha256Digest aggregate_write_digest{};
  WindowsDirectShrinkCheckpointEvidence commit_ready_checkpoint;
  WindowsDirectShrinkBootEvidence boot;
  WindowsDirectShrinkFinalCommitEvidence final_commit;
  bool every_payload_captured_and_applied_inside_snapshot_callback{};
  bool snapshots_deleted_before_final_layout_commit{};
  bool target_left_offline{};
};

struct WindowsDirectShrinkCloneOperationReport final {
  operationcore::OperationPlan plan;
  operationcore::OperationResult lifecycle;
  std::optional<WindowsDirectShrinkCloneExecutionReport> execution;
};

// Runs the common OperationPlan/OK lifecycle and the two-disk shrink state
// machine. A successful Result means the lifecycle ran; inspect outcome.
[[nodiscard]] clonecore::Result<WindowsDirectShrinkCloneOperationReport>
execute_windows_direct_shrink_clone(
    const WindowsDirectShrinkClonePlan& plan,
    const WindowsDirectShrinkCloneExecutionOptions& options,
    const WindowsDirectShrinkCloneDependencies& dependencies);

// Product adapter construction is read-only and requests neither UAC nor disk
// access. The returned callbacks use the Windows inventory/VSS/platform
// implementations only when execute_windows_direct_shrink_clone() reaches the
// corresponding safety boundary.
[[nodiscard]] WindowsDirectShrinkCloneDependencies
make_windows_direct_shrink_clone_dependencies(
    const WindowsDirectShrinkCloneExecutionOptions& options);

// Product convenience entry point. It preserves the same OperationPlan and
// confirmation lifecycle while binding the audited Windows implementations.
[[nodiscard]] clonecore::Result<WindowsDirectShrinkCloneOperationReport>
execute_windows_direct_shrink_clone_with_windows_apis(
    const WindowsDirectShrinkClonePlan& plan,
    const WindowsDirectShrinkCloneExecutionOptions& options);

}  // namespace ytec::windowsapp
