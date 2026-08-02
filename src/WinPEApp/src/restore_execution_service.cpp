#include "ytec/winpeapp/app_runner.h"
#include "boot_finalization.h"

#include "ytec/diskmodel/physical_disk.h"
#include "ytec/imageformat/restore.h"
#include "ytec/winpeapp/product_io_policy.h"

#include <Windows.h>

#include <cstddef>
#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace ytec::winpeapp {
namespace {

clonecore::Error restore_service_error(
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

class WindowsRestoreExecutionService final
    : public IRestoreExecutionService {
 public:
  [[nodiscard]] clonecore::Result<RestoreExecutionReport> execute(
      const RestoreExecutionRequest& request) override {
    const auto io_policy = select_product_io_policy(
        request.expected_target.logical_sector_size);
    if (!io_policy.has_value()) {
      return clonecore::Result<RestoreExecutionReport>::failure(
          restore_service_error(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"製品版復元I/O方針",
              L"実機相当検証までは512バイト論理セクターだけを対象にします"));
    }
    auto session =
        imageformat::open_restore_image_file_read_session_v1(
            request.verified_image_path);
    if (!session) {
      return clonecore::Result<RestoreExecutionReport>::failure(
          session.error());
    }
    const imageformat::Sha256ReadCallback reader =
        [&session](
            const std::uint64_t offset,
            const std::size_t length) {
          return session.value()->read_at(offset, length);
        };
    if (session.value()->image_length() !=
        request.expected_image.length_bytes) {
      return clonecore::Result<RestoreExecutionReport>::failure(
          restore_service_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"復元サービスのdcimg再識別",
              L"実行直前に開いたdcimgの長さがジョブ記録と一致しません"));
    }
    auto prepared = imageformat::prepare_verified_dcimg_v1_from_reader(
        session.value()->image_length(),
        reader,
        imageformat::DcimgRestoreRequest{
            .expected_image_length =
                request.expected_image.length_bytes,
            .expected_global_hash =
                request.expected_image.global_hash,
            .maximum_chunk_bytes = io_policy->transfer_chunk_bytes,
            .callbacks = request.callbacks,
        });
    if (!prepared) {
      return clonecore::Result<RestoreExecutionReport>::failure(
          prepared.error());
    }
    if (clonecore::disk_operation_cancellation_requested(
            request.callbacks)) {
      return clonecore::Result<RestoreExecutionReport>::failure(
          restore_service_error(
              clonecore::ErrorCode::access_denied,
              ERROR_CANCELLED,
              L"復元サービス開始前キャンセル",
              L"利用者の要求により復元を開始しませんでした"));
    }

    const auto offline =
        diskmodel::set_verified_physical_target_offline_with_windows_apis(
            request.expected_target,
            request.confirmation,
            true);
    if (!offline) {
      return clonecore::Result<RestoreExecutionReport>::failure(
          offline.error());
    }

    imageformat::DcimgRestoreReport restore_report;
    {
      auto target =
          diskmodel::open_verified_physical_target_with_windows_apis(
              request.expected_target,
              request.confirmation);
      if (!target) {
        return clonecore::Result<RestoreExecutionReport>::failure(
            target.error());
      }
      auto restored = imageformat::restore_prepared_dcimg_v1(
          prepared.take_value(),
          imageformat::DcimgRestoreRequest{
              .expected_target = request.expected_target,
              .observed_target =
                  target.value().observed.target_identity,
              .confirmation = request.confirmation,
              .expected_image_length =
                  request.expected_image.length_bytes,
              .expected_global_hash =
                  request.expected_image.global_hash,
              .maximum_chunk_bytes = io_policy->transfer_chunk_bytes,
              .callbacks = request.callbacks,
          },
          *target.value().target);
      if (!restored) {
        // A failed or cancelled partial target deliberately remains offline.
        return clonecore::Result<RestoreExecutionReport>::failure(
            restored.error());
      }
      restore_report = restored.take_value();
    }

    const auto online =
        diskmodel::set_verified_physical_target_offline_with_windows_apis(
            request.expected_target,
            request.confirmation,
            false);
    if (!online) {
      return clonecore::Result<RestoreExecutionReport>::failure(
          online.error());
    }
    const diskmodel::PartitionStyle restored_style =
        restore_report.partition_style ==
                imageformat::BackupPartitionStyle::gpt
            ? diskmodel::PartitionStyle::gpt
            : diskmodel::PartitionStyle::mbr;
    const auto expected_boot_mode = restored_style ==
            diskmodel::PartitionStyle::gpt
        ? imageformat::BackupBootMode::uefi
        : imageformat::BackupBootMode::legacy_bios;
    if (restore_report.boot_mode != expected_boot_mode ||
        restore_report.windows_partition_offset == 0U) {
      const auto protected_offline =
          diskmodel::set_verified_physical_target_offline_with_windows_apis(
              request.expected_target,
              request.confirmation,
              true);
      return clonecore::Result<RestoreExecutionReport>::failure(
          protected_offline ? restore_service_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_INVALID_DATA,
              L"復元後起動最終化情報",
              L"検証済みdcimgの形式、起動方式、またはWindows領域が矛盾しています")
                            : protected_offline.error());
    }
    auto inventory = diskmodel::make_windows_disk_inventory_provider();
    const auto finalized = finalize_product_target_boot(
        *inventory,
        request.expected_target,
        restored_style,
        restore_report.windows_partition_offset,
        request.callbacks);
    if (!finalized) {
      const auto protected_offline =
          diskmodel::set_verified_physical_target_offline_with_windows_apis(
              request.expected_target,
              request.confirmation,
              true);
      return clonecore::Result<RestoreExecutionReport>::failure(
          protected_offline ? finalized.error() : protected_offline.error());
    }
    return clonecore::Result<RestoreExecutionReport>::success(
        RestoreExecutionReport{
            .restored_data_bytes =
                restore_report.restored_data_bytes,
            .committed_partition_table_bytes =
                restore_report.committed_partition_table_bytes,
            .restored_chunk_count =
                restore_report.restored_chunk_count,
            .complete_image_verified_before_write =
                restore_report.complete_image_verified_before_write,
            .backup_manifest_verified_before_write =
                restore_report.backup_manifest_verified_before_write,
            .read_back_verified =
                restore_report.read_back_verified,
            .partition_table_committed =
                restore_report.partition_table_committed,
            .target_returned_online = true,
            .boot_repair = finalized.value().boot_repair,
            .windows_partition_temporarily_mounted =
                finalized.value().windows_partition_temporarily_mounted,
            .system_partition_temporarily_mounted =
                finalized.value().system_partition_temporarily_mounted,
            .temporary_mounts_released =
                finalized.value().temporary_mounts_released,
            .boot_finalization_verified =
                finalized.value().final_target_verified,
        });
  }
};

}  // namespace

std::unique_ptr<IRestoreExecutionService>
make_windows_restore_execution_service() {
  return std::make_unique<WindowsRestoreExecutionService>();
}

}  // namespace ytec::winpeapp
