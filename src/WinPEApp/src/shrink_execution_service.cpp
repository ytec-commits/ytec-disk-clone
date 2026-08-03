#include "ytec/winpeapp/app_runner.h"
#include "boot_finalization.h"

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/diskmodel/physical_disk.h"
#include "ytec/migrationengine/bundle_capture.h"
#include "ytec/migrationengine/shrink_bundle.h"
#include "ytec/migrationengine/source_analysis.h"
#include "ytec/migrationengine/target_layout.h"
#include "ytec/migrationengine/target_layout_io.h"
#include "ytec/migrationengine/volume_apply.h"
#include "ytec/winpeapp/clone_execution_readiness.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ytec::winpeapp {
namespace {

constexpr std::uint32_t kMigrationProcessTimeoutMilliseconds =
    6U * 60U * 60U * 1000U;
constexpr std::uint32_t kVolumeArrivalAttempts = 120U;
constexpr DWORD kVolumeArrivalDelayMilliseconds = 250U;

clonecore::Error shrink_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

template <typename T>
clonecore::Result<T> failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Result<T>::failure(shrink_error(
      code, native_code, std::move(operation), std::move(message)));
}

std::string current_utc_timestamp() {
  SYSTEMTIME time{};
  GetSystemTime(&time);
  std::array<char, 32> text{};
  const int length = std::snprintf(
      text.data(),
      text.size(),
      "%04u-%02u-%02uT%02u:%02u:%02uZ",
      time.wYear,
      time.wMonth,
      time.wDay,
      time.wHour,
      time.wMinute,
      time.wSecond);
  return length > 0 && static_cast<std::size_t>(length) < text.size()
      ? std::string(text.data(), static_cast<std::size_t>(length))
      : std::string{};
}

clonecore::Result<std::wstring> trusted_system_directory() {
  std::vector<wchar_t> path(32U * 1024U, L'\0');
  const UINT length =
      GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
  if (length == 0U || length >= path.size()) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"縮小移行System32取得",
            GetLastError()));
  }
  return clonecore::Result<std::wstring>::success(
      std::wstring(path.data(), length));
}

clonecore::Status validate_system_scratch_directory(
    const std::wstring& scratch_directory) {
  const auto system = trusted_system_directory();
  if (!system) {
    return clonecore::Status::failure(system.error());
  }
  const std::filesystem::path scratch(scratch_directory);
  const std::filesystem::path system_path(system.value());
  if (!scratch.is_absolute() || scratch.root_name().empty() ||
      scratch.root_directory().empty() ||
      _wcsicmp(
          scratch.root_name().c_str(), system_path.root_name().c_str()) != 0) {
    return clonecore::Status::failure(shrink_error(
        clonecore::ErrorCode::access_denied,
        ERROR_INVALID_DRIVE,
        L"縮小移行作業フォルダー物理分離",
        L"作業フォルダーは実行中Windows/WinPEのシステムドライブ上に限定します"));
  }
  std::filesystem::path current = scratch.root_path();
  for (const auto& component : scratch.relative_path()) {
    if (component == L".") {
      continue;
    }
    if (component == L"..") {
      return clonecore::Status::failure(shrink_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_NAME,
          L"縮小移行作業フォルダー検証",
          L"親参照を含む作業フォルダーは使用できません"));
    }
    current /= component;
    const DWORD attributes = GetFileAttributesW(current.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return clonecore::Status::failure(shrink_error(
          clonecore::ErrorCode::unsupported_layout,
          attributes == INVALID_FILE_ATTRIBUTES ? GetLastError()
                                                 : ERROR_REPARSE_TAG_INVALID,
          L"縮小移行作業フォルダー検証",
          L"存在する通常フォルダーだけを作業フォルダーに使用できます"));
    }
  }
  return clonecore::success_status();
}

