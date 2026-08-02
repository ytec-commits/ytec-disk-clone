#include "ytec/windowsapp/job_creation.h"

#include "ytec/clonecore/disk_identity.h"
#include "ytec/imageformat/job_manifest.h"

#include <Windows.h>

#include <utility>

namespace ytec::windowsapp {
namespace {

clonecore::Error creation_error(
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

}  // namespace

std::wstring clone_job_confirmation_token(
    const diskmodel::DiskInfo& target) {
  auto identity =
      diskmodel::make_stable_disk_identity(target, target.is_system_disk);
  if (!identity) {
    return {};
  }
  return clonecore::make_target_confirmation_token(identity.value());
}

std::wstring restore_job_confirmation_token(
    const diskmodel::DiskInfo& target) {
  auto identity =
      diskmodel::make_stable_disk_identity(target, target.is_system_disk);
  if (!identity) {
    return {};
  }
  return clonecore::make_target_confirmation_token(identity.value());
}

clonecore::Result<std::vector<std::byte>>
create_confirmed_clone_job(
    const CloneJobCreationRequest& request) {
  auto source = diskmodel::make_stable_disk_identity(
      request.source, request.source.is_system_disk);
  if (!source) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        source.error());
  }
  auto target = diskmodel::make_stable_disk_identity(
      request.target, request.target.is_system_disk);
  if (!target) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        target.error());
  }
  const clonecore::TargetConfirmation confirmation{
      .first_step_acknowledged = request.first_step_acknowledged,
      .typed_token = request.typed_confirmation,
  };
  const auto confirmation_status = clonecore::validate_clone_identities(
      source.value(),
      source.value(),
      target.value(),
      target.value(),
      confirmation);
  if (!confirmation_status) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        confirmation_status.error());
  }
  if (request.created_utc.empty() || request.app_version.empty()) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        creation_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_DATA,
            L"Windows版クローンジョブ作成",
            L"作成日時またはアプリバージョンがありません"));
  }
  if (request.requested_conversion !=
          imageformat::RequestedConversion::preserve &&
      request.requested_conversion !=
          imageformat::RequestedConversion::mbr_to_gpt) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        creation_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"Windows版クローンジョブの変換指定",
            L"形式維持またはMBRからGPTだけを指定できます"));
  }
  const bool mbr_to_gpt =
      request.requested_conversion ==
      imageformat::RequestedConversion::mbr_to_gpt;
  if (mbr_to_gpt &&
      (request.source.partition_style != diskmodel::PartitionStyle::mbr ||
       request.source.partitions.empty() ||
       request.target.partition_style != diskmodel::PartitionStyle::raw ||
       !request.target.partitions.empty())) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        creation_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"Windows版MBRからGPTジョブのディスク構成",
            L"パーティションを持つMBRコピー元と空のRAWコピー先が必要です"));
  }
  if (mbr_to_gpt && request.auto_execute_once) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        creation_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_ACCESS_DENIED,
            L"Windows版MBRからGPTジョブの実行方式",
            L"ファームウェア切替を伴う移行はWinPEでの手動確認だけを許可します"));
  }
  return imageformat::serialize_hashed_job_manifest(
      imageformat::JobManifest{
          .schema_version = imageformat::kJobManifestSchemaVersion,
          .job_type = mbr_to_gpt
              ? imageformat::JobType::mbr_to_gpt
              : imageformat::JobType::clone,
          .source = source.take_value(),
          .target = target.take_value(),
          .image_path = {},
          .requested_conversion = request.requested_conversion,
          .created_utc = request.created_utc,
          .app_version = request.app_version,
          .execution_mode = request.auto_execute_once
              ? imageformat::JobExecutionMode::auto_once
              : imageformat::JobExecutionMode::review_required,
          .destructive_target_confirmed = true,
      });
}

clonecore::Result<std::vector<std::byte>>
create_confirmed_restore_job(
    const RestoreJobCreationRequest& request) {
  auto target = diskmodel::make_stable_disk_identity(
      request.target, request.target.is_system_disk);
  if (!target) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        target.error());
  }
  if (target.value().is_system_disk) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        creation_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_ACCESS_DENIED,
            L"復元ジョブのシステムディスク保護",
            L"実行中システムのディスクは復元先にできません"));
  }
  if (!request.first_step_acknowledged ||
      request.typed_confirmation !=
          clonecore::make_target_confirmation_token(target.value())) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        creation_error(
            clonecore::ErrorCode::confirmation_required,
            ERROR_CANCELLED,
            L"復元先消去の二段階確認",
            L"復元先の確認操作または入力確認文字列が一致しません"));
  }
  if (request.verified_image_path.empty() ||
      request.verified_image_length == 0 ||
      request.created_utc.empty() || request.app_version.empty()) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        creation_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_DATA,
            L"Windows版復元ジョブ作成",
            L"検証済みイメージ、作成日時、またはアプリバージョンがありません"));
  }
  return imageformat::serialize_hashed_job_manifest(
      imageformat::JobManifest{
          .schema_version = imageformat::kJobManifestSchemaVersion,
          .job_type = imageformat::JobType::restore_image,
          .source = std::nullopt,
          .target = target.take_value(),
          .image_path = request.verified_image_path,
          .restore_image_identity =
              imageformat::RestoreImageIdentity{
                  .length_bytes = request.verified_image_length,
                  .global_hash = request.verified_image_global_hash,
              },
          .requested_conversion =
              imageformat::RequestedConversion::preserve,
          .created_utc = request.created_utc,
          .app_version = request.app_version,
          .execution_mode = request.auto_execute_once
              ? imageformat::JobExecutionMode::auto_once
              : imageformat::JobExecutionMode::review_required,
          .destructive_target_confirmed = true,
      });
}

}  // namespace ytec::windowsapp
