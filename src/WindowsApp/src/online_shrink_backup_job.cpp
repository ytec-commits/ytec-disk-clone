#include "ytec/windowsapp/online_shrink_backup_job.h"

#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/diskmodel/physical_disk.h"
#include "ytec/migrationengine/shrink_bundle.h"
#include "ytec/migrationengine/source_analysis.h"
#include "ytec/vssrequester/workflow.h"

#include <Windows.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

clonecore::Error job_error(
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
  return clonecore::Result<T>::failure(job_error(
      code, native_code, std::move(operation), std::move(message)));
}

std::wstring pending_path_for(const std::wstring& final_path) {
  const std::filesystem::path path(final_path);
  return (path.parent_path() /
          (path.stem().wstring() + L".vss-pending-" +
           std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(GetTickCount64()) + L".dcmig"))
      .wstring();
}

void cleanup_pending_bundle(
    const std::wstring& directory,
    const migrationengine::ShrinkSourceAnalysis& analysis) noexcept {
  for (const auto& volume : analysis.content_volumes) {
    const std::filesystem::path file =
        std::filesystem::path(directory) / volume.payload_file_name;
    (void)DeleteFileW(file.c_str());
  }
  const std::filesystem::path manifest =
      std::filesystem::path(directory) /
      migrationengine::kShrinkBundleManifestFileName;
  (void)DeleteFileW(manifest.c_str());
  (void)RemoveDirectoryW(directory.c_str());
}

