#pragma once

#include "ytec/bootrepair/standalone_repair.h"
#include "ytec/bootrepair/mbr2gpt.h"
#include "ytec/bootrepair/winre_diagnostic.h"
#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/operation_progress.h"
#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/imageformat/job_manifest.h"
#include "ytec/imageformat/restore_image_inspection.h"
#include "ytec/winpeapp/restore_execution_readiness.h"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ytec::winpeapp {

enum class ClonePartitionStyle : std::uint8_t {
  gpt,
  mbr,
};

struct CloneExecutionRequest final {
  clonecore::StableDiskIdentity expected_source;
  clonecore::StableDiskIdentity expected_target;
  clonecore::TargetConfirmation confirmation;
  std::wstring authorization;
  clonecore::DiskOperationCallbacks callbacks;
};

struct CloneExecutionReport final {
  ClonePartitionStyle partition_style{ClonePartitionStyle::gpt};
  std::uint64_t copied_data_bytes{};
  std::uint32_t copied_partition_count{};
  std::uint32_t recreated_partition_count{};
  bool read_back_verified{};
  bool partition_table_committed{};
  bool target_returned_online{};
  bootrepair::StandaloneBootRepairReport boot_repair;
  bool windows_partition_temporarily_mounted{};
  bool system_partition_temporarily_mounted{};
  bool temporary_mounts_released{};
  bool boot_finalization_verified{};
};

class ICloneExecutionService {
 public:
  virtual ~ICloneExecutionService() = default;

  [[nodiscard]] virtual clonecore::Result<CloneExecutionReport> execute(
      const CloneExecutionRequest& request) = 0;
};

// Product WinPE clone implementation. Direct --clone-execute remains reserved
// for the separately injected VM harness; this service is connected only to a
// verified --job-execute clone job. It holds the source read-only handle,
// writes only through the verified target boundary, and leaves a failed
// partial target offline. Calling execute performs destructive raw-disk writes.
[[nodiscard]] std::unique_ptr<ICloneExecutionService>
make_windows_clone_job_execution_service();

struct Mbr2GptJobExecutionRequest final {
  clonecore::StableDiskIdentity expected_source;
  clonecore::StableDiskIdentity expected_target;
  clonecore::TargetConfirmation confirmation;
  clonecore::DiskOperationCallbacks callbacks;
};

struct Mbr2GptJobExecutionReport final {
  CloneExecutionReport clone;
  bootrepair::Mbr2GptConversionReport conversion;
  bootrepair::StandaloneBootRepairReport boot_repair;
  bool source_reidentified_unchanged{};
  bool target_reidentified_as_gpt{};
  bool efi_system_partition_verified{};
  bool microsoft_reserved_partition_verified{};
  bool offline_windows_verified{};
  bool windows_partition_temporarily_mounted{};
  bool temporary_windows_mount_released{};
  bool final_layout_verified{};
};

class IMbr2GptJobExecutionService {
 public:
  virtual ~IMbr2GptJobExecutionService() = default;

  [[nodiscard]] virtual clonecore::Result<Mbr2GptJobExecutionReport> execute(
      const Mbr2GptJobExecutionRequest& request) = 0;
};

// Product WinPE migration implementation. It first performs the verified MBR
// clone to the separate target, then invokes only the Microsoft-signed
// System32 MBR2GPT and BCDBoot boundaries. The source remains read-only.
[[nodiscard]] std::unique_ptr<IMbr2GptJobExecutionService>
make_windows_mbr2gpt_job_execution_service();

struct RestoreExecutionRequest final {
  clonecore::StableDiskIdentity expected_target;
  imageformat::RestoreImageIdentity expected_image;
  std::wstring verified_image_path;
  clonecore::TargetConfirmation confirmation;
  clonecore::DiskOperationCallbacks callbacks;
};

struct RestoreExecutionReport final {
  std::uint64_t restored_data_bytes{};
  std::uint64_t committed_partition_table_bytes{};
  std::uint32_t restored_chunk_count{};
  bool complete_image_verified_before_write{};
  bool backup_manifest_verified_before_write{};
  bool read_back_verified{};
  bool partition_table_committed{};
  bool target_returned_online{};
  bootrepair::StandaloneBootRepairReport boot_repair;
  bool windows_partition_temporarily_mounted{};
  bool system_partition_temporarily_mounted{};
  bool temporary_mounts_released{};
  bool boot_finalization_verified{};
};

class IRestoreExecutionService {
 public:
  virtual ~IRestoreExecutionService() = default;

  // The coordinator supplies only identities and paths revalidated during the
  // current invocation. Implementations must independently reopen and verify
  // the image, re-identify the target, and validate the confirmation before
  // their first write.
  [[nodiscard]] virtual clonecore::Result<RestoreExecutionReport> execute(
      const RestoreExecutionRequest& request) = 0;
};

// Product WinPE restore implementation. It holds one non-share-write dcimg
// handle from full verification through restore, re-identifies the target
// before each state transition/open, and leaves a failed partial target
// offline. Calling execute performs destructive raw-disk writes.
[[nodiscard]] std::unique_ptr<IRestoreExecutionService>
make_windows_restore_execution_service();

class IJobManifestLoader {
 public:
  virtual ~IJobManifestLoader() = default;

  [[nodiscard]] virtual clonecore::Result<std::vector<std::byte>> load(
      const std::wstring& path) = 0;
};

