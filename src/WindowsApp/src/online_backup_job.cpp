#include "ytec/windowsapp/online_backup_job.h"

#include "ytec/vssrequester/snapshot_metadata.h"

#include <Windows.h>

#include <utility>

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

clonecore::Status validate_request(
    const OnlineBackupJobRequest& request,
    const OnlineBackupJobDependencies& dependencies) {
  if (!request.administrator) {
    return clonecore::Status::failure(job_error(
        clonecore::ErrorCode::access_denied,
        ERROR_ELEVATION_REQUIRED,
        L"オンラインイメージ 管理者確認",
        L"オンラインイメージ作成には管理者権限が必要です。"
        L"この処理からUAC昇格は要求しません"));
  }
  if (request.final_path.empty() || request.created_utc.empty() ||
      request.app_version.empty() ||
      request.windows_architecture.empty()) {
    return clonecore::Status::failure(job_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"オンラインイメージ ジョブ情報",
        L"保存先、作成日時、アプリ版、またはWindowsアーキテクチャがありません"));
  }
  if (!dependencies.open_read_only_disk ||
      !dependencies.query_gpt_bindings ||
      !dependencies.query_mbr_bindings ||
      !dependencies.execute_backup) {
    return clonecore::Status::failure(job_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"オンラインイメージ 依存境界",
        L"読取り専用オープン、Volume対応、または実行処理がありません"));
  }
  return clonecore::success_status();
}

vssrequester::SnapshotMetadataContext metadata_context(
    const OnlineBackupJobRequest& request,
    const diskmodel::ReadOnlyPhysicalDiskHandle& source) {
  return vssrequester::SnapshotMetadataContext{
      .source = source.observed.identity,
      .physical_sector_size =
          source.observed.observed.physical_sector_size,
      .windows_major = request.windows_major,
      .windows_minor = request.windows_minor,
      .windows_build = request.windows_build,
      .windows_architecture = request.windows_architecture,
      .created_utc = request.created_utc,
      .app_version = request.app_version,
  };
}

vssrequester::SnapshotImagePlanOptions plan_options(
    const OnlineBackupJobRequest& request,
    const diskmodel::ReadOnlyPhysicalDiskHandle& source,
    std::vector<std::byte> manifest,
    std::vector<std::byte> partition_snapshot) {
  return vssrequester::SnapshotImagePlanOptions{
      .administrator = request.administrator,
      .physical_sector_size =
          source.observed.observed.physical_sector_size,
      .chunk_size = imageformat::kDcimgChunkSize16MiB,
      .compression = imageformat::DcimgCompression::zstandard,
      .verification_block_bytes = 4U * 1024U * 1024U,
      .manifest = std::move(manifest),
      .partition_table_snapshot = std::move(partition_snapshot),
  };
}

}  // namespace

