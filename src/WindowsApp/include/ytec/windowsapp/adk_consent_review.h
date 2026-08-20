#pragma once

#include "ytec/windowsapp/adk_acquisition.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ytec::windowsapp {

struct AdkEulaDocumentReceipt final {
  AdkEmbeddedEulaPin extracted_identity;

  // The adapter first verifies the whole bootstrap using its release payload
  // URL, SHA-256, Authenticode signer and file version. It then reopens that
  // same owned file, copies only the pinned attached-CAB range, validates the
  // Burn UX mapping to the pinned member, and extracts that one member with
  // CREATE_NEW and bounded reads.
  bool bootstrap_full_identity_verified{};
  bool attached_container_bounds_verified{};
  bool burn_ux_mapping_verified{};
  bool member_copy_created_new{};
  bool member_copy_regular_file{};
  bool member_copy_single_link{};
  bool member_copy_reparse_point{};
  bool bounded_read_complete{};
  bool temporary_files_removed{};
};

enum class AdkConsentReviewIssue : std::uint8_t {
  blocked_manifest_missing,
  blocked_eula_source_unconfirmed,
  blocked_manifest_invalid,
  blocked_eula_receipt_unavailable,
  blocked_eula_receipt_unverified,
  blocked_eula_receipt_mismatch,
  review_required,
};

struct AdkConsentPayloadReview final {
  AdkPayloadKind kind{AdkPayloadKind::deployment_tools};
  std::wstring display_name;
  std::wstring exact_source_url;
  std::uint64_t expected_byte_count{};
  std::string expected_sha256;
  std::vector<std::wstring> acquired_components;
};

struct AdkConsentReviewView final {
  AdkConsentReviewIssue issue{
      AdkConsentReviewIssue::blocked_manifest_missing};
  bool ready_to_present{};
  bool consent_permitted{};
  std::string manifest_id;
  std::wstring product_release_version;
  std::wstring tested_adk_version;
  std::wstring information_url;
  AdkEmbeddedEulaPin embedded_eula;
  std::vector<AdkConsentPayloadReview> payloads;
  std::wstring message;
  std::wstring summary;
};

struct AdkConsentReviewAcknowledgement final {
  std::string reviewed_manifest_id;
  AdkEmbeddedEulaPin reviewed_embedded_eula;
  bool official_sources_presented{};
  bool acquired_components_presented{};
  bool eula_body_opened{};
  bool eula_body_fully_presented{};
  bool explicit_acceptance{};
};

// Produces display-ready, exact source/component rows only when both the
// release manifest and a bounded ADK-specific EULA receipt are verified.
// The bounded Windows extractor can produce the receipt, but current 1.0.0
// still has no product UI that presents the complete returned RTF before
// consent. A missing receipt remains a hard stop.
[[nodiscard]] AdkConsentReviewView build_adk_consent_review(
    const AdkReleaseManifest* manifest,
    const AdkEulaDocumentReceipt* eula_receipt);

// Issues the exact AdkAcquisitionConsent consumed by execute_adk_acquisition.
// The caller must have displayed every source/component row and the complete
// verified EULA body, then received an explicit user acceptance in this same
// review. No I/O or installer launch occurs here.
[[nodiscard]] clonecore::Result<AdkAcquisitionConsent>
complete_adk_consent_review(
    const AdkReleaseManifest& manifest,
    const AdkEulaDocumentReceipt& eula_receipt,
    const AdkConsentReviewAcknowledgement& acknowledgement);

}  // namespace ytec::windowsapp
