#include "ytec/windowsapp/adk_consent_review.h"

#include <Windows.h>

#include <sstream>
#include <string_view>
#include <utility>

namespace ytec::windowsapp {
namespace {

AdkConsentReviewView issue_view(
    const AdkConsentReviewIssue issue,
    std::wstring message) {
  return AdkConsentReviewView{
      .issue = issue,
      .message = std::move(message),
  };
}

clonecore::Error consent_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = L"ADK利用条件レビュー",
      .message = std::move(message),
  };
}

std::wstring widen_ascii(const std::string_view value) {
  return std::wstring(value.begin(), value.end());
}

bool eula_pin_is_declared(const AdkReleaseManifest& manifest) {
  const auto& eula = manifest.embedded_eula;
  return eula.primary_source_confirmed &&
         !eula.official_bootstrap_url.empty() &&
         eula.container_offset != 0U && eula.container_length != 0U &&
         !eula.container_member_name.empty() &&
         !eula.display_file_name.empty() &&
         !eula.expected_sha256.empty() &&
         eula.expected_byte_count != 0U &&
         !eula.expected_document_title.empty();
}

bool receipt_has_verified_provenance(
    const AdkEulaDocumentReceipt& receipt) noexcept {
  return receipt.bootstrap_full_identity_verified &&
         receipt.attached_container_bounds_verified &&
         receipt.burn_ux_mapping_verified &&
         receipt.member_copy_created_new &&
         receipt.member_copy_regular_file &&
         receipt.member_copy_single_link &&
         !receipt.member_copy_reparse_point &&
         receipt.bounded_read_complete &&
         receipt.temporary_files_removed;
}

bool receipt_matches_manifest(
    const AdkReleaseManifest& manifest,
    const AdkEulaDocumentReceipt& receipt) {
  return receipt.extracted_identity == manifest.embedded_eula;
}

std::wstring build_review_summary(
    const AdkReleaseManifest& manifest) {
  std::wostringstream summary;
  const auto& eula = manifest.embedded_eula;
  summary << L"Microsoft公式案内: " << manifest.information_url
          << L"\nADK: " << manifest.tested_adk_version
          << L"\nADK固有EULAを含むMicrosoft bootstrap: "
          << eula.official_bootstrap_url
          << L"\nEULA container offset: " << eula.container_offset
          << L" bytes\nEULA container length: "
          << eula.container_length << L" bytes"
          << L"\nEULA表示名: " << eula.display_file_name.native()
          << L"\nEULA container member: "
          << eula.container_member_name
          << L"\nEULA長さ: " << eula.expected_byte_count
          << L" bytes\nEULA SHA-256: "
          << widen_ascii(eula.expected_sha256)
          << L"\nEULA文書: " << eula.expected_document_title
          << L"\n\n取得内容:";
  for (const auto& payload : manifest.payloads) {
    summary << L"\n- " << payload.display_name
            << L"\n  公式取得元: " << payload.exact_source_url
            << L"\n  長さ: " << payload.expected_byte_count
            << L" bytes\n  SHA-256: "
            << widen_ascii(payload.expected_sha256);
    for (const auto& component : payload.acquired_components) {
      summary << L"\n  ・" << component;
    }
  }
  summary << L"\n\n利用条件を最後まで表示し、内容を確認した利用者が"
             L"明示的に同意した場合だけ導入へ進みます。";
  return summary.str();
}

}  // namespace

