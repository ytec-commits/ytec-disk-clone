#include "vm_clone_execution_service.h"

#include "ytec/clonecore/operation_progress.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/winpeapp/app_runner.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef YTEC_VM_DESTRUCTIVE_TEST_ONLY
#error This executable must never be built as a product target.
#endif

namespace {

constexpr std::wstring_view kAuthorization =
    L"YTEC-VM-ONLY-PRODUCT-JOB-CANCELLATION";
constexpr std::wstring_view kCloneProfile = L"clone";
constexpr std::wstring_view kRestoreProfile = L"restore";
constexpr std::wstring_view kCancellationMode = L"cancellation";
constexpr std::wstring_view kSuccessMode = L"success";
constexpr std::wstring_view kCloneJobPath =
    L"X:\\TsumugiValidation\\clone-job.json";
constexpr std::wstring_view kRestoreJobPath =
    L"X:\\TsumugiValidation\\restore-job.json";
constexpr std::uint32_t kRestoreTargetDiskNumber = 0;
constexpr std::uint64_t kRestoreTargetBytes = 8ULL * 1024ULL * 1024ULL;

bool fixed_restore_target_is_safe(
    ytec::diskmodel::IDiskInventoryProvider& provider) {
  const auto report = provider.enumerate();
  if (!report || !report.value().issues.empty() ||
      report.value().disks.size() != 1U) {
    return false;
  }
  const auto& target = report.value().disks.front();
  return target.disk_number == kRestoreTargetDiskNumber &&
      target.size_bytes == kRestoreTargetBytes &&
      target.logical_sector_size == 512U &&
      target.partition_style == ytec::diskmodel::PartitionStyle::raw &&
      target.partitions.empty() && !target.is_system_disk &&
      target.offline.has_value() && !target.offline.value() &&
      target.read_only.has_value() && !target.read_only.value() &&
      target.removable.has_value() && !target.removable.value() &&
      _wcsicmp(target.model.c_str(), L"VBOX HARDDISK") == 0;
}

bool target_remains_offline_without_committed_table(
    ytec::diskmodel::IDiskInventoryProvider& provider,
    const std::uint32_t target_number) {
  const auto report = provider.enumerate();
  if (!report) {
    return false;
  }
  const auto target = std::find_if(
      report.value().disks.begin(),
      report.value().disks.end(),
      [target_number](const auto& disk) {
        return disk.disk_number == target_number;
      });
  return target != report.value().disks.end() &&
      target->offline.has_value() && target->offline.value() &&
      target->partition_style == ytec::diskmodel::PartitionStyle::raw &&
      target->partitions.empty();
}

bool target_is_online_with_committed_table(
    ytec::diskmodel::IDiskInventoryProvider& provider,
    const std::uint32_t target_number,
    const bool clone) {
  const auto report = provider.enumerate();
  if (!report || !report.value().issues.empty()) {
    return false;
  }
  const auto target = std::find_if(
      report.value().disks.begin(),
      report.value().disks.end(),
      [target_number](const auto& disk) {
        return disk.disk_number == target_number;
      });
  if (target == report.value().disks.end() || target->is_system_disk ||
      !target->offline.has_value() || target->offline.value() ||
      !target->read_only.has_value() || target->read_only.value() ||
      !target->removable.has_value() || target->removable.value() ||
      _wcsicmp(target->model.c_str(), L"VBOX HARDDISK") != 0) {
    return false;
  }
  return clone
      ? target->size_bytes == 3ULL * 1024ULL * 1024ULL * 1024ULL &&
            target->partition_style == ytec::diskmodel::PartitionStyle::gpt &&
            target->partitions.size() == 4U
      : target->size_bytes == kRestoreTargetBytes &&
            target->partition_style == ytec::diskmodel::PartitionStyle::mbr &&
            target->partitions.size() == 1U;
}

}  // namespace