clonecore::Status validate_third_disk_bundle_path(
    const std::wstring& path,
    const std::uint32_t source_disk_number,
    const std::uint32_t target_disk_number) {
  const std::filesystem::path bundle(path);
  if (_wcsicmp(bundle.extension().c_str(), L".dcmig") != 0 ||
      GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    return clonecore::Status::failure(shrink_error(
        clonecore::ErrorCode::access_denied,
        ERROR_FILE_EXISTS,
        L"縮小移行作業イメージ保存先",
        L"未使用の.dcmig名を指定してください。既存項目は上書きしません"));
  }
  const auto disk_number =
      diskmodel::query_single_disk_number_for_local_path(path);
  if (!disk_number) {
    return clonecore::Status::failure(disk_number.error());
  }
  if (disk_number.value() == source_disk_number ||
      disk_number.value() == target_disk_number) {
    return clonecore::Status::failure(shrink_error(
        clonecore::ErrorCode::access_denied,
        ERROR_INVALID_DRIVE,
        L"縮小移行作業イメージの物理分離",
        L"原本とコピー先を保護するため、作業イメージは第三の物理ディスクへ保存してください"));
  }
  return clonecore::success_status();
}

clonecore::Result<std::vector<std::uint32_t>>
collect_connected_mbr_signatures() {
  auto inventory = diskmodel::make_windows_disk_inventory_provider();
  const auto report = inventory->enumerate();
  if (!report) {
    return clonecore::Result<std::vector<std::uint32_t>>::failure(
        report.error());
  }
  if (!report.value().issues.empty()) {
    return failure<std::vector<std::uint32_t>>(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"縮小移行MBR署名衝突検査",
        L"全ディスク列挙に未解決の診断があるため一意なMBR署名を作れません");
  }
  std::vector<std::uint32_t> signatures;
  for (const auto& disk : report.value().disks) {
    if (disk.partition_style != diskmodel::PartitionStyle::mbr) {
      continue;
    }
    auto identity =
        diskmodel::make_stable_disk_identity(disk, disk.is_system_disk);
    if (!identity) {
      return clonecore::Result<std::vector<std::uint32_t>>::failure(
          identity.error());
    }
    auto opened =
        diskmodel::open_verified_read_only_physical_disk_with_windows_apis(
            identity.value());
    if (!opened) {
      return clonecore::Result<std::vector<std::uint32_t>>::failure(
          opened.error());
    }
    const auto parsed = clonecore::parse_mbr(*opened.value().reader);
    if (!parsed) {
      return clonecore::Result<std::vector<std::uint32_t>>::failure(
          parsed.error());
    }
    signatures.push_back(parsed.value().disk_signature);
  }
  std::sort(signatures.begin(), signatures.end());
  signatures.erase(
      std::unique(signatures.begin(), signatures.end()), signatures.end());
  return clonecore::Result<std::vector<std::uint32_t>>::success(
      std::move(signatures));
}

clonecore::Result<migrationengine::ShrinkSourceAnalysis> analyze_source(
    const CloneExecutionRequest& request,
    diskmodel::ReadOnlyPhysicalDiskHandle& source) {
  clonecore::StableDiskIdentity semantic_identity =
      source.observed.identity;
  semantic_identity.is_system_disk = request.expected_source.is_system_disk;
  const migrationengine::ShrinkSourceAnalysisContext context{
      .source_identity = std::move(semantic_identity),
      .physical_sector_size = source.observed.observed.physical_sector_size,
      .created_utc = current_utc_timestamp(),
      .app_version = YTEC_PROJECT_VERSION,
  };
  if (context.created_utc.empty()) {
    return failure<migrationengine::ShrinkSourceAnalysis>(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"縮小移行作成日時",
        L"UTC作成日時を生成できません");
  }
  if (source.observed.observed.partition_style ==
      diskmodel::PartitionStyle::gpt) {
    const auto layout = clonecore::parse_gpt(*source.reader);
    if (!layout) {
      return clonecore::Result<migrationengine::ShrinkSourceAnalysis>::failure(
          layout.error());
    }
    return migrationengine::analyze_gpt_shrink_source_with_windows_apis(
        source.observed.observed, *source.reader, layout.value(), context);
  }
  if (source.observed.observed.partition_style ==
      diskmodel::PartitionStyle::mbr) {
    const auto layout = clonecore::parse_mbr(*source.reader);
    if (!layout) {
      return clonecore::Result<migrationengine::ShrinkSourceAnalysis>::failure(
          layout.error());
    }
    return migrationengine::analyze_mbr_shrink_source_with_windows_apis(
        source.observed.observed, *source.reader, layout.value(), context);
  }
  return failure<migrationengine::ShrinkSourceAnalysis>(
      clonecore::ErrorCode::unsupported_layout,
      ERROR_NOT_SUPPORTED,
      L"縮小移行コピー元形式",
      L"基本GPTまたはMBRディスクだけに対応します");
}