AdkConsentReviewView build_adk_consent_review(
    const AdkReleaseManifest* manifest,
    const AdkEulaDocumentReceipt* eula_receipt) {
  if (manifest == nullptr) {
    return issue_view(
        AdkConsentReviewIssue::blocked_manifest_missing,
        L"ADK固定マニフェストがないため、取得や導入へ進めません。");
  }
  if (!eula_pin_is_declared(*manifest)) {
    return issue_view(
        AdkConsentReviewIssue::blocked_eula_source_unconfirmed,
        L"Microsoft署名済みADK bootstrap内のADK固有EULAについて、"
        L"有界container範囲、member、本文の長さとSHA-256が未確認です。"
        L"汎用Microsoft規約や別製品の利用条件では代用しません。");
  }
  if (eula_receipt == nullptr) {
    return issue_view(
        AdkConsentReviewIssue::
            blocked_eula_receipt_unavailable,
        L"Microsoft署名・版・bootstrap全体SHA-256の検証後に、"
        L"固定CAB範囲とmemberだけを有界抽出したreceiptが未指定です。"
        L"抽出結果を全文表示する製品UIは未接続のため、"
        L"自動取得・導入は停止中です。");
  }
  const auto manifest_status = validate_adk_release_manifest(*manifest);
  if (!manifest_status) {
    return issue_view(
        AdkConsentReviewIssue::blocked_manifest_invalid,
        L"ADK固定マニフェストが安全検証に合格しないため、"
        L"取得や導入へ進めません: " + manifest_status.error().message);
  }
  if (!receipt_has_verified_provenance(*eula_receipt)) {
    return issue_view(
        AdkConsentReviewIssue::blocked_eula_receipt_unverified,
        L"ADK bootstrap全体の署名・版・Hash、固定CAB範囲、"
        L"Burn UX mapping、非reparse新規member、または"
        L"有界完全読取りを証明できません。");
  }
  if (!receipt_matches_manifest(*manifest, *eula_receipt)) {
    return issue_view(
        AdkConsentReviewIssue::blocked_eula_receipt_mismatch,
        L"抽出したADK固有EULAのbootstrap、container範囲、member、"
        L"表示名、長さ、文書名、またはSHA-256が固定値と一致しません。");
  }

  AdkConsentReviewView view{
      .issue = AdkConsentReviewIssue::review_required,
      .ready_to_present = true,
      .consent_permitted = false,
      .manifest_id = manifest->manifest_id,
      .product_release_version = manifest->product_release_version,
      .tested_adk_version = manifest->tested_adk_version,
      .information_url = manifest->information_url,
      .embedded_eula = manifest->embedded_eula,
      .message =
          L"Microsoft公式取得元、取得内容、検証済みADK固有EULAを"
          L"確認してください。まだ取得・導入処理は開始していません。",
      .summary = build_review_summary(*manifest),
  };
  view.payloads.reserve(manifest->payloads.size());
  for (const auto& payload : manifest->payloads) {
    view.payloads.push_back(AdkConsentPayloadReview{
        .kind = payload.kind,
        .display_name = payload.display_name,
        .exact_source_url = payload.exact_source_url,
        .expected_byte_count = payload.expected_byte_count,
        .expected_sha256 = payload.expected_sha256,
        .acquired_components = payload.acquired_components,
    });
  }
  return view;
}

clonecore::Result<AdkAcquisitionConsent>
complete_adk_consent_review(
    const AdkReleaseManifest& manifest,
    const AdkEulaDocumentReceipt& eula_receipt,
    const AdkConsentReviewAcknowledgement& acknowledgement) {
  const auto review = build_adk_consent_review(
      &manifest, &eula_receipt);
  if (!review.ready_to_present ||
      review.issue != AdkConsentReviewIssue::review_required) {
    return clonecore::Result<AdkAcquisitionConsent>::failure(
        consent_error(
            clonecore::ErrorCode::verification_failed,
            ERROR_REVISION_MISMATCH,
            review.message.empty()
                ? L"ADK利用条件レビューを安全に開始できません"
                : review.message));
  }
  if (acknowledgement.reviewed_manifest_id != manifest.manifest_id ||
      acknowledgement.reviewed_embedded_eula !=
          manifest.embedded_eula) {
    return clonecore::Result<AdkAcquisitionConsent>::failure(
        consent_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_REVISION_MISMATCH,
            L"画面で確認したマニフェストまたはADK固有EULAが現在の固定値と一致しません"));
  }
  if (!acknowledgement.official_sources_presented ||
      !acknowledgement.acquired_components_presented) {
    return clonecore::Result<AdkAcquisitionConsent>::failure(
        consent_error(
            clonecore::ErrorCode::confirmation_required,
            ERROR_CANCELLED,
            L"Microsoft公式取得元と取得内容をすべて表示してから確認してください"));
  }
  if (!acknowledgement.eula_body_opened ||
      !acknowledgement.eula_body_fully_presented) {
    return clonecore::Result<AdkAcquisitionConsent>::failure(
        consent_error(
            clonecore::ErrorCode::confirmation_required,
            ERROR_CANCELLED,
            L"検証済みADK固有EULA本文を最後まで表示してから確認してください"));
  }
  if (!acknowledgement.explicit_acceptance) {
    return clonecore::Result<AdkAcquisitionConsent>::failure(
        consent_error(
            clonecore::ErrorCode::confirmation_required,
            ERROR_CANCELLED,
            L"利用者による明示的な同意がないため、ADK導入へ進みません"));
  }

  AdkAcquisitionConsent consent{
      .accepted = true,
      .presented_manifest_id = manifest.manifest_id,
      .presented_embedded_eula = manifest.embedded_eula,
  };
  consent.presented_payloads.reserve(manifest.payloads.size());
  for (const auto& payload : manifest.payloads) {
    consent.presented_payloads.push_back(payload.kind);
  }
  return clonecore::Result<AdkAcquisitionConsent>::success(
      std::move(consent));
}

}  // namespace ytec::windowsapp
