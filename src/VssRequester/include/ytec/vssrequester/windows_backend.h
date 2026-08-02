#pragma once

#include "ytec/clonecore/log.h"
#include "ytec/clonecore/result.h"
#include "ytec/vssrequester/workflow.h"

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::vssrequester {

struct AsyncWaitOptions final {
  DWORD timeout_ms{120'000};
  DWORD poll_interval_ms{250};
  std::function<bool()> cancellation_requested;
};

class IVssAsyncOperation {
 public:
  virtual ~IVssAsyncOperation() = default;

  [[nodiscard]] virtual HRESULT wait(DWORD milliseconds) noexcept = 0;
  [[nodiscard]] virtual HRESULT query_status(
      HRESULT* operation_status) noexcept = 0;
  [[nodiscard]] virtual HRESULT cancel() noexcept = 0;
};

[[nodiscard]] clonecore::Status wait_for_vss_async(
    IVssAsyncOperation& operation,
    const AsyncWaitOptions& options,
    std::wstring_view operation_name);

// このコールバックへ渡るのは、VSSが返し、元ボリュームとの対応を
// 再検証済みのSnapshotデバイスパスだけです。稼働中Volume GUIDパスは
// 型として渡しません。
using SnapshotCopyCallback = std::function<clonecore::Status(
    const std::vector<std::wstring>& snapshot_device_paths)>;

struct WindowsVssBackendOptions final {
  AsyncWaitOptions async_wait;
  SnapshotCopyCallback copy_snapshot_data;
  const clonecore::Logger* logger{};
};

// 1インスタンスを1バックアップ処理の同一スレッド内だけで使用します。
// 実VSSを呼ぶため、管理者権限とWindows VSSサービスが必要です。
class WindowsVssBackend final : public IWorkflowBackend {
 public:
  explicit WindowsVssBackend(WindowsVssBackendOptions options);
  ~WindowsVssBackend() override;

  WindowsVssBackend(const WindowsVssBackend&) = delete;
  WindowsVssBackend& operator=(const WindowsVssBackend&) = delete;
  WindowsVssBackend(WindowsVssBackend&&) = delete;
  WindowsVssBackend& operator=(WindowsVssBackend&&) = delete;

  [[nodiscard]] clonecore::Status initialize_components() override;
  [[nodiscard]] clonecore::Status set_backup_state() override;
  [[nodiscard]] clonecore::Status gather_writer_metadata() override;
  [[nodiscard]] clonecore::Result<std::wstring>
  start_snapshot_set() override;
  [[nodiscard]] clonecore::Status add_volume(
      const std::wstring& snapshot_set_id,
      const std::wstring& volume_guid_path) override;
  [[nodiscard]] clonecore::Status prepare_for_backup() override;
  [[nodiscard]] clonecore::Status do_snapshot_set() override;
  [[nodiscard]] clonecore::Result<std::vector<WriterStatus>>
  query_writer_statuses() override;
  [[nodiscard]] clonecore::Result<std::vector<SnapshotMapping>>
  query_snapshot_devices(
      const std::wstring& snapshot_set_id,
      const std::vector<VolumeRequest>& volumes) override;
  [[nodiscard]] clonecore::Status copy_snapshot_data(
      const std::vector<SnapshotMapping>& mappings) override;
  [[nodiscard]] clonecore::Status backup_complete() override;
  [[nodiscard]] clonecore::Status delete_snapshot_set(
      const std::wstring& snapshot_set_id) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ytec::vssrequester