clonecore::Result<diskmodel::ReidentifiedPhysicalTarget>
wait_for_online_target(
    const clonecore::StableDiskIdentity& expected,
    const clonecore::TargetConfirmation& confirmation,
    const clonecore::DiskOperationCallbacks& callbacks) {
  std::optional<clonecore::Error> last_error;
  for (std::uint32_t attempt = 0; attempt < kVolumeArrivalAttempts; ++attempt) {
    auto inventory = diskmodel::make_windows_disk_inventory_provider();
    auto observed = diskmodel::reidentify_physical_target(
        expected, confirmation, *inventory);
    if (observed && !observed.value().target.offline.value_or(true)) {
      return observed;
    }
    if (!observed) {
      last_error = observed.error();
    }
    if (clonecore::disk_operation_cancellation_requested(callbacks)) {
      return failure<diskmodel::ReidentifiedPhysicalTarget>(
          clonecore::ErrorCode::cancelled,
          ERROR_CANCELLED,
          L"縮小移行コピー先Volume待機",
          L"新規Volumeの到着待ち中に取り消されました");
    }
    Sleep(kVolumeArrivalDelayMilliseconds);
  }
  return last_error.has_value()
      ? clonecore::Result<diskmodel::ReidentifiedPhysicalTarget>::failure(
            std::move(*last_error))
      : failure<diskmodel::ReidentifiedPhysicalTarget>(
            clonecore::ErrorCode::query_failed,
            ERROR_TIMEOUT,
            L"縮小移行コピー先Volume待機",
            L"新規パーティションを30秒以内に再識別できませんでした");
}

std::optional<std::uint64_t> windows_partition_offset(
    const migrationengine::ShrinkTargetLayout& layout) {
  const auto partition = std::find_if(
      layout.migration.target_partitions.begin(),
      layout.migration.target_partitions.end(),
      [](const auto& value) {
        return value.role == migrationcore::MigrationPartitionRole::windows;
      });
  return partition == layout.migration.target_partitions.end()
      ? std::nullopt
      : std::optional<std::uint64_t>(partition->offset_bytes);
}

clonecore::Result<migrationengine::ShrinkTargetLayout> build_layout(
    const imageformat::ShrinkImageManifest& manifest,
    const clonecore::StableDiskIdentity& target) {
  auto signatures = collect_connected_mbr_signatures();
  if (!signatures) {
    return clonecore::Result<migrationengine::ShrinkTargetLayout>::failure(
        signatures.error());
  }
  auto guid_generator = clonecore::make_windows_guid_generator();
  auto signature_generator = clonecore::make_windows_mbr_signature_generator();
  return migrationengine::build_shrink_target_layout(
      manifest,
      target.size_bytes,
      target.logical_sector_size,
      *guid_generator,
      *signature_generator,
      signatures.value());
}

