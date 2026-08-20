#pragma once

#include "ytec/operationcore/checkpoint.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ytec::operationcore {

// The product owns one active resume slot. Callers never pass a path to an
// individual operation; the platform adapter is configured once and reports
// the fixed path on every observation.
inline constexpr std::wstring_view kResumeSlotFileName =
    L"active.checkpoint";

enum class ResumeCapability : std::uint8_t {
  persistent_exact_restore,
  persistent_rescue_restore,
  same_process_only_vss_image_create,
  same_process_only_vss_clone,
  same_process_only_pe_image_create,
  same_process_only_pe_clone,
  unsupported_shrink_migration,
  unsupported_raw_rescue,
};

enum class ResumeLifetime : std::uint8_t {
  persistent,
  same_process_only,
  unsupported,
};

[[nodiscard]] ResumeLifetime resume_lifetime(
    ResumeCapability capability) noexcept;

[[nodiscard]] clonecore::Status validate_resume_capability(
    ResumeCapability capability,
    OperationKind kind,
    OperationEnvironment environment);

// These are operation-specific identity digests produced by the owning
// engine. They may identify a disk, an authenticated image, or an output file
// object. The slot layer never guesses them from a path or disk number.
struct ResumeIdentityBinding final {
  Sha256Digest source_identity_hash{};
  Sha256Digest target_identity_hash{};
  Sha256Digest output_identity_hash{};
};

// A .partial file is optional (whole-disk restore has none). When declared,
// the platform must obtain file_object_identity_hash from an open regular
// single-link file, not from a pathname alone.
struct ResumeOwnedPartialBinding final {
  OperationId operation_id{};
  ResumeIdentityBinding identities{};
  Sha256Digest file_object_identity_hash{};
};

struct ResumeSlotRecord final {
  ResumeCapability capability{ResumeCapability::persistent_exact_restore};
  ParsedCheckpoint checkpoint{};
  ResumeIdentityBinding identities{};
  std::optional<ResumeOwnedPartialBinding> owned_partial;
};

// A stale UI observation cannot authorize resume, replacement, or discard.
// Every mutating platform call receives this complete immutable binding and
// must re-check it on the open file handles immediately before mutation.
struct ResumeSlotBinding final {
  ResumeCapability capability{ResumeCapability::persistent_exact_restore};
  OperationId operation_id{};
  ResumeIdentityBinding identities{};
  Sha256Digest checkpoint_record_hash{};
  std::optional<Sha256Digest> partial_file_object_identity_hash;
};

struct ResumeFileStorageProof final {
  bool exists{};
  bool is_regular_file{};
  bool is_reparse_free{};
  std::uint32_t hard_link_count{};
};

// This proof is deliberately produced by a platform seam. A concrete Windows
// adapter must use opened handles to prove regular-file/reparse/link state and
// backing-storage placement. A query failure is not represented as false data;
// it must fail observe_fixed_slot().
struct ResumeSlotStorageProof final {
  std::wstring checkpoint_path;
  bool paths_are_canonical_local{};
  bool parent_chain_reparse_free{};
  bool placement_separated_from_source{};
  bool checkpoint_and_partial_paths_distinct{};
  ResumeFileStorageProof checkpoint_file{};
  ResumeFileStorageProof owned_partial_file{};
};

struct ResumeSlotObservation final {
  ResumeSlotStorageProof storage{};
  std::optional<ResumeSlotRecord> slot;
  std::optional<ResumeOwnedPartialBinding> observed_owned_partial;
};

class IResumeSlotPlatform {
 public:
  virtual ~IResumeSlotPlatform() = default;

  // Observes one configured slot only. Directory scanning and history/list
  // semantics are outside this interface.
  [[nodiscard]] virtual clonecore::Result<ResumeSlotObservation>
  observe_fixed_slot() = 0;

  [[nodiscard]] virtual clonecore::Status create_fixed_slot(
      const ResumeSlotRecord& record) = 0;

  [[nodiscard]] virtual clonecore::Status replace_fixed_slot(
      const Sha256Digest& expected_checkpoint_record_hash,
      const ResumeSlotRecord& next) = 0;

  // If binding declares a partial, both open objects must still match and the
  // platform removes that owned partial and checkpoint as one guarded action.
  // If it does not, only the exactly matched checkpoint may be removed.
  [[nodiscard]] virtual clonecore::Status
  discard_fixed_slot_and_owned_partial(
      const ResumeSlotBinding& binding) = 0;
};

[[nodiscard]] clonecore::Status validate_resume_slot_record(
    const ResumeSlotRecord& record);

[[nodiscard]] clonecore::Result<ResumeSlotBinding>
make_resume_slot_binding(const ResumeSlotRecord& record);

class SingleResumeSlot final {
 public:
  explicit SingleResumeSlot(IResumeSlotPlatform& platform) noexcept;

  SingleResumeSlot(const SingleResumeSlot&) = delete;
  SingleResumeSlot& operator=(const SingleResumeSlot&) = delete;
  SingleResumeSlot(SingleResumeSlot&&) = delete;
  SingleResumeSlot& operator=(SingleResumeSlot&&) = delete;

  // Returns no value only when both the checkpoint and owned partial are
  // absent. Corrupt, unknown, orphaned, relinked, or relocated state fails
  // closed and remains untouched.
  [[nodiscard]] clonecore::Result<std::optional<ResumeSlotRecord>> inspect();

  [[nodiscard]] clonecore::Result<ResumeSlotRecord> open_bound(
      const ResumeSlotBinding& expected);

  [[nodiscard]] clonecore::Status create(const ResumeSlotRecord& record);

  [[nodiscard]] clonecore::Status replace(
      const ResumeSlotBinding& expected,
      const ParsedCheckpoint& next_checkpoint);

  [[nodiscard]] clonecore::Status discard(
      const ResumeSlotBinding& expected);

 private:
  [[nodiscard]] clonecore::Result<ResumeSlotObservation> observe();

  IResumeSlotPlatform* platform_{};
  std::optional<std::wstring> bound_checkpoint_path_;
};

}  // namespace ytec::operationcore