class IJobManifestCandidateProvider {
 public:
  virtual ~IJobManifestCandidateProvider() = default;

  // Returns only bounded, fixed-name local candidates. It does not read or
  // parse file content, and performs no writes.
  [[nodiscard]] virtual clonecore::Result<std::vector<std::wstring>>
  candidates() = 0;
};

struct DiscoveredJobManifest final {
  std::wstring path;
  imageformat::VerifiedJobManifest verified_job;
};

// Builds the four fixed product candidate names for each eligible bit in the
// supplied drive mask. A:, B:, and WinPE's X: RAM drive are always excluded.
// No directory traversal or recursive search is performed.
[[nodiscard]] std::vector<std::wstring>
build_job_manifest_candidate_paths(std::uint32_t logical_drive_mask);

// Treats every reported candidate as untrusted. Zero candidates is a normal
// empty result; duplicate, invalid, or multiple valid candidates fail closed.
[[nodiscard]] clonecore::Result<std::optional<DiscoveredJobManifest>>
discover_unique_job_manifest(
    IJobManifestCandidateProvider& candidate_provider,
    IJobManifestLoader& loader);

// Enumerates fixed/removable local drives and returns only existing regular
// non-reparse files at the fixed candidate paths. It performs no writes and
// does not request elevation.
[[nodiscard]] std::unique_ptr<IJobManifestCandidateProvider>
make_windows_job_manifest_candidate_provider();

class IRestoreImageVerifier {
 public:
  virtual ~IRestoreImageVerifier() = default;

  [[nodiscard]] virtual clonecore::Result<
      imageformat::RestoreImageInspectionReport>
  verify(const std::wstring& path) = 0;
};

class IRestoreExecutionSafetyProbe {
 public:
  virtual ~IRestoreExecutionSafetyProbe() = default;

  // Implementations may inspect only the already selected target and verified
  // image. They must not open the target for write access or change disk state.
  [[nodiscard]] virtual clonecore::Result<
      RestoreExecutionSafetyObservation>
  inspect(
      const diskmodel::DiskInfo& target,
      const imageformat::RestoreImageInspectionReport& image) = 0;
};

// Combines the already verified image and fresh read-only inventory data with
// GetSystemPowerStatus. It performs no disk writes and does not request
// elevation. Pending-restart remains an advisory unknown.
[[nodiscard]] std::unique_ptr<IRestoreExecutionSafetyProbe>
make_windows_restore_execution_safety_probe();

class IRestoreImageCandidateProvider {
 public:
  virtual ~IRestoreImageCandidateProvider() = default;

  // Returns only bounded candidate paths. It does not verify or open the
  // image; every candidate remains untrusted.
  [[nodiscard]] virtual clonecore::Result<std::vector<std::wstring>>
  candidates_for(const std::wstring& configured_path) = 0;
};

// Replaces only the drive letter while preserving the exact relative path.
// A: and B: are ignored, as is WinPE's X: RAM drive. No directory traversal
// or recursive search is performed.
[[nodiscard]] std::vector<std::wstring>
build_restore_image_candidate_paths(
    const std::wstring& configured_path,
    std::uint32_t logical_drive_mask);

// Enumerates fixed/removable local drives, filters to existing non-directory
// paths, performs no writes, and does not request elevation.
[[nodiscard]] std::unique_ptr<IRestoreImageCandidateProvider>
make_windows_restore_image_candidate_provider();

// Opens one local regular file read-only, rejects reparse points and files
// larger than the job-manifest limit, and does not request elevation.
[[nodiscard]] std::unique_ptr<IJobManifestLoader>
make_windows_job_manifest_loader();

// Wraps an untrusted loader and requires the canonical payload hash to equal
// the hash approved during the current GUI preflight. A job replaced between
// review and execution is rejected before disk enumeration or physical I/O.
[[nodiscard]] std::unique_ptr<IJobManifestLoader>
make_hash_locked_job_manifest_loader(
    std::unique_ptr<IJobManifestLoader> inner,
    const imageformat::Sha256Digest& expected_payload_hash);

// Uses the shared ImageFormat verifier to open a local .dcimg read-only and
// verify the full container, all chunks, metadata, and restore layout.
[[nodiscard]] std::unique_ptr<IRestoreImageVerifier>
make_windows_restore_image_verifier();

[[nodiscard]] int run_winpe_app(
    const std::vector<std::wstring>& arguments,
    diskmodel::IDiskInventoryProvider& provider,
    std::ostream& output,
    std::ostream& error_output,
    ICloneExecutionService* execution_service = nullptr,
    bootrepair::IStandaloneBootRepairService* boot_repair_service = nullptr,
    IJobManifestLoader* job_manifest_loader = nullptr,
    IRestoreImageVerifier* restore_image_verifier = nullptr,
    IRestoreExecutionSafetyProbe* restore_safety_probe = nullptr,
    IRestoreImageCandidateProvider* restore_image_candidates = nullptr,
    const clonecore::DiskOperationCallbacks* operation_callbacks = nullptr,
    IRestoreExecutionService* restore_execution_service = nullptr,
    ICloneExecutionService* clone_job_execution_service = nullptr,
    bootrepair::IWinReDiagnosticService* winre_diagnostic_service = nullptr,
    IMbr2GptJobExecutionService* mbr2gpt_job_execution_service = nullptr);

}  // namespace ytec::winpeapp