clonecore::Result<migrationengine::ShrinkVolumeApplyReport>
apply_layout_volumes(
    const diskmodel::DiskInfo& observed_target,
    const migrationengine::ShrinkTargetLayout& layout,
    const migrationengine::VerifiedShrinkBundle& bundle,
    const std::wstring& scratch,
    const clonecore::DiskOperationCallbacks& callbacks) {
  auto system = trusted_system_directory();
  if (!system) {
    return clonecore::Result<migrationengine::ShrinkVolumeApplyReport>::failure(
        system.error());
  }
  auto trust = bootrepair::make_windows_authenticode_verifier();
  auto process = bootrepair::make_windows_process_runner(
      kMigrationProcessTimeoutMilliseconds);
  return migrationengine::format_and_apply_shrink_volumes(
      observed_target,
      layout,
      bundle,
      scratch,
      system.value(),
      *trust,
      *process,
      callbacks);
}

clonecore::Status validate_raw_restore_target(
    const diskmodel::DiskInfo& target) {
  if (target.is_system_disk || target.partition_style !=
          diskmodel::PartitionStyle::raw ||
      !target.partitions.empty() || !target.offline.has_value() ||
      !target.read_only.has_value() || !target.removable.has_value() ||
      target.read_only.value() || target.removable.value() ||
      target.logical_sector_size != 512U || target.size_bytes == 0U) {
    return clonecore::Status::failure(shrink_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"縮小イメージ復元先",
        L"書込み可能な固定512バイト論理セクターの空RAWディスクだけを復元先にできます"));
  }
  return clonecore::success_status();
}

clonecore::Result<ProductBootFinalizationReport> finalize_if_required(
    const bool required,
    const clonecore::StableDiskIdentity& target,
    const migrationengine::ShrinkTargetLayout& layout,
    const clonecore::DiskOperationCallbacks& callbacks) {
  if (!required) {
    return clonecore::Result<ProductBootFinalizationReport>::success(
        ProductBootFinalizationReport{
            .temporary_mounts_released = true,
            .final_target_verified = true,
        });
  }
  auto inventory = diskmodel::make_windows_disk_inventory_provider();
  return finalize_product_target_boot(
      *inventory,
      target,
      layout.is_gpt ? diskmodel::PartitionStyle::gpt
                    : diskmodel::PartitionStyle::mbr,
      windows_partition_offset(layout),
      callbacks);
}