clonecore::Result<vssrequester::OnlineImageBackupReport>
execute_online_backup_job(
    const OnlineBackupJobRequest& request,
    const OnlineBackupJobDependencies& dependencies) {
  const auto valid = validate_request(request, dependencies);
  if (!valid) {
    return clonecore::Result<
        vssrequester::OnlineImageBackupReport>::failure(valid.error());
  }
  auto expected = diskmodel::make_stable_disk_identity(
      request.selected_source, request.selected_source.is_system_disk);
  if (!expected) {
    return clonecore::Result<
        vssrequester::OnlineImageBackupReport>::failure(expected.error());
  }
  auto source = dependencies.open_read_only_disk(expected.value());
  if (!source) {
    return clonecore::Result<
        vssrequester::OnlineImageBackupReport>::failure(source.error());
  }
  if (!source.value().reader) {
    return clonecore::Result<
        vssrequester::OnlineImageBackupReport>::failure(job_error(
            clonecore::ErrorCode::internal_error,
            ERROR_INVALID_HANDLE,
            L"オンラインイメージ 読取り専用ハンドル",
            L"検証済み読取り専用Readerがありません"));
  }
  const auto identity = clonecore::validate_stable_identity(
      expected.value(),
      source.value().observed.identity,
      L"オンラインイメージ コピー元");
  if (!identity) {
    return clonecore::Result<
        vssrequester::OnlineImageBackupReport>::failure(identity.error());
  }

  clonecore::Result<vssrequester::PreparedSnapshotImagePlan> prepared =
      clonecore::Result<
          vssrequester::PreparedSnapshotImagePlan>::failure(job_error(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"オンラインイメージ パーティション形式",
              L"コピー元はGPTまたはMBRでなければなりません"));

  if (source.value().observed.observed.partition_style ==
      diskmodel::PartitionStyle::gpt) {
    auto metadata = vssrequester::build_gpt_snapshot_metadata(
        *source.value().reader,
        metadata_context(request, source.value()));
    if (!metadata) {
      return clonecore::Result<
          vssrequester::OnlineImageBackupReport>::failure(
              metadata.error());
    }
    auto metadata_value = metadata.take_value();
    auto bindings = dependencies.query_gpt_bindings(
        source.value().observed.observed, metadata_value.layout);
    if (!bindings) {
      return clonecore::Result<
          vssrequester::OnlineImageBackupReport>::failure(
              bindings.error());
    }
    prepared = vssrequester::prepare_gpt_snapshot_image_plan(
        metadata_value.layout,
        *source.value().reader,
        bindings.value(),
        plan_options(
            request,
            source.value(),
            std::move(metadata_value.backup_manifest),
            std::move(metadata_value.partition_table_snapshot)));
  } else if (
      source.value().observed.observed.partition_style ==
      diskmodel::PartitionStyle::mbr) {
    auto metadata = vssrequester::build_mbr_snapshot_metadata(
        *source.value().reader,
        metadata_context(request, source.value()));
    if (!metadata) {
      return clonecore::Result<
          vssrequester::OnlineImageBackupReport>::failure(
              metadata.error());
    }
    auto metadata_value = metadata.take_value();
    auto bindings = dependencies.query_mbr_bindings(
        source.value().observed.observed, metadata_value.layout);
    if (!bindings) {
      return clonecore::Result<
          vssrequester::OnlineImageBackupReport>::failure(
              bindings.error());
    }
    prepared = vssrequester::prepare_mbr_snapshot_image_plan(
        metadata_value.layout,
        *source.value().reader,
        bindings.value(),
        plan_options(
            request,
            source.value(),
            std::move(metadata_value.backup_manifest),
            std::move(metadata_value.partition_table_snapshot)));
  }
  if (!prepared) {
    return clonecore::Result<
        vssrequester::OnlineImageBackupReport>::failure(prepared.error());
  }

  auto execution_plan = prepared.take_value();
  execution_plan.image_copy.callbacks = request.callbacks;
  const vssrequester::WindowsOnlineImageBackupRequest execution{
      .plan = std::move(execution_plan),
      .staging =
          imageformat::WindowsFileStagingRequest{
              .final_path = request.final_path,
              .expected_source_disk = source.value().observed.identity,
              .expected_clone_target_disk = std::nullopt,
          },
      .async_wait = request.async_wait,
      .logger = request.logger,
  };
  return dependencies.execute_backup(execution);
}

clonecore::Result<vssrequester::OnlineImageBackupReport>
execute_online_backup_job_with_windows_apis(
    const OnlineBackupJobRequest& request) {
  return execute_online_backup_job(
      request,
      OnlineBackupJobDependencies{
          .open_read_only_disk =
              [](const clonecore::StableDiskIdentity& expected) {
                return diskmodel::
                    open_verified_read_only_physical_disk_with_windows_apis(
                        expected);
              },
          .query_gpt_bindings =
              [](const diskmodel::DiskInfo& disk,
                 const clonecore::GptDisk& layout) {
                return diskmodel::query_windows_volume_bitmap_bindings(
                    disk, layout);
              },
          .query_mbr_bindings =
              [](const diskmodel::DiskInfo& disk,
                 const clonecore::MbrDisk& layout) {
                return diskmodel::query_windows_volume_bitmap_bindings(
                    disk, layout);
              },
          .execute_backup =
              [](const vssrequester::WindowsOnlineImageBackupRequest&
                     execution) {
                return vssrequester::execute_windows_online_image_backup(
                    execution);
              },
      });
}

}  // namespace ytec::windowsapp
