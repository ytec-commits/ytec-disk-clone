#include "ytec/windowsapp/reboot_handoff.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>

#include <memory>
#include <string>
#include <utility>

namespace ytec::windowsapp {
namespace {

clonecore::Error reboot_error(
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

class WindowsRebootHandoffService final
    : public IRebootHandoffService {
 public:
  clonecore::Status restart_to_advanced_boot_options() override {
    clonecore::UniqueHandle token;
    HANDLE raw_token{};
    if (OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
            &raw_token) == FALSE) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::access_denied,
          L"WinPE引き継ぎ再起動の権限確認",
          GetLastError()));
    }
    token.reset(raw_token);

    LUID shutdown_privilege{};
    if (LookupPrivilegeValueW(
            nullptr, SE_SHUTDOWN_NAME, &shutdown_privilege) == FALSE) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::query_failed,
          L"WinPE引き継ぎ再起動の権限識別",
          GetLastError()));
    }
    TOKEN_PRIVILEGES requested{};
    requested.PrivilegeCount = 1;
    requested.Privileges[0].Luid = shutdown_privilege;
    requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    TOKEN_PRIVILEGES previous{};
    DWORD previous_size{};
    SetLastError(ERROR_SUCCESS);
    const BOOL adjusted = AdjustTokenPrivileges(
        token.get(),
        FALSE,
        &requested,
        sizeof(previous),
        &previous,
        &previous_size);
    const DWORD adjustment_error = GetLastError();
    if (adjusted == FALSE || adjustment_error == ERROR_NOT_ALL_ASSIGNED) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::access_denied,
          L"WinPE引き継ぎ再起動の権限有効化",
          adjusted == FALSE ? adjustment_error : ERROR_PRIVILEGE_NOT_HELD));
    }

    std::wstring message =
        L"Y-TEC Tsumugi Drive: 作成済みWinPEメディアを選ぶため、"
        L"起動オプションへ再起動します。";
    const DWORD result = InitiateShutdownW(
        nullptr,
        message.data(),
        0,
        SHUTDOWN_RESTART | SHUTDOWN_RESTART_BOOTOPTIONS,
        SHTDN_REASON_MAJOR_APPLICATION |
            SHTDN_REASON_MINOR_MAINTENANCE |
            SHTDN_REASON_FLAG_PLANNED);

    if (previous_size != 0U) {
      static_cast<void>(AdjustTokenPrivileges(
          token.get(), FALSE, &previous, 0, nullptr, nullptr));
    }
    if (result != ERROR_SUCCESS) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          result == ERROR_ACCESS_DENIED
              ? clonecore::ErrorCode::access_denied
              : clonecore::ErrorCode::io_failed,
          L"Windows起動オプションへの再起動要求",
          result));
    }
    return clonecore::success_status();
  }
};

}  // namespace

RebootHandoffPlan build_reboot_handoff_plan(
    const bool process_elevated,
    const std::uint32_t windows_major,
    const std::uint32_t windows_minor) {
  const bool advanced_startup_supported = windows_major > 6U ||
      (windows_major == 6U && windows_minor >= 2U);
  if (!advanced_startup_supported) {
    return RebootHandoffPlan{
        .readiness = RebootHandoffReadiness::unsupported_windows,
        .guidance =
            L"このWindowsではアプリから起動オプションへ移動できません。"
            L"作業を保存して通常再起動し、電源投入直後のBoot Menuから"
            L"作成済みWinPE USBを選んでください。",
    };
  }
  if (!process_elevated) {
    return RebootHandoffPlan{
        .readiness = RebootHandoffReadiness::elevation_required,
        .guidance =
            L"自動UACは行いません。作業を保存してWindowsの回復メニューから"
            L"「今すぐ再起動」を選ぶか、アプリを管理者として手動起動して"
            L"起動オプションへ進んでください。",
    };
  }
  return RebootHandoffPlan{
      .readiness = RebootHandoffReadiness::ready,
      .guidance =
          L"作業中のファイルを保存し、WinPE USBを接続してください。"
          L"再起動後はMicrosoftの「デバイスの使用」からUSBを選びます。"
          L"表示されない機種では電源投入直後のBoot Menuを使用してください。",
  };
}

clonecore::Status request_reboot_handoff(
    const RebootHandoffPlan& plan,
    IRebootHandoffService& service) {
  if (plan.readiness == RebootHandoffReadiness::elevation_required) {
    return clonecore::Status::failure(reboot_error(
        clonecore::ErrorCode::access_denied,
        ERROR_ELEVATION_REQUIRED,
        L"WinPE引き継ぎ再起動",
        L"再起動要求には管理者としての手動起動が必要です"));
  }
  if (plan.readiness != RebootHandoffReadiness::ready) {
    return clonecore::Status::failure(reboot_error(
        clonecore::ErrorCode::unsupported_platform,
        ERROR_OLD_WIN_VERSION,
        L"WinPE引き継ぎ再起動",
        L"このWindowsでは起動オプションへの再起動を使用できません"));
  }
  return service.restart_to_advanced_boot_options();
}

std::unique_ptr<IRebootHandoffService>
make_windows_reboot_handoff_service() {
  return std::make_unique<WindowsRebootHandoffService>();
}

}  // namespace ytec::windowsapp