class WindowsShrinkCloneExecutionService final
    : public ICloneExecutionService {
 public:
  clonecore::Result<CloneExecutionReport> execute(
      const CloneExecutionRequest& request) override {
    if (!request.authorization.empty() ||
        request.transfer_mode != imageformat::TransferMode::shrink ||
        request.shrink_bundle_directory.empty() ||
        request.scratch_directory.empty()) {
      return failure<CloneExecutionReport>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"縮小移行クローン要求",
          L"縮小移行モード、作業イメージ、または作業フォルダー指定が不正です");
    }
    auto inventory = diskmodel::make_windows_disk_inventory_provider();
    auto selected = diskmodel::reidentify_physical_clone(
        request.expected_source,
        request.expected_target,
        request.confirmation,
        *inventory,
        false);
    if (!selected) {
      return clonecore::Result<CloneExecutionReport>::failure(
          selected.error());
    }
    const auto ready = validate_clone_execution_observation(
        selected.value().source, selected.value().target, false);
    if (!ready) {
      return clonecore::Result<CloneExecutionReport>::failure(ready.error());
    }
    const auto separate = validate_third_disk_bundle_path(
        request.shrink_bundle_directory,
        selected.value().source.disk_number,
        selected.value().target.disk_number);
    if (!separate) {
      return clonecore::Result<CloneExecutionReport>::failure(
          separate.error());
    }
    auto source =
        diskmodel::open_verified_read_only_physical_disk_with_windows_apis(
            request.expected_source);
    if (!source) {
      return clonecore::Result<CloneExecutionReport>::failure(source.error());
    }
    auto analyzed = analyze_source(request, source.value());
    if (!analyzed) {
      return clonecore::Result<CloneExecutionReport>::failure(analyzed.error());
    }
    std::vector<migrationengine::ShrinkCaptureSource> capture_sources;
    capture_sources.reserve(analyzed.value().content_volumes.size());
    for (const auto& volume : analyzed.value().content_volumes) {
      capture_sources.push_back(migrationengine::ShrinkCaptureSource{
          .source_table_index = volume.source_table_index,
          .capture_root = volume.volume_guid_path,
      });
    }
    const auto scratch_safe =
        validate_system_scratch_directory(request.scratch_directory);
    if (!scratch_safe) {
      return clonecore::Result<CloneExecutionReport>::failure(
          scratch_safe.error());
    }
    const auto still_separate = validate_third_disk_bundle_path(
        request.shrink_bundle_directory,
        selected.value().source.disk_number,
        selected.value().target.disk_number);
    if (!still_separate) {
      return clonecore::Result<CloneExecutionReport>::failure(
          still_separate.error());
    }
    auto captured = migrationengine::capture_shrink_bundle_with_windows_apis(
        migrationengine::ShrinkBundleCaptureRequest{
            .analysis = analyzed.value(),
            .capture_sources = std::move(capture_sources),
            .final_bundle_directory = request.shrink_bundle_directory,
            .scratch_directory = request.scratch_directory,
            .callbacks = request.callbacks,
        });
    if (!captured) {
      return clonecore::Result<CloneExecutionReport>::failure(captured.error());
    }
    const std::wstring manifest_path =
        (std::filesystem::path(request.shrink_bundle_directory) /
         migrationengine::kShrinkBundleManifestFileName)
            .wstring();
    auto bundle = migrationengine::verify_shrink_bundle_read_only(manifest_path);
    if (!bundle ||
        bundle.value().manifest_sha256 != captured.value().manifest_sha256) {
      return clonecore::Result<CloneExecutionReport>::failure(
          bundle ? shrink_error(
                       clonecore::ErrorCode::verification_failed,
                       ERROR_CRC,
                       L"縮小移行作業イメージ再検証",
                       L"完成後のマニフェストSHA-256が変化しました")
                 : bundle.error());
    }
    auto layout = build_layout(bundle.value().manifest, request.expected_target);
    if (!layout) {
      return clonecore::Result<CloneExecutionReport>::failure(layout.error());
    }
    const auto scratch_still_safe =
        validate_system_scratch_directory(request.scratch_directory);
    if (!scratch_still_safe) {
      return clonecore::Result<CloneExecutionReport>::failure(
          scratch_still_safe.error());
    }
    const auto offline =
        diskmodel::set_verified_physical_target_offline_with_windows_apis(
            request.expected_target, request.confirmation, true);
    if (!offline) {
      return clonecore::Result<CloneExecutionReport>::failure(offline.error());
    }
    migrationengine::ShrinkTargetMetadataWriteReport metadata;
    {
      auto target =
          diskmodel::open_verified_physical_target_with_windows_apis(
              request.expected_target, request.confirmation);
      if (!target) {
        return clonecore::Result<CloneExecutionReport>::failure(target.error());
      }
      auto written = migrationengine::write_shrink_target_metadata(
          layout.value(), *target.value().target, request.callbacks);
      if (!written) {
        return clonecore::Result<CloneExecutionReport>::failure(written.error());
      }
      metadata = written.take_value();
    }
    const auto online =
        diskmodel::set_verified_physical_target_offline_with_windows_apis(
            request.expected_target, request.confirmation, false);
    if (!online) {
      return clonecore::Result<CloneExecutionReport>::failure(online.error());
    }
    auto observed = wait_for_online_target(
        request.expected_target, request.confirmation, request.callbacks);
    if (!observed) {
      (void)diskmodel::set_verified_physical_target_offline_with_windows_apis(
          request.expected_target, request.confirmation, true);
      return clonecore::Result<CloneExecutionReport>::failure(observed.error());
    }
    auto applied = apply_layout_volumes(
        observed.value().target,
        layout.value(),
        bundle.value(),
        request.scratch_directory,
        request.callbacks);
    if (!applied) {
      (void)diskmodel::set_verified_physical_target_offline_with_windows_apis(
          request.expected_target, request.confirmation, true);
      return clonecore::Result<CloneExecutionReport>::failure(applied.error());
    }
    auto finalized = finalize_if_required(
        bundle.value().manifest.source.is_system_disk,
        request.expected_target,
        layout.value(),
        request.callbacks);
    if (!finalized) {
      (void)diskmodel::set_verified_physical_target_offline_with_windows_apis(
          request.expected_target, request.confirmation, true);
      return clonecore::Result<CloneExecutionReport>::failure(finalized.error());
    }
    const auto& boot = finalized.value();
    return clonecore::Result<CloneExecutionReport>::success(
        CloneExecutionReport{
            .partition_style = layout.value().is_gpt
                ? ClonePartitionStyle::gpt
                : ClonePartitionStyle::mbr,
            .copied_data_bytes = applied.value().applied_payload_bytes,
            .copied_partition_count = applied.value().applied_wim_count,
            .recreated_partition_count =
                applied.value().formatted_volume_count -
                applied.value().applied_wim_count,
            .read_back_verified =
                metadata.every_write_read_back_verified,
            .partition_table_committed = metadata.partition_table_committed,
            .target_returned_online = true,
            .boot_finalization_required =
                bundle.value().manifest.source.is_system_disk,
            .boot_repair = boot.boot_repair,
            .windows_partition_temporarily_mounted =
                boot.windows_partition_temporarily_mounted,
            .system_partition_temporarily_mounted =
                boot.system_partition_temporarily_mounted,
            .temporary_mounts_released =
                applied.value().every_temporary_mount_released &&
                boot.temporary_mounts_released,
            .boot_finalization_verified = boot.final_target_verified,
        });
  }
};