int wmain(const int argc, wchar_t* argv[]) {
  SetConsoleOutputCP(CP_UTF8);
  if (argc != 9 || std::wstring_view(argv[1]) != L"--profile" ||
      std::wstring_view(argv[3]) != L"--mode" ||
      std::wstring_view(argv[5]) != L"--confirmation" ||
      std::wstring_view(argv[7]) != L"--authorization" ||
      std::wstring_view(argv[8]) != kAuthorization) {
    std::wcerr
        << L"VM専用の固定プロファイル、実行モード、確認語、許可語が必要です。\n";
    return 2;
  }
  const std::wstring_view profile(argv[2]);
  const std::wstring_view mode(argv[4]);
  const std::wstring confirmation(argv[6]);
  const bool success_mode = mode == kSuccessMode;
  if ((profile != kCloneProfile && profile != kRestoreProfile) ||
      (mode != kCancellationMode && mode != kSuccessMode) ||
      confirmation.empty()) {
    std::wcerr << L"固定プロファイル、実行モード、または確認語が不正です。\n";
    return 2;
  }
  if (!ytec::vmtest::is_virtualbox_guest() ||
      !ytec::vmtest::is_administrator()) {
    std::wcerr << L"VirtualBox上の管理者WinPEだけで実行できます。\n";
    return 3;
  }

  auto provider = ytec::diskmodel::make_windows_disk_inventory_provider();
  const bool clone = profile == kCloneProfile;
  std::uint32_t clone_target_disk_number = 0;
  if (clone) {
    const auto selection = ytec::vmtest::select_unique_vm_clone_target(
        0, ytec::vmtest::VmCloneProfile::synthetic);
    if (!selection) {
      std::wcerr << selection.error().operation << L": "
                 << selection.error().message << L"\n";
      return 4;
    }
    clone_target_disk_number =
        selection.value().target_disk.disk_number;
  } else if (!fixed_restore_target_is_safe(*provider)) {
    std::wcerr
        << L"8MiBオンラインRAWの固定VirtualBox復元先ではありません。\n";
    return 4;
  }

  std::atomic_bool cancellation_requested{false};
  std::uint32_t progress_events = 0;
  std::uint64_t verified_before_cancel = 0;
  std::uint64_t final_verified_bytes = 0;
  ytec::clonecore::DiskOperationStage final_stage =
      ytec::clonecore::DiskOperationStage::planning;
  const ytec::clonecore::DiskOperationCallbacks callbacks{
      .progress =
          [&](const ytec::clonecore::DiskOperationProgress& progress) {
            ++progress_events;
            final_verified_bytes = progress.verified_bytes;
            final_stage = progress.stage;
            if (!success_mode && progress.stage ==
                    ytec::clonecore::DiskOperationStage::copying_data &&
                progress.written_bytes >=
                    (clone ? 1024ULL * 1024ULL : 4096ULL)) {
              verified_before_cancel = progress.verified_bytes;
              cancellation_requested.store(true);
            }
          },
      .cancellation_requested =
          [&]() { return cancellation_requested.load(); },
  };

  auto loader = ytec::winpeapp::make_windows_job_manifest_loader();
  auto verifier = ytec::winpeapp::make_windows_restore_image_verifier();
  auto safety =
      ytec::winpeapp::make_windows_restore_execution_safety_probe();
  auto candidates =
      ytec::winpeapp::make_windows_restore_image_candidate_provider();
  auto restore = ytec::winpeapp::make_windows_restore_execution_service();
  auto product_clone =
      ytec::winpeapp::make_windows_clone_job_execution_service();
  std::ostringstream output;
  std::ostringstream error;
  const std::wstring job_path(
      clone ? kCloneJobPath : kRestoreJobPath);
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--job-execute",
       L"--job-path",
       job_path,
       L"--acknowledge-target-erasure",
       L"--confirmation",
       confirmation,
       L"--json"},
      *provider,
      output,
      error,
      nullptr,
      nullptr,
      loader.get(),
      verifier.get(),
      safety.get(),
      candidates.get(),
      &callbacks,
      restore.get(),
      product_clone.get(),
      nullptr);

  const std::uint32_t target_number =
      clone ? clone_target_disk_number : kRestoreTargetDiskNumber;
  if (success_mode) {
    const std::string expected_mode =
        clone ? "\"mode\":\"clone-execution\""
              : "\"mode\":\"restore-execution\"";
    const bool expected_success = exit_code == 0 &&
        !cancellation_requested.load() && progress_events > 0 &&
        final_verified_bytes > 0 &&
        final_stage == ytec::clonecore::DiskOperationStage::completed &&
        error.str().empty() &&
        output.str().find(expected_mode) != std::string::npos &&
        output.str().find("\"result\":\"PASS\"") != std::string::npos &&
        output.str().find("\"readBackVerified\":true") !=
            std::string::npos &&
        output.str().find("\"partitionTableCommitted\":true") !=
            std::string::npos &&
        output.str().find("\"targetReturnedOnline\":true") !=
            std::string::npos &&
        target_is_online_with_committed_table(
            *provider, target_number, clone);
    if (!expected_success) {
      std::cerr << "Unexpected product success result. exit="
                << exit_code << " progressEvents=" << progress_events
                << " finalVerifiedBytes=" << final_verified_bytes
                << " finalStage=" << static_cast<int>(final_stage)
                << "\nstdout:\n" << output.str()
                << "\nstderr:\n" << error.str();
      return 5;
    }
    std::cout
        << (clone ? "YDC_PRODUCT_CLONE_SUCCESS_PASS"
                  : "YDC_PRODUCT_RESTORE_SUCCESS_PASS")
        << " targetDisk=" << target_number
        << " progressEvents=" << progress_events
        << " verifiedBytes=" << final_verified_bytes
        << " targetOnline=true partitionTableCommitted=true\n";
    return 0;
  }

  const bool expected_failure = exit_code != 0 &&
      cancellation_requested.load() && progress_events > 0 &&
      verified_before_cancel > 0 &&
      error.str().find("Windows error 1223") != std::string::npos &&
      target_remains_offline_without_committed_table(
          *provider, target_number);
  if (!expected_failure) {
    std::cerr << "Unexpected product cancellation result. exit="
              << exit_code << " progressEvents=" << progress_events
              << " verifiedBeforeCancel=" << verified_before_cancel
              << "\nstdout:\n" << output.str()
              << "\nstderr:\n" << error.str();
    return 5;
  }

  std::cout
      << (clone ? "YDC_PRODUCT_CLONE_CANCEL_PASS"
                : "YDC_PRODUCT_RESTORE_CANCEL_PASS")
      << " targetDisk=" << target_number
      << " progressEvents=" << progress_events
      << " verifiedBeforeCancel=" << verified_before_cancel
      << " targetOffline=true partitionTableCommitted=false\n";
  return 0;
}