clonecore::Status validate_request(
    const OnlineShrinkBackupJobRequest& request) {
  if (!request.administrator) {
    return clonecore::Status::failure(job_error(
        clonecore::ErrorCode::access_denied,
        ERROR_ELEVATION_REQUIRED,
        L"オンライン縮小イメージ 管理者確認",
        L"オンラインイメージ作成には管理者権限が必要です"));
  }
  const std::filesystem::path final(request.final_bundle_directory);
  if (!final.is_absolute() ||
      _wcsicmp(final.extension().c_str(), L".dcmig") != 0 ||
      request.scratch_directory.empty() || request.created_utc.empty() ||
      request.app_version.empty() ||
      (request.selected_source.is_system_disk &&
       (request.windows_major != 10U || request.windows_build < 10240U ||
        request.windows_architecture != "AMD64"))) {
    return clonecore::Status::failure(job_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"オンライン縮小イメージ ジョブ情報",
        L"保存先、作業フォルダー、作成情報、またはWindows版が不正です"));
  }
  if (GetFileAttributesW(request.final_bundle_directory.c_str()) !=
      INVALID_FILE_ATTRIBUTES) {
    return clonecore::Status::failure(job_error(
        clonecore::ErrorCode::access_denied,
        ERROR_FILE_EXISTS,
        L"オンライン縮小イメージ 保存先",
        L"既存のファイルまたはフォルダーは上書きしません"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_destination_disk_separation(
    const OnlineShrinkBackupJobRequest& request,
    const std::uint32_t source_disk_number) {
  const auto destination_disk =
      diskmodel::query_single_disk_number_for_local_path(
          request.final_bundle_directory);
  const auto scratch_probe =
      (std::filesystem::path(request.scratch_directory) /
       L".ytec-shrink-destination-probe")
          .wstring();
  const auto scratch_disk =
      diskmodel::query_single_disk_number_for_local_path(scratch_probe);
  if (!destination_disk || !scratch_disk) {
    return clonecore::Status::failure(
        destination_disk ? scratch_disk.error() : destination_disk.error());
  }
  if (destination_disk.value() != scratch_disk.value() ||
      destination_disk.value() == source_disk_number) {
    return clonecore::Status::failure(job_error(
        clonecore::ErrorCode::access_denied,
        ERROR_INVALID_DRIVE,
        L"オンライン縮小イメージ 保存先物理分離",
        L"原本へ書き込まないため、保存先と作業フォルダーは原本とは別の単一物理ディスクにしてください"));
  }
  return clonecore::success_status();
}

}  // namespace

clonecore::Result<OnlineShrinkBackupJobReport>
execute_online_shrink_backup_job_with_windows_apis(
    const OnlineShrinkBackupJobRequest& request) {
  const auto valid = validate_request(request);
  if (!valid) {
    return clonecore::Result<OnlineShrinkBackupJobReport>::failure(
        valid.error());
  }
  auto expected = diskmodel::make_stable_disk_identity(
      request.selected_source, request.selected_source.is_system_disk);
  if (!expected) {
    return clonecore::Result<OnlineShrinkBackupJobReport>::failure(
        expected.error());
  }
  auto source =
      diskmodel::open_verified_read_only_physical_disk_with_windows_apis(
          expected.value());
  if (!source) {
    return clonecore::Result<OnlineShrinkBackupJobReport>::failure(
        source.error());
  }
  const auto destination_separated = validate_destination_disk_separation(
      request, source.value().observed.observed.disk_number);
  if (!destination_separated) {
    return clonecore::Result<OnlineShrinkBackupJobReport>::failure(
        destination_separated.error());
  }
  const migrationengine::ShrinkSourceAnalysisContext context{
      .source_identity = source.value().observed.identity,
      .physical_sector_size =
          source.value().observed.observed.physical_sector_size,
      .created_utc = request.created_utc,
      .app_version = request.app_version,
      .known_windows_version = request.selected_source.is_system_disk
          ? std::optional<migrationengine::WindowsSourceVersion>(
                migrationengine::WindowsSourceVersion{
                    .major = request.windows_major,
                    .minor = request.windows_minor,
                    .build = request.windows_build,
                    .architecture = request.windows_architecture,
                })
          : std::nullopt,
  };
  clonecore::Result<migrationengine::ShrinkSourceAnalysis> analyzed =
      failure<migrationengine::ShrinkSourceAnalysis>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"オンライン縮小イメージ パーティション形式",
          L"GPTまたはMBRの基本ディスクだけに対応します");
  if (source.value().observed.observed.partition_style ==
      diskmodel::PartitionStyle::gpt) {
    const auto layout = clonecore::parse_gpt(*source.value().reader);
    if (!layout) {
      return clonecore::Result<OnlineShrinkBackupJobReport>::failure(
          layout.error());
    }
    analyzed = migrationengine::analyze_gpt_shrink_source_with_windows_apis(
        source.value().observed.observed,
        *source.value().reader,
        layout.value(),
        context);
  } else if (source.value().observed.observed.partition_style ==
             diskmodel::PartitionStyle::mbr) {
    const auto layout = clonecore::parse_mbr(*source.value().reader);
    if (!layout) {
      return clonecore::Result<OnlineShrinkBackupJobReport>::failure(
          layout.error());
    }
    analyzed = migrationengine::analyze_mbr_shrink_source_with_windows_apis(
        source.value().observed.observed,
        *source.value().reader,
        layout.value(),
        context);
  }
  if (!analyzed) {
    return clonecore::Result<OnlineShrinkBackupJobReport>::failure(
        analyzed.error());
  }
  auto analysis = analyzed.take_value();
  vssrequester::WorkflowRequest workflow{
      .administrator = true,
  };
  workflow.volumes.reserve(analysis.content_volumes.size());
  for (const auto& volume : analysis.content_volumes) {
    workflow.volumes.push_back(vssrequester::VolumeRequest{
        .volume_guid_path = volume.volume_guid_path,
        .file_system = L"NTFS",
    });
  }

  const std::wstring pending = pending_path_for(request.final_bundle_directory);
  std::optional<migrationengine::ShrinkBundleCaptureReport> captured;
  vssrequester::WindowsVssBackend backend(
      vssrequester::WindowsVssBackendOptions{
          .async_wait = request.async_wait,
          .copy_snapshot_data =
              [&](const std::vector<std::wstring>& snapshots) {
                const auto separated = validate_destination_disk_separation(
                    request, source.value().observed.observed.disk_number);
                if (!separated) {
                  return separated;
                }
                if (snapshots.size() != analysis.content_volumes.size()) {
                  return clonecore::Status::failure(job_error(
                      clonecore::ErrorCode::identity_mismatch,
                      ERROR_INVALID_DATA,
                      L"オンライン縮小イメージ Snapshot対応",
                      L"VSS Snapshot件数が解析済みボリューム件数と一致しません"));
                }
                std::vector<migrationengine::ShrinkCaptureSource> sources;
                sources.reserve(snapshots.size());
                for (std::size_t index = 0; index < snapshots.size(); ++index) {
                  sources.push_back(migrationengine::ShrinkCaptureSource{
                      .source_table_index =
                          analysis.content_volumes[index].source_table_index,
                      .capture_root = snapshots[index],
                  });
                }
                auto result =
                    migrationengine::capture_shrink_bundle_with_windows_apis(
                        migrationengine::ShrinkBundleCaptureRequest{
                            .analysis = analysis,
                            .capture_sources = std::move(sources),
                            .final_bundle_directory = pending,
                            .scratch_directory = request.scratch_directory,
                            .callbacks = request.callbacks,
                        });
                if (!result) {
                  return clonecore::Status::failure(result.error());
                }
                captured = result.take_value();
                return clonecore::success_status();
              },
          .logger = request.logger,
      });
  auto completed = vssrequester::execute_backup_workflow(workflow, backend);
  if (!completed) {
    cleanup_pending_bundle(pending, analysis);
    return clonecore::Result<OnlineShrinkBackupJobReport>::failure(
        completed.error());
  }
  if (!captured.has_value() || !captured->committed_after_complete_verification) {
    cleanup_pending_bundle(pending, analysis);
    return failure<OnlineShrinkBackupJobReport>(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"オンライン縮小イメージ 完成確認",
        L"VSS処理は完了しましたが検証済みイメージ束がありません");
  }
  const auto destination_still_separated =
      validate_destination_disk_separation(
          request, source.value().observed.observed.disk_number);
  if (!destination_still_separated) {
    cleanup_pending_bundle(pending, analysis);
    return clonecore::Result<OnlineShrinkBackupJobReport>::failure(
        destination_still_separated.error());
  }
  if (!MoveFileExW(
          pending.c_str(),
          request.final_bundle_directory.c_str(),
          MOVEFILE_WRITE_THROUGH)) {
    const auto error = clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"オンライン縮小イメージ 完成名確定",
        GetLastError());
    cleanup_pending_bundle(pending, analysis);
    return clonecore::Result<OnlineShrinkBackupJobReport>::failure(error);
  }
  const std::wstring final_manifest =
      (std::filesystem::path(request.final_bundle_directory) /
       migrationengine::kShrinkBundleManifestFileName)
          .wstring();
  const auto verified =
      migrationengine::verify_shrink_bundle_read_only(final_manifest);
  if (!verified ||
      verified.value().manifest_sha256 != captured->manifest_sha256) {
    return clonecore::Result<OnlineShrinkBackupJobReport>::failure(
        verified
            ? job_error(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"オンライン縮小イメージ 完成名再検証",
                  L"VSS完了後にマニフェストSHA-256が変化しました")
            : verified.error());
  }
  captured->bundle_directory = request.final_bundle_directory;
  captured->manifest_path = final_manifest;
  return clonecore::Result<OnlineShrinkBackupJobReport>::success(
      OnlineShrinkBackupJobReport{
          .workflow = completed.take_value(),
          .bundle = std::move(*captured),
          .final_bundle_committed_after_vss_cleanup = true,
      });
}

}  // namespace ytec::windowsapp
