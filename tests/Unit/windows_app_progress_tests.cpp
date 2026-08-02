#include "ytec/imageformat/job_manifest.h"
#include "ytec/windowsapp/job_creation.h"
#include "ytec/windowsapp/progress.h"
#include "ytec/windowsapp/reboot_handoff.h"
#include "ytec/windowsapp/selection.h"

#include <chrono>
#include <iostream>
#include <limits>
#include <string>

namespace {

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

ytec::imageformat::Sha256Digest test_image_hash() {
  ytec::imageformat::Sha256Digest digest{};
  digest[0] = std::byte{0x42};
  return digest;
}

void test_units_and_duration() {
  check(
      ytec::windowsapp::format_bytes(1536) == L"1.5 KiB",
      "Binary byte units should be readable");
  check(
      ytec::windowsapp::format_duration(std::chrono::seconds(3661)) ==
          L"1時間1分1秒",
      "Duration should include hours, minutes, and seconds");
}

void test_eta_is_hidden_until_stable() {
  const auto view = ytec::windowsapp::calculate_progress(
      ytec::windowsapp::ProgressInput{
          .stage = ytec::windowsapp::JobStage::reading,
          .processed_bytes = 8ULL * 1024ULL * 1024ULL,
          .total_bytes = 64ULL * 1024ULL * 1024ULL,
          .elapsed = std::chrono::seconds(2),
          .cancellation_allowed = true});
  check(!view.remaining.has_value(), "Early ETA must remain hidden");
  check(view.remaining_label == L"計算中", "Active ETA should say calculating");
  check(view.cancellation_allowed, "Cancellation state should be preserved");
}

void test_eta_and_progress_are_bounded() {
  const auto view = ytec::windowsapp::calculate_progress(
      ytec::windowsapp::ProgressInput{
          .stage = ytec::windowsapp::JobStage::writing,
          .processed_bytes = 32ULL * 1024ULL * 1024ULL,
          .total_bytes = 64ULL * 1024ULL * 1024ULL,
          .elapsed = std::chrono::seconds(4)});
  check(view.fraction == 0.5, "Progress should be one half");
  check(view.bytes_per_second == 8ULL * 1024ULL * 1024ULL,
        "Speed should be calculated from elapsed time");
  check(
      view.remaining == std::chrono::seconds(4),
      "ETA should be calculated after the stability threshold");

  const auto overflow_safe = ytec::windowsapp::calculate_progress(
      ytec::windowsapp::ProgressInput{
          .stage = ytec::windowsapp::JobStage::verifying,
          .processed_bytes = 16ULL * 1024ULL * 1024ULL,
          .total_bytes = (std::numeric_limits<std::uint64_t>::max)(),
          .elapsed = std::chrono::seconds(3)});
  check(
      overflow_safe.remaining.has_value(),
      "Large totals should calculate without unsigned overflow");
}

void test_terminal_labels() {
  const auto waiting = ytec::windowsapp::calculate_progress(
      ytec::windowsapp::ProgressInput{});
  check(waiting.remaining_label == L"—", "Waiting ETA should be neutral");

  const auto completed = ytec::windowsapp::calculate_progress(
      ytec::windowsapp::ProgressInput{
          .stage = ytec::windowsapp::JobStage::completed,
          .processed_bytes = 100,
          .total_bytes = 100,
          .elapsed = std::chrono::seconds(8)});
  check(completed.fraction == 1.0, "Completed progress should be full");
  check(completed.remaining_label == L"0秒", "Completed ETA should be zero");
}

void test_online_image_progress_keeps_work_streams_distinct() {
  const auto writing =
      ytec::windowsapp::build_online_image_progress_view(
          ytec::clonecore::DiskOperationProgress{
              .stage =
                  ytec::clonecore::DiskOperationStage::copying_data,
              .total_read_bytes = 32ULL * 1024ULL * 1024ULL,
              .total_write_bytes = 32ULL * 1024ULL * 1024ULL,
              .total_verify_bytes = 64ULL * 1024ULL * 1024ULL,
              .read_bytes = 32ULL * 1024ULL * 1024ULL,
              .written_bytes = 32ULL * 1024ULL * 1024ULL,
              .verified_bytes = 0,
              .cancellation_allowed = true,
          },
          std::chrono::seconds(4));
  check(
      writing.fraction == 0.5 &&
          writing.percentage_label == L"50%",
      "Online-image percentage should cover read, write, and verification");
  check(
      writing.read_label == L"32 / 32 MiB" &&
          writing.write_label == L"32 / 32 MiB" &&
          writing.verified_label == L"0 / 64 MiB",
      "Online-image counters should stay visually distinct");
  check(
      writing.remaining_label == L"4秒" &&
          writing.cancellation_allowed,
      "Stable aggregate work should expose ETA and cancellation");

  const auto finalizing =
      ytec::windowsapp::build_online_image_progress_view(
          ytec::clonecore::DiskOperationProgress{
              .stage =
                  ytec::clonecore::DiskOperationStage::flushing_data,
              .total_read_bytes = 32ULL * 1024ULL * 1024ULL,
              .total_write_bytes = 32ULL * 1024ULL * 1024ULL,
              .total_verify_bytes = 64ULL * 1024ULL * 1024ULL,
              .read_bytes = 32ULL * 1024ULL * 1024ULL,
              .written_bytes = 32ULL * 1024ULL * 1024ULL,
              .verified_bytes = 64ULL * 1024ULL * 1024ULL,
              .cancellation_allowed = false,
          },
          std::chrono::seconds(9));
  check(
      finalizing.remaining_label == L"仕上げ中" &&
          !finalizing.cancellation_allowed,
      "Final file commit should avoid an exact ETA and disable cancellation");
}

void test_clone_selection_safety() {
  ytec::diskmodel::DiskInfo system;
  system.disk_number = 1;
  system.size_bytes = 512;
  system.is_system_disk = true;
  system.read_only = false;

  ytec::diskmodel::DiskInfo target;
  target.disk_number = 2;
  target.size_bytes = 1024;
  target.read_only = false;

  ytec::diskmodel::InventoryReport inventory;
  inventory.disks.push_back(system);
  inventory.disks.push_back(target);

  const auto ready = ytec::windowsapp::evaluate_clone_selection(
      &inventory, 0, 1, false);
  check(ready.ready, "System-to-larger-non-system selection should be ready");

  const auto same = ytec::windowsapp::evaluate_clone_selection(
      &inventory, 0, 0, false);
  check(
      same.issue == ytec::windowsapp::CloneSelectionIssue::same_disk,
      "The same disk must be rejected");

  const auto system_target =
      ytec::windowsapp::evaluate_clone_selection(
          &inventory, 1, 0, false);
  check(
      system_target.issue ==
          ytec::windowsapp::CloneSelectionIssue::target_is_system,
      "The running Windows disk must never be selected as target");

  inventory.disks[1].size_bytes = 256;
  const auto too_small = ytec::windowsapp::evaluate_clone_selection(
      &inventory, 0, 1, false);
  check(
      too_small.issue ==
          ytec::windowsapp::CloneSelectionIssue::target_too_small,
      "A smaller target must fail closed");

  inventory.disks[1].size_bytes = 1024;
  inventory.disks[1].read_only.reset();
  const auto unknown = ytec::windowsapp::evaluate_clone_selection(
      &inventory, 0, 1, false);
  check(
      unknown.issue ==
          ytec::windowsapp::CloneSelectionIssue::target_state_unknown,
      "Unknown target state must fail closed");
}

void test_confirmed_clone_job_is_hashed_and_target_bound() {
  ytec::diskmodel::DiskInfo source;
  source.disk_number = 1;
  source.model = L"Tsumugi Source";
  source.device_instance_id = L"MOCK\\SOURCE\\1";
  source.size_bytes = 512ULL * 1024U * 1024U * 1024U;
  source.logical_sector_size = 512;
  source.serial_suffix = "SOURCE01";
  source.is_system_disk = true;

  ytec::diskmodel::DiskInfo target;
  target.disk_number = 2;
  target.model = L"Tsumugi Target";
  target.device_instance_id = L"MOCK\\TARGET\\2";
  target.size_bytes = 1024ULL * 1024U * 1024U * 1024U;
  target.logical_sector_size = 512;
  target.serial_suffix = "TARGET02";
  target.is_system_disk = false;

  const std::wstring token =
      ytec::windowsapp::clone_job_confirmation_token(target);
  check(
      token == L"ERASE Tsumugi Target TARGET02 1099511627776",
      "Confirmation must bind model, serial suffix, and capacity");
  auto job = ytec::windowsapp::create_confirmed_clone_job(
      ytec::windowsapp::CloneJobCreationRequest{
          .source = source,
          .target = target,
          .first_step_acknowledged = true,
          .typed_confirmation = token,
          .auto_execute_once = true,
          .created_utc = "2026-07-31T04:00:00Z",
          .app_version = "0.1.0-dev",
      });
  check(job.has_value(), "Exact confirmation should create a job");
  const auto verified =
      ytec::imageformat::parse_and_verify_hashed_job_manifest(job.value());
  check(verified.has_value(), "Created job must verify");
  check(
      verified.value().manifest.target->serial_suffix == "TARGET02",
      "Created job must preserve the confirmed target identity");
  check(
      verified.value().manifest.execution_mode ==
          ytec::imageformat::JobExecutionMode::auto_once,
      "Explicit Windows choice must bind auto-once mode into the job");

  auto wrong = ytec::windowsapp::create_confirmed_clone_job(
      ytec::windowsapp::CloneJobCreationRequest{
          .source = source,
          .target = target,
          .first_step_acknowledged = true,
          .typed_confirmation = L"ERASE WRONG",
          .created_utc = "2026-07-31T04:00:00Z",
          .app_version = "0.1.0-dev",
      });
  check(!wrong.has_value(), "Wrong typed token must not create a job");
}

void test_confirmed_mbr_to_gpt_job_is_manual_and_layout_bound() {
  ytec::diskmodel::DiskInfo source;
  source.disk_number = 3;
  source.model = L"Tsumugi MBR Source";
  source.device_instance_id = L"MOCK\\MBR-SOURCE\\3";
  source.size_bytes = 256ULL * 1024U * 1024U * 1024U;
  source.logical_sector_size = 512;
  source.serial_suffix = "MBRSRC03";
  source.partition_style = ytec::diskmodel::PartitionStyle::mbr;
  source.partitions.push_back(ytec::diskmodel::PartitionInfo{
      .number = 1,
      .offset_bytes = 1'048'576,
      .size_bytes = source.size_bytes - 1'048'576,
      .style = ytec::diskmodel::PartitionStyle::mbr,
      .type = L"0x07",
      .bootable = true,
  });

  ytec::diskmodel::DiskInfo target;
  target.disk_number = 4;
  target.model = L"Tsumugi GPT Target";
  target.device_instance_id = L"MOCK\\GPT-TARGET\\4";
  target.size_bytes = 512ULL * 1024U * 1024U * 1024U;
  target.logical_sector_size = 512;
  target.serial_suffix = "GPTTGT04";
  target.partition_style = ytec::diskmodel::PartitionStyle::raw;

  const std::wstring token =
      ytec::windowsapp::clone_job_confirmation_token(target);
  auto job = ytec::windowsapp::create_confirmed_clone_job(
      ytec::windowsapp::CloneJobCreationRequest{
          .source = source,
          .target = target,
          .first_step_acknowledged = true,
          .typed_confirmation = token,
          .requested_conversion =
              ytec::imageformat::RequestedConversion::mbr_to_gpt,
          .auto_execute_once = false,
          .created_utc = "2026-08-01T12:00:00Z",
          .app_version = "0.2.0-dev",
      });
  check(job.has_value(),
        "A confirmed MBR source and empty RAW target should create a migration job");
  const auto verified =
      ytec::imageformat::parse_and_verify_hashed_job_manifest(job.value());
  check(verified.has_value() &&
            verified.value().manifest.job_type ==
                ytec::imageformat::JobType::mbr_to_gpt &&
            verified.value().manifest.requested_conversion ==
                ytec::imageformat::RequestedConversion::mbr_to_gpt &&
            verified.value().manifest.execution_mode ==
                ytec::imageformat::JobExecutionMode::review_required,
        "Migration jobs must bind their type, conversion, and manual review mode");

  auto automatic = ytec::windowsapp::create_confirmed_clone_job(
      ytec::windowsapp::CloneJobCreationRequest{
          .source = source,
          .target = target,
          .first_step_acknowledged = true,
          .typed_confirmation = token,
          .requested_conversion =
              ytec::imageformat::RequestedConversion::mbr_to_gpt,
          .auto_execute_once = true,
          .created_utc = "2026-08-01T12:00:00Z",
          .app_version = "0.2.0-dev",
      });
  check(!automatic.has_value(),
        "Firmware-changing migration must reject one-time automatic execution");

  source.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  source.partitions.front().style = ytec::diskmodel::PartitionStyle::gpt;
  auto wrong_source = ytec::windowsapp::create_confirmed_clone_job(
      ytec::windowsapp::CloneJobCreationRequest{
          .source = source,
          .target = target,
          .first_step_acknowledged = true,
          .typed_confirmation = token,
          .requested_conversion =
              ytec::imageformat::RequestedConversion::mbr_to_gpt,
          .created_utc = "2026-08-01T12:00:00Z",
          .app_version = "0.2.0-dev",
      });
  check(!wrong_source.has_value(),
        "A non-MBR source must never create an MBR-to-GPT job");
}

void test_confirmed_restore_job_is_hashed_and_target_bound() {
  ytec::diskmodel::DiskInfo target;
  target.disk_number = 7;
  target.model = L"Tsumugi Restore Target";
  target.device_instance_id = L"MOCK\\RESTORE\\7";
  target.size_bytes = 2ULL * 1024U * 1024U * 1024U * 1024U;
  target.logical_sector_size = 512;
  target.serial_suffix = "RESTORE7";
  target.is_system_disk = false;

  const std::wstring token =
      ytec::windowsapp::restore_job_confirmation_token(target);
  check(
      token ==
          L"ERASE Tsumugi Restore Target RESTORE7 2199023255552",
      "Restore confirmation must bind the exact target identity");
  auto job = ytec::windowsapp::create_confirmed_restore_job(
      ytec::windowsapp::RestoreJobCreationRequest{
          .target = target,
          .verified_image_path = L"D:\\Images\\verified.dcimg",
          .verified_image_length = 4096,
          .verified_image_global_hash = test_image_hash(),
          .first_step_acknowledged = true,
          .typed_confirmation = token,
          .created_utc = "2026-07-31T04:00:00Z",
          .app_version = "0.1.0-dev",
      });
  check(job.has_value(), "Exact restore confirmation should create a job");
  const auto verified =
      ytec::imageformat::parse_and_verify_hashed_job_manifest(job.value());
  check(verified.has_value(), "Created restore job must verify");
  check(
      verified.value().manifest.job_type ==
          ytec::imageformat::JobType::restore_image,
      "Restore job type must be preserved");
  check(
      !verified.value().manifest.source.has_value(),
      "Restore job must not contain a source disk");
  check(
      verified.value().manifest.target.has_value() &&
          verified.value().manifest.target->serial_suffix == "RESTORE7",
      "Restore job must preserve the confirmed target identity");
  check(
      verified.value().manifest.image_path ==
          L"D:\\Images\\verified.dcimg",
      "Restore job must preserve the verified image path");
  check(
      verified.value().manifest.restore_image_identity.has_value() &&
          verified.value().manifest.restore_image_identity->length_bytes ==
              4096 &&
          verified.value().manifest.restore_image_identity->global_hash ==
              test_image_hash(),
      "Restore job must preserve the verified image fingerprint");
  check(
      verified.value().manifest.destructive_target_confirmed,
      "Restore job must record the two-step confirmation");
  check(
      verified.value().manifest.execution_mode ==
          ytec::imageformat::JobExecutionMode::review_required,
      "Auto execution must remain opt-in by default");

  auto wrong = ytec::windowsapp::create_confirmed_restore_job(
      ytec::windowsapp::RestoreJobCreationRequest{
          .target = target,
          .verified_image_path = L"D:\\Images\\verified.dcimg",
          .verified_image_length = 4096,
          .verified_image_global_hash = test_image_hash(),
          .first_step_acknowledged = true,
          .typed_confirmation = L"ERASE WRONG",
          .created_utc = "2026-07-31T04:00:00Z",
          .app_version = "0.1.0-dev",
      });
  check(
      !wrong.has_value(),
      "Wrong typed token must not create a restore job");

  target.is_system_disk = true;
  const std::wstring system_token =
      ytec::windowsapp::restore_job_confirmation_token(target);
  auto system = ytec::windowsapp::create_confirmed_restore_job(
      ytec::windowsapp::RestoreJobCreationRequest{
          .target = target,
          .verified_image_path = L"D:\\Images\\verified.dcimg",
          .verified_image_length = 4096,
          .verified_image_global_hash = test_image_hash(),
          .first_step_acknowledged = true,
          .typed_confirmation = system_token,
          .created_utc = "2026-07-31T04:00:00Z",
          .app_version = "0.1.0-dev",
      });
  check(
      !system.has_value(),
      "Running Windows disk must never be used by a restore job");
}

class MockRebootHandoffService final
    : public ytec::windowsapp::IRebootHandoffService {
 public:
  ytec::clonecore::Status restart_to_advanced_boot_options() override {
    ++call_count;
    return ytec::clonecore::success_status();
  }

  int call_count{};
};

void test_reboot_handoff_requires_supported_elevated_windows() {
  const auto standard = ytec::windowsapp::build_reboot_handoff_plan(
      false, 10, 0);
  check(
      standard.readiness ==
          ytec::windowsapp::RebootHandoffReadiness::elevation_required,
      "Standard process should get guidance without automatic UAC");

  const auto legacy = ytec::windowsapp::build_reboot_handoff_plan(
      true, 6, 1);
  check(
      legacy.readiness ==
          ytec::windowsapp::RebootHandoffReadiness::unsupported_windows,
      "Windows 7 should use the firmware Boot Menu guidance");

  const auto ready = ytec::windowsapp::build_reboot_handoff_plan(
      true, 10, 0);
  check(
      ready.readiness ==
          ytec::windowsapp::RebootHandoffReadiness::ready,
      "Elevated Windows 10 should offer advanced startup");

  MockRebootHandoffService service;
  const auto blocked =
      ytec::windowsapp::request_reboot_handoff(standard, service);
  check(!blocked.has_value(), "Non-elevated plan must fail closed");
  check(service.call_count == 0, "Blocked plan must not request a reboot");

  const auto requested =
      ytec::windowsapp::request_reboot_handoff(ready, service);
  check(requested.has_value(), "Ready plan should reach the reboot backend");
  check(service.call_count == 1, "Ready plan should request exactly once");
}

}  // namespace

int main() {
  try {
    test_units_and_duration();
    test_eta_is_hidden_until_stable();
    test_eta_and_progress_are_bounded();
    test_terminal_labels();
    test_online_image_progress_keeps_work_streams_distinct();
    test_clone_selection_safety();
    test_confirmed_clone_job_is_hashed_and_target_bound();
    test_confirmed_mbr_to_gpt_job_is_manual_and_layout_bound();
    test_confirmed_restore_job_is_hashed_and_target_bound();
    test_reboot_handoff_requires_supported_elevated_windows();
    std::cout << "windows app progress tests: PASS\n";
    return 0;
  } catch (const TestFailure& failure) {
    std::cerr << "windows app progress tests: FAIL: "
              << failure.message << '\n';
    return 1;
  }
}