class WindowsShrinkRestoreExecutionService final
    : public IRestoreExecutionService {
 public:
  clonecore::Result<RestoreExecutionReport> execute(
      const RestoreExecutionRequest& request) override {
    if (request.transfer_mode != imageformat::TransferMode::shrink ||
        request.verified_image_path.empty() || request.scratch_directory.empty()) {
      return failure<RestoreExecutionReport>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"縮小イメージ復元要求",
          L"縮小移行モード、マニフェスト、または作業フォルダー指定が不正です");
    }
    auto bundle = migrationengine::verify_shrink_bundle_read_only(
        request.verified_image_path);
    if (!bundle) {
      return clonecore::Result<RestoreExecutionReport>::failure(bundle.error());
    }
    if (bundle.value().manifest_length_bytes !=
            request.expected_image.length_bytes ||
        bundle.value().manifest_sha256 != request.expected_image.global_hash) {
      return failure<RestoreExecutionReport>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"縮小イメージ実行直前再識別",
          L"ロックしたmanifest.dcmigの長さまたはSHA-256が承認済みジョブと一致しません");
    }
    auto inventory = diskmodel::make_windows_disk_inventory_provider();
    auto target = diskmodel::reidentify_physical_target(
        request.expected_target, request.confirmation, *inventory);
    if (!target) {
      return clonecore::Result<RestoreExecutionReport>::failure(target.error());
    }
    const auto ready = validate_raw_restore_target(target.value().target);
    if (!ready) {
      return clonecore::Result<RestoreExecutionReport>::failure(ready.error());
    }
    auto layout = build_layout(bundle.value().manifest, request.expected_target);
    if (!layout) {
      return clonecore::Result<RestoreExecutionReport>::failure(layout.error());
    }
    const auto scratch_safe =
        validate_system_scratch_directory(request.scratch_directory);
    if (!scratch_safe) {
      return clonecore::Result<RestoreExecutionReport>::failure(
          scratch_safe.error());
    }
    const auto offline =
        diskmodel::set_verified_physical_target_offline_with_windows_apis(
            request.expected_target, request.confirmation, true);
    if (!offline) {
      return clonecore::Result<RestoreExecutionReport>::failure(offline.error());
    }
    migrationengine::ShrinkTargetMetadataWriteReport metadata;
    {
      auto opened =
          diskmodel::open_verified_physical_target_with_windows_apis(
              request.expected_target, request.confirmation);
      if (!opened) {
        return clonecore::Result<RestoreExecutionReport>::failure(opened.error());
      }
      auto written = migrationengine::write_shrink_target_metadata(
          layout.value(), *opened.value().target, request.callbacks);
      if (!written) {
        return clonecore::Result<RestoreExecutionReport>::failure(written.error());
      }
      metadata = written.take_value();
    }
    const auto online =
        diskmodel::set_verified_physical_target_offline_with_windows_apis(
            request.expected_target, request.confirmation, false);
    if (!online) {
      return clonecore::Result<RestoreExecutionReport>::failure(online.error());
    }
    auto observed = wait_for_online_target(
        request.expected_target, request.confirmation, request.callbacks);
    if (!observed) {
      (void)diskmodel::set_verified_physical_target_offline_with_windows_apis(
          request.expected_target, request.confirmation, true);
      return clonecore::Result<RestoreExecutionReport>::failure(observed.error());
    }
    auto applied = apply_layout_volumes(
        observed.value().target,
        layout.value(),
        bundle.value(),
        request.scratch_directory,
        request.callbacks);
    if (!applied) {
      (void)diskmodel::set_verified_physical_target_offline_with_windows_apis(
          request.expected_target, request.confirmation, true);
      return clonecore::Result<RestoreExecutionReport>::failure(applied.error());
    }
    auto finalized = finalize_if_required(
        bundle.value().manifest.source.is_system_disk,
        request.expected_target,
        layout.value(),
        request.callbacks);
    if (!finalized) {
      (void)diskmodel::set_verified_physical_target_offline_with_windows_apis(
          request.expected_target, request.confirmation, true);
      return clonecore::Result<RestoreExecutionReport>::failure(finalized.error());
    }
    const auto& boot = finalized.value();
    return clonecore::Result<RestoreExecutionReport>::success(
        RestoreExecutionReport{
            .restored_data_bytes = applied.value().applied_payload_bytes,
            .committed_partition_table_bytes = metadata.metadata_bytes,
            .restored_chunk_count = applied.value().applied_wim_count,
            .complete_image_verified_before_write = true,
            .backup_manifest_verified_before_write = true,
            .read_back_verified = metadata.every_write_read_back_verified,
            .partition_table_committed = metadata.partition_table_committed,
            .target_returned_online = true,
            .boot_finalization_required =
                bundle.value().manifest.source.is_system_disk,
            .boot_repair = boot.boot_repair,
            .windows_partition_temporarily_mounted =
                boot.windows_partition_temporarily_mounted,
            .system_partition_temporarily_mounted =
                boot.system_partition_temporarily_mounted,
            .temporary_mounts_released =
                applied.value().every_temporary_mount_released &&
                boot.temporary_mounts_released,
            .boot_finalization_verified = boot.final_target_verified,
        });
  }
};

}  // namespace

std::unique_ptr<ICloneExecutionService>
make_windows_shrink_clone_job_execution_service() {
  return std::make_unique<WindowsShrinkCloneExecutionService>();
}

std::unique_ptr<IRestoreExecutionService>
make_windows_shrink_restore_execution_service() {
  return std::make_unique<WindowsShrinkRestoreExecutionService>();
}

}  // namespace ytec::winpeapp
