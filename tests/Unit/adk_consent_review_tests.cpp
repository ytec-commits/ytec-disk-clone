#include "ytec/windowsapp/adk_consent_review.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

namespace {

struct TestFailure final {
  std::string message;
};

void check(const bool condition, std::string message) {
  if (!condition) {
    throw TestFailure{std::move(message)};
  }
}

std::string hash_of(const char value) {
  return std::string(64U, value);
}

ytec::windowsapp::AdkReleaseManifest confirmed_manifest() {
  using ytec::windowsapp::AdkInstallerKind;
  using ytec::windowsapp::AdkPayloadKind;
  using ytec::windowsapp::AdkPinnedPayload;
  return ytec::windowsapp::AdkReleaseManifest{
      .manifest_id = "tsumugi-adk-consent-test",
      .product_release_version = L"1.0.0",
      .tested_adk_version = L"10.1.26100.2454",
      .information_url =
          L"https://learn.microsoft.com/en-us/windows-hardware/get-started/adk-install",
      .embedded_eula = ytec::windowsapp::AdkEmbeddedEulaPin{
          .source_payload_kind = AdkPayloadKind::deployment_tools,
          .official_bootstrap_url =
              L"https://download.microsoft.com/download/a/adksetup.exe",
          .container_offset = 10U,
          .container_length = 80U,
          .container_member_name = L"u6",
          .display_file_name = L"ja\\eula.rtf",
          .expected_byte_count = 50U,
          .expected_sha256 = hash_of('D'),
          .expected_document_title =
              L"WINDOWS ASSESSMENT AND DEPLOYMENT KIT (ADK)",
          .primary_source_confirmed = true,
      },
      .unattended_install_no_unexpected_restart_confirmed = true,
      .expected_deployment_tools_version = L"10.1.26100.2454",
      .expected_winpe_addon_version = L"10.1.26100.2454",
      .expected_serviced_dism_version = L"10.0.26100.8972",
      .required_servicing_update_id = L"KB5101684",
      .payloads = {
          AdkPinnedPayload{
              .kind = AdkPayloadKind::deployment_tools,
              .installer_kind =
                  AdkInstallerKind::microsoft_bootstrap_exe,
              .display_name = L"Windows ADK Deployment Tools",
              .staging_file_name = L"adksetup.exe",
              .offline_relative_path = L"adksetup.exe",
              .exact_source_url =
                  L"https://download.microsoft.com/download/a/adksetup.exe",
              .expected_sha256 = hash_of('A'),
              .expected_signer_subject = L"Microsoft Corporation",
              .expected_payload_version = L"10.1.26100.2454",
              .acquired_components = {L"Deployment Tools"},
              .uninstall_registration_id = L"ADK-DEPLOYMENT-TEST",
              .expected_byte_count = 100U,
              .maximum_bytes = 1'024U,
          },
          AdkPinnedPayload{
              .kind = AdkPayloadKind::winpe_addon,
              .installer_kind =
                  AdkInstallerKind::microsoft_bootstrap_exe,
              .display_name = L"Windows PE Add-on",
              .staging_file_name = L"adkwinpesetup.exe",
              .offline_relative_path = L"adkwinpesetup.exe",
              .exact_source_url =
                  L"https://download.microsoft.com/download/b/adkwinpesetup.exe",
              .expected_sha256 = hash_of('B'),
              .expected_signer_subject = L"Microsoft Corporation",
              .expected_payload_version = L"10.1.26100.2454",
              .acquired_components = {
                  L"Windows Preinstallation Environment"},
              .uninstall_registration_id = L"ADK-WINPE-TEST",
              .expected_byte_count = 101U,
              .maximum_bytes = 2'048U,
          },
          AdkPinnedPayload{
              .kind = AdkPayloadKind::servicing_update,
              .installer_kind = AdkInstallerKind::windows_update_msu,
              .display_name = L"Windows ADK Servicing Update",
              .staging_file_name = L"adk-update.msu",
              .offline_relative_path = L"adk-update.msu",
              .exact_source_url =
                  L"https://download.microsoft.com/download/c/adk-update.msu",
              .expected_sha256 = hash_of('C'),
              .expected_signer_subject = L"Microsoft Corporation",
              .expected_payload_version = L"10.0.26100.8972",
              .acquired_components = {L"DISM", L"Oscdimg"},
              .uninstall_registration_id = L"KB5101684",
              .expected_byte_count = 102U,
              .maximum_bytes = 4'096U,
          },
      },
      .primary_source_pins_confirmed = true,
  };
}

ytec::windowsapp::AdkEulaDocumentReceipt receipt_for(
    const ytec::windowsapp::AdkReleaseManifest& manifest) {
  return ytec::windowsapp::AdkEulaDocumentReceipt{
      .extracted_identity = manifest.embedded_eula,
      .bootstrap_full_identity_verified = true,
      .attached_container_bounds_verified = true,
      .burn_ux_mapping_verified = true,
      .member_copy_created_new = true,
      .member_copy_regular_file = true,
      .member_copy_single_link = true,
      .member_copy_reparse_point = false,
      .bounded_read_complete = true,
      .temporary_files_removed = true,
  };
}

ytec::windowsapp::AdkConsentReviewAcknowledgement acknowledgement_for(
    const ytec::windowsapp::AdkReleaseManifest& manifest) {
  return ytec::windowsapp::AdkConsentReviewAcknowledgement{
      .reviewed_manifest_id = manifest.manifest_id,
      .reviewed_embedded_eula = manifest.embedded_eula,
      .official_sources_presented = true,
      .acquired_components_presented = true,
      .eula_body_opened = true,
      .eula_body_fully_presented = true,
      .explicit_acceptance = true,
  };
}

void test_product_manifest_names_exact_eula_ui_blocker() {
  const auto manifest =
      ytec::windowsapp::tsumugi_1_0_0_adk_manifest();
  check(
      manifest.embedded_eula.official_bootstrap_url ==
              manifest.payloads[0].exact_source_url &&
          manifest.embedded_eula.container_offset == 0xB3000U &&
          manifest.embedded_eula.container_length == 0x16C2CDU &&
          manifest.embedded_eula.container_member_name == L"u6" &&
          manifest.embedded_eula.display_file_name ==
              std::filesystem::path(L"ja\\eula.rtf") &&
          manifest.embedded_eula.expected_byte_count == 293'766U &&
          manifest.embedded_eula.expected_sha256 ==
              "32B66AE90683DE9C91EDE927A45E8E44845CD36E43821BFA4EB2CA5C36A9CF54" &&
          manifest.embedded_eula.primary_source_confirmed &&
          !manifest.unattended_install_no_unexpected_restart_confirmed &&
          !manifest.primary_source_pins_confirmed,
      "The product must retain the audited embedded EULA pin and closed execution gates");
  const auto view = ytec::windowsapp::build_adk_consent_review(
      &manifest, nullptr);
  check(
      view.issue == ytec::windowsapp::AdkConsentReviewIssue::
                        blocked_eula_receipt_unavailable &&
          !view.ready_to_present && !view.consent_permitted,
      "The product must remain blocked until the verified receipt reaches the UI");
  check(
      view.message.find(L"製品UIは未接続") !=
          std::wstring::npos,
      "The blocker must name the exact remaining presentation UI gap");

  const auto fabricated_receipt = receipt_for(manifest);
  const auto blocked_consent =
      ytec::windowsapp::complete_adk_consent_review(
          manifest,
          fabricated_receipt,
          acknowledgement_for(manifest));
  check(
      !blocked_consent,
      "Even a matching receipt cannot bypass the closed product release gate");
}

void test_generic_terms_and_missing_receipt_fail_closed() {
  auto manifest = confirmed_manifest();
  auto view = ytec::windowsapp::build_adk_consent_review(
      &manifest, nullptr);
  check(
      view.issue == ytec::windowsapp::AdkConsentReviewIssue::
                        blocked_eula_receipt_unavailable,
      "A confirmed pin still requires a verified document receipt");

  manifest.embedded_eula.official_bootstrap_url =
      L"https://www.microsoft.com/en-us/licensing/terms";
  auto generic_receipt = receipt_for(manifest);
  view = ytec::windowsapp::build_adk_consent_review(
      &manifest, &generic_receipt);
  check(
      view.issue == ytec::windowsapp::AdkConsentReviewIssue::
                        blocked_manifest_invalid,
      "A generic Microsoft terms page must fail manifest validation");
}

void test_unverified_or_changed_receipt_cannot_reach_review() {
  const auto manifest = confirmed_manifest();
  auto receipt = receipt_for(manifest);
  receipt.bootstrap_full_identity_verified = false;
  auto view = ytec::windowsapp::build_adk_consent_review(
      &manifest, &receipt);
  check(
      view.issue == ytec::windowsapp::AdkConsentReviewIssue::
                        blocked_eula_receipt_unverified,
      "Unverified Microsoft provenance must fail");

  receipt = receipt_for(manifest);
  receipt.member_copy_reparse_point = true;
  view = ytec::windowsapp::build_adk_consent_review(&manifest, &receipt);
  check(
      view.issue == ytec::windowsapp::AdkConsentReviewIssue::
                        blocked_eula_receipt_unverified,
      "A reparse staged document must fail");

  receipt = receipt_for(manifest);
  receipt.extracted_identity.expected_sha256 = hash_of('0');
  view = ytec::windowsapp::build_adk_consent_review(&manifest, &receipt);
  check(
      view.issue == ytec::windowsapp::AdkConsentReviewIssue::
                        blocked_eula_receipt_mismatch,
      "A different EULA body must fail exact identity matching");
}

void test_ready_review_contains_every_exact_source_and_component() {
  const auto manifest = confirmed_manifest();
  const auto receipt = receipt_for(manifest);
  const auto view = ytec::windowsapp::build_adk_consent_review(
      &manifest, &receipt);
  check(
      view.issue ==
              ytec::windowsapp::AdkConsentReviewIssue::review_required &&
          view.ready_to_present && !view.consent_permitted,
      "Verified inputs should create a review, not implicit consent");
  check(
      view.payloads.size() == manifest.payloads.size() &&
          view.payloads[0].exact_source_url ==
              manifest.payloads[0].exact_source_url &&
          view.payloads[1].acquired_components ==
              manifest.payloads[1].acquired_components &&
          view.embedded_eula == manifest.embedded_eula,
      "Review rows must preserve exact sources, components, and EULA pin");
  check(
      view.summary.find(manifest.payloads[2].exact_source_url) !=
              std::wstring::npos &&
          view.summary.find(L"Windows Preinstallation Environment") !=
              std::wstring::npos,
      "Display summary must include every acquired source and component");
}

void test_consent_requires_full_same_review_and_explicit_acceptance() {
  const auto manifest = confirmed_manifest();
  const auto receipt = receipt_for(manifest);
  auto acknowledgement = acknowledgement_for(manifest);

  acknowledgement.eula_body_fully_presented = false;
  auto result = ytec::windowsapp::complete_adk_consent_review(
      manifest, receipt, acknowledgement);
  check(!result, "A partially displayed EULA must not issue consent");

  acknowledgement = acknowledgement_for(manifest);
  acknowledgement.explicit_acceptance = false;
  result = ytec::windowsapp::complete_adk_consent_review(
      manifest, receipt, acknowledgement);
  check(!result, "Review without explicit acceptance must not issue consent");

  acknowledgement = acknowledgement_for(manifest);
  acknowledgement.reviewed_embedded_eula.expected_sha256 = hash_of('0');
  result = ytec::windowsapp::complete_adk_consent_review(
      manifest, receipt, acknowledgement);
  check(!result, "Consent cannot move to a different EULA revision");

  acknowledgement = acknowledgement_for(manifest);
  result = ytec::windowsapp::complete_adk_consent_review(
      manifest, receipt, acknowledgement);
  check(result.has_value(), "A complete exact review should issue consent");
  check(
      result.value().accepted &&
          result.value().presented_manifest_id == manifest.manifest_id &&
          result.value().presented_embedded_eula ==
              manifest.embedded_eula &&
          result.value().presented_payloads.size() == 3U,
      "Issued consent must bind the exact manifest, EULA, and payload order");
}

void test_receipt_requires_matching_container_range_and_member() {
  const auto manifest = confirmed_manifest();
  auto receipt = receipt_for(manifest);
  receipt.extracted_identity.container_offset += 1U;
  auto view = ytec::windowsapp::build_adk_consent_review(
      &manifest, &receipt);
  check(
      view.issue == ytec::windowsapp::AdkConsentReviewIssue::
                        blocked_eula_receipt_mismatch,
      "A different attached container range must fail");

  receipt = receipt_for(manifest);
  receipt.extracted_identity.container_member_name = L"u7";
  view = ytec::windowsapp::build_adk_consent_review(&manifest, &receipt);
  check(
      view.issue == ytec::windowsapp::AdkConsentReviewIssue::
                        blocked_eula_receipt_mismatch,
      "A different Burn UX member must fail");
}

}  // namespace

int main() {
  try {
    test_product_manifest_names_exact_eula_ui_blocker();
    test_generic_terms_and_missing_receipt_fail_closed();
    test_unverified_or_changed_receipt_cannot_reach_review();
    test_ready_review_contains_every_exact_source_and_component();
    test_consent_requires_full_same_review_and_explicit_acceptance();
    test_receipt_requires_matching_container_range_and_member();
    std::cout << "adk consent review tests: PASS\n";
    return 0;
  } catch (const TestFailure& failure) {
    std::cerr << "adk consent review tests: FAIL: "
              << failure.message << '\n';
    return 1;
  }
}
