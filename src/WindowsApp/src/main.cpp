#include "ytec/clonecore/log.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/imageformat/job_file.h"
#include "ytec/imageformat/job_manifest.h"
#include "ytec/uisupport/private_fonts.h"
#include "ytec/vssrequester/windows_backend.h"
#include "ytec/windowsapp/job_creation.h"
#include "ytec/windowsapp/job_result_import.h"
#include "ytec/windowsapp/layout.h"
#include "ytec/windowsapp/media_creation.h"
#include "ytec/windowsapp/media_preflight.h"
#include "ytec/windowsapp/media_wizard.h"
#include "ytec/windowsapp/online_backup_job.h"
#include "ytec/windowsapp/online_shrink_backup_job.h"
#include "ytec/windowsapp/progress.h"
#include "ytec/windowsapp/reboot_handoff.h"
#include "ytec/windowsapp/restore_preflight.h"
#include "ytec/windowsapp/selection.h"
#include "ytec/windowsapp/system_state.h"
#include "ytec/windowsapp/usb_volume_mapping.h"

#include "resource.h"

#include <Windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern "C" NTSYSAPI NTSTATUS NTAPI RtlGetVersion(
    PRTL_OSVERSIONINFOW version_information);

namespace {

constexpr wchar_t kWindowClass[] = L"YtecTsumugiDriveMainWindow";
constexpr wchar_t kWindowTitle[] = L"Y-TEC Tsumugi Drive";
constexpr std::string_view kAppVersion{"0.2.0-dev"};
constexpr std::wstring_view kAppVersionWide{L"0.2.0-dev"};
constexpr wchar_t kOfficialAdkInstallGuideUrl[] =
    L"https://learn.microsoft.com/ja-jp/windows-hardware/get-started/adk-install";
constexpr wchar_t kOfficialAdkServicingGuideUrl[] =
    L"https://learn.microsoft.com/ja-jp/windows-hardware/get-started/adk-servicing";
constexpr UINT kInventoryCompleteMessage = WM_APP + 1U;
constexpr UINT kBackupCompleteMessage = WM_APP + 2U;
constexpr UINT kMediaPreflightCompleteMessage = WM_APP + 3U;
constexpr UINT kRestorePreflightCompleteMessage = WM_APP + 4U;
constexpr UINT kMediaCreationProgressMessage = WM_APP + 5U;
constexpr UINT kMediaCreationCompleteMessage = WM_APP + 6U;
constexpr UINT kBackupProgressMessage = WM_APP + 7U;
constexpr UINT_PTR kUiRefreshTimerId = 1U;

constexpr int kNavFirstId = 100;
constexpr int kRefreshId = 200;
constexpr int kSourceComboId = 201;
constexpr int kTargetComboId = 202;
constexpr int kPrimaryActionId = 203;
constexpr int kRestoreChangeImageId = 204;
constexpr int kMediaKindComboId = 205;
constexpr int kMediaProfileComboId = 206;
constexpr int kMediaOutputEditId = 207;
constexpr int kMediaBrowseId = 208;
constexpr int kTransferModeComboId = 209;
constexpr int kAdkGuideDownloadPageId = 3101;
constexpr int kAdkGuideServicingPageId = 3102;
constexpr int kAdkGuideRecheckId = 3103;

constexpr COLORREF kCanvas = RGB(244, 247, 249);
constexpr COLORREF kSidebar = RGB(29, 37, 49);
constexpr COLORREF kSidebarSelected = RGB(51, 64, 81);
constexpr COLORREF kInk = RGB(29, 40, 52);
constexpr COLORREF kMuted = RGB(91, 105, 118);
constexpr COLORREF kBorder = RGB(214, 222, 228);
constexpr COLORREF kCard = RGB(255, 255, 255);
constexpr COLORREF kTsumugiBlue = RGB(30, 145, 160);
constexpr COLORREF kTsumugiPurple = RGB(121, 91, 174);
constexpr COLORREF kSafeGreen = RGB(42, 137, 93);
constexpr COLORREF kWarning = RGB(183, 112, 26);

class UiComApartment final {
 public:
  UiComApartment() noexcept
      : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}

  ~UiComApartment() {
    if (SUCCEEDED(result_)) {
      CoUninitialize();
    }
  }

  UiComApartment(const UiComApartment&) = delete;
  UiComApartment& operator=(const UiComApartment&) = delete;

  [[nodiscard]] bool initialized() const noexcept {
    return SUCCEEDED(result_);
  }

  [[nodiscard]] HRESULT result() const noexcept { return result_; }

 private:
  HRESULT result_{};
};

enum class Page : std::uint8_t {
  clone,
  create_image,
  restore_image,
  boot_repair,
  rescue_media,
  diagnostics,
};

struct InventoryPayload final {
  std::optional<ytec::diskmodel::InventoryReport> report;
  std::wstring error;
  std::vector<ytec::windowsapp::ImportedJobResult> job_results;
  std::wstring job_result_error;
};

struct BackupPayload final {
  std::optional<ytec::vssrequester::OnlineImageBackupReport> report;
  std::optional<ytec::windowsapp::OnlineShrinkBackupJobReport>
      shrink_report;
  std::optional<ytec::clonecore::Error> error;
  std::wstring final_path;
  ytec::imageformat::TransferMode transfer_mode{
      ytec::imageformat::TransferMode::exact};
};

struct BackupProgressPayload final {
  ytec::clonecore::DiskOperationProgress progress;
  std::chrono::milliseconds elapsed{};
};

struct MediaPreflightPayload final {
  ytec::windowsapp::MediaPreflightView view;
};

struct MediaCreationProgressPayload final {
  ytec::windowsapp::RescueMediaCreationProgress progress;
};

struct MediaCreationPayload final {
  std::optional<ytec::windowsapp::RescueMediaCreationReport> report;
  std::optional<ytec::clonecore::Error> error;
};

struct RestorePreflightPayload final {
  std::optional<ytec::windowsapp::RestoreImagePreflightReport> report;
  std::optional<ytec::clonecore::Error> error;
};

struct ProductLogSession final {
  ytec::clonecore::Logger logger;
  std::wstring path;
};

struct AppState final {
  HWND window{};
  std::array<HWND, 6> navigation{};
  HWND refresh{};
  HWND source_combo{};
  HWND target_combo{};
  HWND transfer_mode_combo{};
  HWND restore_change_image{};
  HWND media_kind_combo{};
  HWND media_profile_combo{};
  HWND media_output_edit{};
  HWND media_browse{};
  HWND primary_action{};
  HFONT body_font{};
  HFONT small_font{};
  HFONT heading_font{};
  HFONT brand_font{};
  ytec::uisupport::PrivateFontCollection private_fonts;
  Page page{Page::clone};
  std::optional<ytec::clonecore::Logger> logger;
  std::wstring log_path;
  std::wstring log_error;
  std::optional<ytec::diskmodel::InventoryReport> inventory;
  std::wstring inventory_error;
  std::vector<ytec::windowsapp::ImportedJobResult> job_results;
  std::wstring job_result_error;
  std::atomic_bool inventory_loading{false};
  std::thread inventory_thread;
  std::atomic_bool backup_running{false};
  std::atomic_bool backup_cancel_requested{false};
  std::optional<ytec::clonecore::DiskOperationProgress> backup_progress;
  std::chrono::milliseconds backup_elapsed{};
  std::thread backup_thread;
  std::atomic_bool media_preflight_running{false};
  std::optional<ytec::windowsapp::MediaPreflightView> media_preflight;
  std::thread media_preflight_thread;
  std::atomic_bool media_creation_running{false};
  std::atomic_bool media_creation_cancel_requested{false};
  std::optional<ytec::windowsapp::RescueMediaCreationProgress>
      media_creation_progress;
  std::optional<ytec::windowsapp::RescueMediaCreationReport>
      media_creation_report;
  std::optional<ytec::windowsapp::RescueMediaCreationStage>
      last_logged_media_stage;
  ULONGLONG media_creation_started_tick{};
  std::thread media_creation_thread;
  std::atomic_bool restore_preflight_running{false};
  std::atomic_bool restore_preflight_cancel_requested{false};
  std::optional<ytec::windowsapp::RestoreImagePreflightReport>
      restore_preflight;
  std::thread restore_preflight_thread;
  bool elevated{};
};

struct ConfirmationDialogState final {
  std::wstring details;
  std::wstring token;
  std::wstring confirm_button_label{L"ジョブを作成"};
  bool offer_auto_execute_once{};
  bool auto_execute_once_selected{};
  HFONT font{};
};

constexpr std::array<std::wstring_view, 6> kNavigationLabels{
    L"ドライブをクローン",
    L"イメージを作成",
    L"イメージを復元",
    L"起動を修復",
    L"レスキューメディア",
    L"ログ・診断",
};

std::wstring format_bytes(const std::uint64_t bytes) {
  return ytec::windowsapp::format_bytes(bytes);
}

std::wstring partition_style_text(
    const ytec::diskmodel::PartitionStyle style) {
  switch (style) {
    case ytec::diskmodel::PartitionStyle::gpt:
      return L"GPT";
    case ytec::diskmodel::PartitionStyle::mbr:
      return L"MBR";
    case ytec::diskmodel::PartitionStyle::raw:
      return L"未初期化";
    case ytec::diskmodel::PartitionStyle::unknown:
      return L"不明";
  }
  return L"不明";
}

std::wstring disk_label(const ytec::diskmodel::DiskInfo& disk) {
  std::wstring label =
      L"ディスク " + std::to_wstring(disk.disk_number) + L"  ";
  label += disk.model.empty() ? L"モデル不明" : disk.model;
  label += L"  (" + format_bytes(disk.size_bytes) + L")";
  if (disk.is_system_disk) {
    label += L"  [Windows]";
  }
  return label;
}

std::wstring format_error_message(
    const ytec::clonecore::Error& error) {
  return error.operation + L"\n\n" + error.message +
         L"\nWindows error: " + std::to_wstring(error.native_code);
}

std::wstring ascii_to_wide(const std::string_view value) {
  return std::wstring(value.begin(), value.end());
}

std::wstring job_result_type_text(
    const ytec::imageformat::JobType type) {
  switch (type) {
    case ytec::imageformat::JobType::clone:
      return L"クローン";
    case ytec::imageformat::JobType::restore_image:
      return L"イメージ復元";
    case ytec::imageformat::JobType::create_image:
      return L"イメージ作成";
    case ytec::imageformat::JobType::mbr_to_gpt:
      return L"MBRからGPT";
  }
  return L"不明";
}

std::wstring job_hash_prefix(
    const ytec::imageformat::Sha256Digest& digest) {
  constexpr std::array<wchar_t, 16> kHex{
      L'0', L'1', L'2', L'3', L'4', L'5', L'6', L'7',
      L'8', L'9', L'A', L'B', L'C', L'D', L'E', L'F'};
  std::wstring output;
  output.reserve(12U);
  for (std::size_t index = 0; index < 6U; ++index) {
    const auto value = std::to_integer<unsigned int>(digest[index]);
    output.push_back(kHex[(value >> 4U) & 0x0FU]);
    output.push_back(kHex[value & 0x0FU]);
  }
  return output;
}

std::string current_utc_timestamp() {
  SYSTEMTIME time{};
  GetSystemTime(&time);
  std::array<char, 21> buffer{};
  const int written = sprintf_s(
      buffer.data(),
      buffer.size(),
      "%04u-%02u-%02uT%02u:%02u:%02uZ",
      static_cast<unsigned int>(time.wYear),
      static_cast<unsigned int>(time.wMonth),
      static_cast<unsigned int>(time.wDay),
      static_cast<unsigned int>(time.wHour),
      static_cast<unsigned int>(time.wMinute),
      static_cast<unsigned int>(time.wSecond));
  return written == 20 ? std::string(buffer.data(), 20) : std::string{};
}

ytec::clonecore::Result<std::array<std::uint32_t, 3>>
current_windows_version() {
  RTL_OSVERSIONINFOW version{};
  version.dwOSVersionInfoSize = sizeof(version);
  const NTSTATUS status = RtlGetVersion(&version);
  if (status < 0) {
    return ytec::clonecore::Result<
        std::array<std::uint32_t, 3>>::failure(
        ytec::clonecore::Error{
            .code = ytec::clonecore::ErrorCode::query_failed,
            .native_code = RtlNtStatusToDosError(status),
            .operation = L"Windowsバージョン取得",
            .message =
                L"バックアップマニフェストへ記録するWindows版を取得できません",
        });
  }
  return ytec::clonecore::Result<
      std::array<std::uint32_t, 3>>::success({
      version.dwMajorVersion,
      version.dwMinorVersion,
      version.dwBuildNumber,
  });
}

std::string current_native_architecture() {
  SYSTEM_INFO information{};
  GetNativeSystemInfo(&information);
  if (information.wProcessorArchitecture ==
      PROCESSOR_ARCHITECTURE_AMD64) {
    return "AMD64";
  }
  return {};
}

ytec::clonecore::Result<ProductLogSession>
create_product_log_session() {
  std::vector<wchar_t> executable_path(1024U, L'\0');
  for (;;) {
    const DWORD copied = GetModuleFileNameW(
        nullptr,
        executable_path.data(),
        static_cast<DWORD>(executable_path.size()));
    if (copied == 0U) {
      return ytec::clonecore::Result<ProductLogSession>::failure(
          ytec::clonecore::make_win32_error(
              ytec::clonecore::ErrorCode::query_failed,
              L"アプリ配置場所の確認",
              GetLastError()));
    }
    if (copied < executable_path.size()) {
      executable_path.resize(copied);
      break;
    }
    if (executable_path.size() >= 32U * 1024U) {
      return ytec::clonecore::Result<ProductLogSession>::failure({
          .code = ytec::clonecore::ErrorCode::query_failed,
          .native_code = ERROR_INSUFFICIENT_BUFFER,
          .operation = L"アプリ配置場所の確認",
          .message = L"配置場所を安全に取得できません",
      });
    }
    executable_path.resize(executable_path.size() * 2U, L'\0');
  }

  const std::wstring full_path(
      executable_path.data(), executable_path.size());
  const std::size_t separator = full_path.find_last_of(L"\\/");
  if (separator == std::wstring::npos || separator == 0U) {
    return ytec::clonecore::Result<ProductLogSession>::failure({
        .code = ytec::clonecore::ErrorCode::invalid_data,
        .native_code = ERROR_INVALID_NAME,
        .operation = L"ログ保存場所の確認",
        .message = L"アプリと同じフォルダーを安全に識別できません",
    });
  }
  const std::wstring application_directory =
      full_path.substr(0U, separator);
  const DWORD application_attributes =
      GetFileAttributesW(application_directory.c_str());
  if (application_attributes == INVALID_FILE_ATTRIBUTES) {
    return ytec::clonecore::Result<ProductLogSession>::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::query_failed,
            L"アプリ配置フォルダーの確認",
            GetLastError()));
  }
  if ((application_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (application_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return ytec::clonecore::Result<ProductLogSession>::failure({
        .code = ytec::clonecore::ErrorCode::invalid_data,
        .native_code = ERROR_REPARSE_TAG_INVALID,
        .operation = L"アプリ配置フォルダーの確認",
        .message =
            L"特殊なリンク先には診断ログを作成しません",
    });
  }

  const std::wstring log_directory =
      application_directory + L"\\logs";
  if (CreateDirectoryW(log_directory.c_str(), nullptr) == FALSE) {
    const DWORD create_error = GetLastError();
    if (create_error != ERROR_ALREADY_EXISTS) {
      return ytec::clonecore::Result<ProductLogSession>::failure(
          ytec::clonecore::make_win32_error(
              ytec::clonecore::ErrorCode::io_failed,
              L"診断ログフォルダー作成",
              create_error));
    }
  }
  const DWORD log_attributes = GetFileAttributesW(log_directory.c_str());
  if (log_attributes == INVALID_FILE_ATTRIBUTES) {
    return ytec::clonecore::Result<ProductLogSession>::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::query_failed,
            L"診断ログフォルダー確認",
            GetLastError()));
  }
  if ((log_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (log_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return ytec::clonecore::Result<ProductLogSession>::failure({
        .code = ytec::clonecore::ErrorCode::invalid_data,
        .native_code = ERROR_REPARSE_TAG_INVALID,
        .operation = L"診断ログフォルダー確認",
        .message =
            L"通常のローカルフォルダー以外には診断ログを作成しません",
    });
  }

  SYSTEMTIME local_time{};
  GetLocalTime(&local_time);
  std::array<wchar_t, 96U> base_name{};
  const int base_length = swprintf_s(
      base_name.data(),
      base_name.size(),
      L"TsumugiDrive-%04u%02u%02u-%02u%02u%02u-%03u-%lu",
      static_cast<unsigned int>(local_time.wYear),
      static_cast<unsigned int>(local_time.wMonth),
      static_cast<unsigned int>(local_time.wDay),
      static_cast<unsigned int>(local_time.wHour),
      static_cast<unsigned int>(local_time.wMinute),
      static_cast<unsigned int>(local_time.wSecond),
      static_cast<unsigned int>(local_time.wMilliseconds),
      static_cast<unsigned long>(GetCurrentProcessId()));
  if (base_length <= 0) {
    return ytec::clonecore::Result<ProductLogSession>::failure({
        .code = ytec::clonecore::ErrorCode::internal_error,
        .native_code = ERROR_INVALID_DATA,
        .operation = L"診断ログ名作成",
        .message = L"診断ログ名を作成できません",
    });
  }

  for (unsigned int attempt = 0U; attempt < 10U; ++attempt) {
    std::wstring file_name(
        base_name.data(), static_cast<std::size_t>(base_length));
    if (attempt != 0U) {
      file_name += L"-" + std::to_wstring(attempt);
    }
    const std::wstring log_path =
        log_directory + L"\\" + file_name + L".log";
    auto logger = ytec::clonecore::make_utf8_file_logger(
        log_path, true);
    if (logger) {
      return ytec::clonecore::Result<ProductLogSession>::success({
          .logger = logger.take_value(),
          .path = log_path,
      });
    }
    if (logger.error().native_code != ERROR_FILE_EXISTS &&
        logger.error().native_code != ERROR_ALREADY_EXISTS) {
      return ytec::clonecore::Result<ProductLogSession>::failure(
          logger.error());
    }
  }
  return ytec::clonecore::Result<ProductLogSession>::failure({
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_FILE_EXISTS,
      .operation = L"診断ログファイル作成",
      .message = L"重複しない新しい診断ログ名を確保できません",
  });
}

void log_error_summary(
    const std::optional<ytec::clonecore::Logger>& logger,
    const std::wstring_view context,
    const ytec::clonecore::Error& error) noexcept {
  if (!logger.has_value()) {
    return;
  }
  logger->error(
      std::wstring(context) + L" code=" +
      std::wstring(ytec::clonecore::error_code_name(error.code)) +
      L" native_code=" + std::to_wstring(error.native_code));
}

void log_inventory_summary(
    const std::optional<ytec::clonecore::Logger>& logger,
    const ytec::diskmodel::InventoryReport& report) noexcept {
  if (!logger.has_value()) {
    return;
  }
  logger->info(
      L"読み取り専用ディスク列挙完了 disks=" +
      std::to_wstring(report.disks.size()) + L" issues=" +
      std::to_wstring(report.issues.size()));
  for (const auto& disk : report.disks) {
    const std::wstring serial(
        disk.serial_suffix.begin(), disk.serial_suffix.end());
    logger->info(
        L"ディスク要約 number=" +
        std::to_wstring(disk.disk_number) + L" model=" +
        (disk.model.empty() ? L"不明" : disk.model) + L" bytes=" +
        std::to_wstring(disk.size_bytes) + L" logical_sector=" +
        std::to_wstring(disk.logical_sector_size) +
        L" physical_sector=" +
        std::to_wstring(disk.physical_sector_size) + L" bus=" +
        (disk.bus_type.empty() ? L"不明" : disk.bus_type) +
        L" serial_suffix=" + (serial.empty() ? L"不明" : serial) +
        L" style=" + partition_style_text(disk.partition_style) +
        L" partitions=" +
        std::to_wstring(disk.partitions.size()) + L" system=" +
        (disk.is_system_disk ? L"true" : L"false"));
  }
  for (const auto& issue : report.issues) {
    logger->warning(
        L"ディスク列挙項目の警告 code=" +
        std::wstring(
            ytec::clonecore::error_code_name(issue.error.code)) +
        L" native_code=" +
        std::to_wstring(issue.error.native_code));
  }
}

INT_PTR CALLBACK confirmation_dialog_proc(
    const HWND dialog,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
  auto* state = reinterpret_cast<ConfirmationDialogState*>(
      GetWindowLongPtrW(dialog, GWLP_USERDATA));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<ConfirmationDialogState*>(lparam);
    SetWindowLongPtrW(
        dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    if (state->font != nullptr) {
      SendMessageW(
          dialog,
          WM_SETFONT,
          reinterpret_cast<WPARAM>(state->font),
          TRUE);
      EnumChildWindows(
          dialog,
          [](const HWND child, const LPARAM font) -> BOOL {
            SendMessageW(
                child,
                WM_SETFONT,
                static_cast<WPARAM>(font),
                TRUE);
            return TRUE;
          },
          reinterpret_cast<LPARAM>(state->font));
    }
    SetDlgItemTextW(dialog, IDC_CONFIRM_DETAILS, state->details.c_str());
    const std::wstring token_text =
        L"入力する確認語:\r\n" + state->token;
    SetDlgItemTextW(dialog, IDC_CONFIRM_TOKEN, token_text.c_str());
    SetDlgItemTextW(
        dialog,
        IDOK,
        state->confirm_button_label.c_str());
    SendDlgItemMessageW(
        dialog, IDC_CONFIRM_EDIT, EM_SETLIMITTEXT, 1024, 0);
    ShowWindow(
        GetDlgItem(dialog, IDC_CONFIRM_AUTO_ONCE),
        state->offer_auto_execute_once ? SW_SHOW : SW_HIDE);
    SendDlgItemMessageW(
        dialog, IDC_CONFIRM_AUTO_ONCE, BM_SETCHECK, BST_UNCHECKED, 0);
    EnableWindow(GetDlgItem(dialog, IDOK), FALSE);
    return TRUE;
  }
  if (message == WM_COMMAND && state != nullptr) {
    const int identifier = LOWORD(wparam);
    if (identifier == IDC_CONFIRM_EDIT &&
        HIWORD(wparam) == EN_CHANGE) {
      const int length =
          GetWindowTextLengthW(GetDlgItem(dialog, IDC_CONFIRM_EDIT));
      std::vector<wchar_t> text(
          static_cast<std::size_t>((std::max)(length, 0)) + 1,
          L'\0');
      GetDlgItemTextW(
          dialog,
          IDC_CONFIRM_EDIT,
          text.data(),
          static_cast<int>(text.size()));
      EnableWindow(
          GetDlgItem(dialog, IDOK),
          std::wstring_view(text.data()) == state->token ? TRUE : FALSE);
      return TRUE;
    }
    if (identifier == IDOK) {
      if (IsWindowEnabled(GetDlgItem(dialog, IDOK)) != FALSE) {
        state->auto_execute_once_selected =
            state->offer_auto_execute_once &&
            SendDlgItemMessageW(
                dialog, IDC_CONFIRM_AUTO_ONCE, BM_GETCHECK, 0, 0) ==
                BST_CHECKED;
        EndDialog(dialog, IDOK);
      }
      return TRUE;
    }
    if (identifier == IDCANCEL) {
      EndDialog(dialog, IDCANCEL);
      return TRUE;
    }
  }
  return FALSE;
}

bool process_is_elevated() {
  HANDLE token{};
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
    return false;
  }
  TOKEN_ELEVATION elevation{};
  DWORD returned{};
  const BOOL result = GetTokenInformation(
      token,
      TokenElevation,
      &elevation,
      static_cast<DWORD>(sizeof(elevation)),
      &returned);
  CloseHandle(token);
  return result != FALSE && elevation.TokenIsElevated != 0;
}

void set_control_font(const HWND control, const HFONT font) {
  SendMessageW(
      control,
      WM_SETFONT,
      reinterpret_cast<WPARAM>(font),
      static_cast<LPARAM>(TRUE));
}

void draw_text(
    HDC dc,
    const std::wstring_view text,
    RECT area,
    const COLORREF color,
    const UINT format,
    const HFONT font) {
  const auto previous_font =
      static_cast<HFONT>(SelectObject(dc, font));
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, color);
  DrawTextW(
      dc,
      text.data(),
      static_cast<int>(text.size()),
      &area,
      format);
  SelectObject(dc, previous_font);
}

void fill_rounded_rect(
    HDC dc,
    const RECT area,
    const COLORREF fill,
    const COLORREF border,
    const int radius = 14) {
  const HBRUSH brush = CreateSolidBrush(fill);
  const HPEN pen = CreatePen(PS_SOLID, 1, border);
  const auto old_brush = SelectObject(dc, brush);
  const auto old_pen = SelectObject(dc, pen);
  RoundRect(dc, area.left, area.top, area.right, area.bottom, radius, radius);
  SelectObject(dc, old_pen);
  SelectObject(dc, old_brush);
  DeleteObject(pen);
  DeleteObject(brush);
}

void draw_progress_metric(
    const AppState& state,
    HDC dc,
    const RECT& area,
    const std::wstring_view label,
    const std::wstring_view value,
    const COLORREF accent,
    const COLORREF fill) {
  fill_rounded_rect(dc, area, fill, accent, 10);
  RECT accent_bar = area;
  accent_bar.right = accent_bar.left + 4;
  const HBRUSH accent_brush = CreateSolidBrush(accent);
  FillRect(dc, &accent_bar, accent_brush);
  DeleteObject(accent_brush);
  RECT label_area{
      area.left + 12,
      area.top + 7,
      area.right - 8,
      area.top + 27};
  draw_text(
      dc,
      label,
      label_area,
      accent,
      DT_LEFT | DT_SINGLELINE | DT_TOP,
      state.small_font);
  RECT value_area{
      area.left + 12,
      area.top + 29,
      area.right - 8,
      area.bottom - 5};
  draw_text(
      dc,
      value,
      value_area,
      kInk,
      DT_LEFT | DT_SINGLELINE | DT_TOP | DT_END_ELLIPSIS,
      state.small_font);
}

void draw_strands(HDC dc, const int left, const int top, const int width) {
  const HPEN blue = CreatePen(PS_SOLID, 3, kTsumugiBlue);
  const HPEN purple = CreatePen(PS_SOLID, 3, kTsumugiPurple);
  const HPEN green = CreatePen(PS_SOLID, 3, kSafeGreen);
  const auto previous = SelectObject(dc, blue);
  MoveToEx(dc, left, top + 8, nullptr);
  for (int x = 0; x <= width; x += 8) {
    const int y = top + 8 + ((x / 8) % 4 < 2 ? -3 : 3);
    LineTo(dc, left + x, y);
  }
  SelectObject(dc, purple);
  MoveToEx(dc, left, top + 14, nullptr);
  for (int x = 0; x <= width; x += 8) {
    const int y = top + 14 + ((x / 8) % 4 < 2 ? 3 : -3);
    LineTo(dc, left + x, y);
  }
  SelectObject(dc, green);
  MoveToEx(dc, left, top + 20, nullptr);
  for (int x = 0; x <= width; x += 8) {
    const int y = top + 20 + ((x / 8) % 4 < 2 ? -2 : 2);
    LineTo(dc, left + x, y);
  }
  SelectObject(dc, previous);
  DeleteObject(green);
  DeleteObject(purple);
  DeleteObject(blue);
}

void layout_controls(AppState& state) {
  RECT client{};
  GetClientRect(state.window, &client);
  const int nav_width = 250;
  const int nav_left = 22;
  const int nav_button_width = nav_width - 44;
  int nav_top = 156;
  for (const HWND button : state.navigation) {
    MoveWindow(button, nav_left, nav_top, nav_button_width, 44, TRUE);
    nav_top += 52;
  }

  const auto clone_layout =
      ytec::windowsapp::calculate_clone_column_layout(client.right);
  MoveWindow(state.refresh, client.right - 152, 34, 116, 36, TRUE);
  MoveWindow(
      state.transfer_mode_combo,
      client.right - 302,
      76,
      266,
      160,
      TRUE);
  MoveWindow(
      state.source_combo,
      clone_layout.source_control.left,
      236,
      clone_layout.source_control.width(),
      280,
      TRUE);
  MoveWindow(
      state.target_combo,
      clone_layout.target_control.left,
      236,
      clone_layout.target_control.width(),
      280,
      TRUE);
  MoveWindow(
      state.restore_change_image,
      client.right - 476,
      client.bottom - 72,
      202,
      42,
      TRUE);
  MoveWindow(
      state.primary_action,
      client.right - 250,
      client.bottom - 72,
      214,
      42,
      TRUE);
}

void update_navigation_state(AppState& state) {
  for (std::size_t index = 0; index < state.navigation.size(); ++index) {
    InvalidateRect(state.navigation[index], nullptr, TRUE);
  }
}

std::optional<std::size_t> combo_selection(const HWND combo) {
  const LRESULT selection = SendMessageW(combo, CB_GETCURSEL, 0, 0);
  if (selection == CB_ERR) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(selection);
}

std::wstring control_text(const HWND control) {
  const int length = GetWindowTextLengthW(control);
  if (length <= 0) {
    return {};
  }
  std::wstring value(static_cast<std::size_t>(length) + 1U, L'\0');
  const int copied = GetWindowTextW(
      control, value.data(), static_cast<int>(value.size()));
  if (copied <= 0) {
    return {};
  }
  value.resize(static_cast<std::size_t>(copied));
  return value;
}

ytec::windowsapp::RescueMediaKind selected_media_kind(
    const AppState& state) {
  return SendMessageW(
             state.media_kind_combo, CB_GETCURSEL, 0, 0) == 1
             ? ytec::windowsapp::RescueMediaKind::usb_drive
             : ytec::windowsapp::RescueMediaKind::iso_file;
}

ytec::imageformat::TransferMode selected_transfer_mode(
    const AppState& state) {
  return SendMessageW(
             state.transfer_mode_combo, CB_GETCURSEL, 0, 0) == 1
             ? ytec::imageformat::TransferMode::shrink
             : ytec::imageformat::TransferMode::exact;
}

ytec::windowsapp::RescueMediaBootProfile selected_media_profile(
    const AppState& state) {
  return SendMessageW(
             state.media_profile_combo, CB_GETCURSEL, 0, 0) == 1
             ? ytec::windowsapp::RescueMediaBootProfile::
                   windows_uefi_2023_ca
             : ytec::windowsapp::RescueMediaBootProfile::
                   windows_uefi_2011_ca;
}

ytec::windowsapp::RescueMediaPlanView current_rescue_media_plan(
    const AppState& state) {
  return ytec::windowsapp::evaluate_rescue_media_plan({
      .preflight = state.media_preflight.has_value()
                       ? &state.media_preflight.value()
                       : nullptr,
      .kind = selected_media_kind(state),
      .boot_profile = selected_media_profile(state),
      .iso_destination = control_text(state.media_output_edit),
      .inventory = state.inventory.has_value()
                       ? &state.inventory.value()
                       : nullptr,
      .usb_target_index = combo_selection(state.target_combo),
      .inventory_loading = state.inventory_loading.load(),
  });
}

ytec::windowsapp::CloneSelectionView current_clone_selection(
    const AppState& state) {
  return ytec::windowsapp::evaluate_clone_selection(
      state.inventory.has_value() ? &state.inventory.value() : nullptr,
      combo_selection(state.source_combo),
      combo_selection(state.target_combo),
      state.inventory_loading.load(),
      selected_transfer_mode(state) ==
          ytec::imageformat::TransferMode::exact);
}

ytec::windowsapp::RestoreTargetSelectionView
current_restore_target_selection(const AppState& state) {
  return ytec::windowsapp::evaluate_restore_target_selection(
      state.restore_preflight.has_value()
          ? &state.restore_preflight.value()
          : nullptr,
      state.inventory.has_value() ? &state.inventory.value() : nullptr,
      combo_selection(state.target_combo),
      state.inventory_loading.load());
}

void select_default_restore_target(AppState& state) {
  SendMessageW(
      state.target_combo,
      CB_SETCURSEL,
      static_cast<WPARAM>(-1),
      0);
  if (!state.restore_preflight.has_value() ||
      !state.inventory.has_value() ||
      state.inventory_loading.load()) {
    return;
  }

  for (std::size_t index = 0;
       index < state.inventory->disks.size();
       ++index) {
    const auto candidate =
        ytec::windowsapp::evaluate_restore_target_selection(
            &state.restore_preflight.value(),
            &state.inventory.value(),
            index,
            false);
    if (candidate.ready_for_confirmation) {
      SendMessageW(
          state.target_combo,
          CB_SETCURSEL,
          static_cast<WPARAM>(index),
          0);
      return;
    }
  }
}

void select_default_media_usb_target(AppState& state) {
  SendMessageW(
      state.target_combo,
      CB_SETCURSEL,
      static_cast<WPARAM>(-1),
      0);
  if (!state.media_preflight.has_value() ||
      !state.inventory.has_value() ||
      state.inventory_loading.load()) {
    return;
  }

  for (std::size_t index = 0;
       index < state.inventory->disks.size();
       ++index) {
    const auto candidate =
        ytec::windowsapp::evaluate_rescue_media_plan({
            .preflight = &state.media_preflight.value(),
            .kind =
                ytec::windowsapp::RescueMediaKind::usb_drive,
            .boot_profile = selected_media_profile(state),
            .inventory = &state.inventory.value(),
            .usb_target_index = index,
        });
    if (candidate.ready_for_confirmation) {
      SendMessageW(
          state.target_combo,
          CB_SETCURSEL,
          static_cast<WPARAM>(index),
          0);
      return;
    }
  }
}

void update_action_state(AppState& state);

void start_media_preflight(AppState& state) {
  if (state.media_preflight_running.exchange(true)) {
    return;
  }
  if (state.logger.has_value()) {
    state.logger->info(
        L"レスキューメディア作成環境の読み取り専用診断開始");
  }
  state.media_preflight.reset();
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);

  const HWND window = state.window;
  state.media_preflight_thread = std::thread([window]() {
    auto payload = std::make_unique<MediaPreflightPayload>();
    payload->view =
        ytec::windowsapp::inspect_local_windows_media_environment();
    if (PostMessageW(
            window,
            kMediaPreflightCompleteMessage,
            0,
            reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
      static_cast<void>(payload.release());
    }
  });
}

void open_official_adk_page(
    AppState& state,
    const wchar_t* const url,
    const std::wstring_view page_name) {
  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
      state.window,
      L"open",
      url,
      nullptr,
      nullptr,
      SW_SHOWNORMAL));
  if (result <= 32) {
    MessageBoxW(
        state.window,
        (std::wstring(page_name) +
         L"を既定のブラウザーで開けませんでした。\n\n" + url)
            .c_str(),
        kWindowTitle,
        MB_OK | MB_ICONERROR);
    return;
  }
  if (state.logger.has_value()) {
    state.logger->info(
        L"Microsoft公式ADK案内を既定ブラウザーで表示 page=" +
        std::wstring(page_name));
  }
}

void show_adk_install_guide(AppState& state) {
  constexpr TASKDIALOG_BUTTON kButtons[]{
      {kAdkGuideDownloadPageId,
       L"1. Microsoft公式 ADK／WinPE ダウンロードページを開く"},
      {kAdkGuideServicingPageId,
       L"2. Microsoft公式 必須更新ページを開く"},
      {kAdkGuideRecheckId,
       L"3. インストール後に作成環境を再確認する"},
  };
  TASKDIALOGCONFIG config{};
  config.cbSize = sizeof(config);
  config.hwndParent = state.window;
  config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION |
                   TDF_SIZE_TO_CONTENT;
  config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
  config.pszWindowTitle = L"ADK／WinPE 導入ガイド";
  config.pszMainIcon = TD_INFORMATION_ICON;
  config.pszMainInstruction =
      L"Microsoft公式ツールを3段階で準備します";
  config.pszContent =
      L"① ADK 10.1.26100.2454を入れ、機能は最低限「Deployment Tools」を選びます。\n"
      L"② 同じ版のWindows PE add-onを入れます。\n"
      L"③ ADK 10.1.26100.2454 Update KB5101684を適用します。\n\n"
      L"ダウンロード、ライセンス同意、UAC、インストールはMicrosoft公式画面で行います。"
      L"Tsumugi DriveはMicrosoft製ファイルを同梱・代理配布しません。";
  config.cButtons = static_cast<UINT>(std::size(kButtons));
  config.pButtons = kButtons;
  config.nDefaultButton = kAdkGuideDownloadPageId;

  using TaskDialogIndirectFunction = HRESULT(WINAPI*)(
      const TASKDIALOGCONFIG*, int*, int*, BOOL*);
  HMODULE common_controls = LoadLibraryW(L"comctl32.dll");
  TaskDialogIndirectFunction task_dialog{};
  if (common_controls != nullptr) {
    task_dialog = reinterpret_cast<TaskDialogIndirectFunction>(
        GetProcAddress(common_controls, "TaskDialogIndirect"));
    if (task_dialog == nullptr) {
      task_dialog = reinterpret_cast<TaskDialogIndirectFunction>(
          GetProcAddress(
              common_controls,
              MAKEINTRESOURCEA(345)));
    }
  }

  int pressed{};
  const HRESULT result = task_dialog == nullptr
      ? HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)
      : task_dialog(&config, &pressed, nullptr, nullptr);
  if (common_controls != nullptr) {
    FreeLibrary(common_controls);
  }
  if (FAILED(result)) {
    const int fallback = MessageBoxW(
        state.window,
        L"Microsoft公式のADK／WinPE導入ページを開きますか？\n\n"
        L"ADK 10.1.26100.2454（Deployment Tools）と同じ版のWinPE Add-on、"
        L"KB5101684が必要です。",
        L"ADK／WinPE 導入ガイド",
        MB_YESNO | MB_ICONINFORMATION);
    if (fallback == IDYES) {
      open_official_adk_page(
          state, kOfficialAdkInstallGuideUrl, L"ADK／WinPE導入ページ");
    }
    return;
  }
  if (pressed == kAdkGuideDownloadPageId) {
    open_official_adk_page(
        state, kOfficialAdkInstallGuideUrl, L"ADK／WinPE導入ページ");
  } else if (pressed == kAdkGuideServicingPageId) {
    open_official_adk_page(
        state, kOfficialAdkServicingGuideUrl, L"ADK必須更新ページ");
  } else if (pressed == kAdkGuideRecheckId) {
    start_media_preflight(state);
  }
}

void choose_media_iso_destination(AppState& state) {
  std::vector<wchar_t> path(32U * 1024U, L'\0');
  const std::wstring current = control_text(state.media_output_edit);
  const std::wstring initial =
      current.empty() ? L"Tsumugi-Drive-Rescue.iso" : current;
  std::copy(initial.begin(), initial.end(), path.begin());
  constexpr wchar_t kFilter[] =
      L"ISOイメージ (*.iso)\0*.iso\0\0";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(OPENFILENAMEW);
  dialog.hwndOwner = state.window;
  dialog.lpstrFilter = kFilter;
  dialog.nFilterIndex = 1;
  dialog.lpstrFile = path.data();
  dialog.nMaxFile = static_cast<DWORD>(path.size());
  dialog.lpstrDefExt = L"iso";
  dialog.lpstrTitle = L"新しく作成するレスキューISOの保存先";
  dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST |
                 OFN_NOCHANGEDIR | OFN_DONTADDTORECENT |
                 OFN_NOREADONLYRETURN;
  if (GetSaveFileNameW(&dialog) == FALSE) {
    const DWORD dialog_error = CommDlgExtendedError();
    if (dialog_error != 0) {
      MessageBoxW(
          state.window,
          L"ISO保存先の選択画面を開けませんでした。",
          kWindowTitle,
          MB_OK | MB_ICONERROR);
    }
    return;
  }

  const std::wstring selected(path.data());
  if (!ytec::windowsapp::is_safe_iso_destination_syntax(selected)) {
    MessageBoxW(
        state.window,
        L"ISOはローカルドライブ上の新しい絶対パス（.iso）を指定してください。",
        L"ISO保存先を使用できません",
        MB_OK | MB_ICONWARNING);
    return;
  }
  const DWORD attributes = GetFileAttributesW(selected.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    MessageBoxW(
        state.window,
        L"既存ファイルは上書きしません。別の新しいISO名を指定してください。",
        L"ISO保存先を使用できません",
        MB_OK | MB_ICONWARNING);
    return;
  }
  if (GetLastError() != ERROR_FILE_NOT_FOUND) {
    MessageBoxW(
        state.window,
        L"ISO保存先が未使用であることを安全に確認できませんでした。",
        L"ISO保存先を使用できません",
        MB_OK | MB_ICONWARNING);
    return;
  }

  SetWindowTextW(state.media_output_edit, selected.c_str());
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);
}

void start_rescue_media_creation(
    AppState& state,
    const ytec::windowsapp::RescueMediaPlanView& plan) {
  if (state.media_creation_running.load()) {
    return;
  }
  const auto kind = selected_media_kind(state);
  if (!state.elevated) {
    const std::wstring media_name =
        kind == ytec::windowsapp::RescueMediaKind::usb_drive
            ? L"USB"
            : L"ISO";
    const std::wstring administrator_message =
        L"作成内容は確認できました。\n\n"
        L"実際のWIM構成と" + media_name +
        L"作成には管理者権限が必要です。"
        L"\nこの画面からUACを自動表示せず、後ほど管理者として起動して"
        L"同じ作成先を選択してください。";
    const std::wstring administrator_title =
        media_name + L"作成は管理者確認待ちです";
    MessageBoxW(
        state.window,
        administrator_message.c_str(),
        administrator_title.c_str(),
        MB_OK | MB_ICONINFORMATION);
    return;
  }

  const std::wstring final_path = control_text(state.media_output_edit);
  std::optional<ytec::windowsapp::RescueUsbTargetAuthorization>
      usb_authorization;
  std::optional<ytec::windowsapp::RescueUsbDriveLetterResolution>
      usb_mapping;
  std::wstring confirmation =
      L"手順 3/4  最終確認\n\n" + plan.summary;
  if (kind == ytec::windowsapp::RescueMediaKind::usb_drive) {
    confirmation +=
        L"\n\nローカルADKのMicrosoft公式作成処理で、選択USBの"
        L"ディスク全体を消去し、MBR／単一FAT32パーティションへ自動初期化します。"
        L"\n選択していないディスクやパーティションは処理しません。"
        L"\n作成後は全ファイルを読み戻してSHA-256を照合します。"
        L"\n\nこのUSBの全内容を消去して続けますか？";
  } else {
    confirmation +=
        L"\n\nローカルADKから新しいISOを作成します。"
        L"\n既存ファイルは上書きせず、完成後にISO全体のSHA-256を検証します。"
        L"\n\n作成を開始しますか？";
  }
  if (MessageBoxW(
          state.window,
          confirmation.c_str(),
          kind == ytec::windowsapp::RescueMediaKind::usb_drive
              ? L"レスキューUSBの最終確認 1/2"
              : L"レスキューISOの最終確認",
          MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    return;
  }

  if (kind == ytec::windowsapp::RescueMediaKind::usb_drive) {
    if (!plan.usb_target_identity.has_value() ||
        plan.confirmation_token.empty()) {
      MessageBoxW(
          state.window,
          L"USBの安定識別情報または対象固有の確認語がありません。"
          L"\n診断情報を更新して、USBを選び直してください。",
          kWindowTitle,
          MB_OK | MB_ICONERROR);
      return;
    }
    const auto& expected = plan.usb_target_identity.value();
    const std::wstring serial =
        expected.serial_suffix.empty()
            ? L"取得できません"
            : std::wstring(
                  expected.serial_suffix.begin(),
                  expected.serial_suffix.end());
    ConfirmationDialogState dialog_state{
        .details =
            L"作成先: ディスク " +
            std::to_wstring(expected.disk_number) + L" / " +
            expected.model + L"\r\n容量: " +
            format_bytes(expected.size_bytes) +
            L" / シリアル末尾: " + serial +
            L"\r\n削除対象: USBディスク全体（既存パーティションを含む全内容）"
            L"\r\n作成構成: MBR / 単一FAT32パーティション",
        .token = plan.confirmation_token,
        .confirm_button_label = L"USB作成を開始",
        .font = state.body_font,
    };
    const INT_PTR dialog_result = DialogBoxParamW(
        GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(IDD_CLONE_CONFIRMATION),
        state.window,
        confirmation_dialog_proc,
        reinterpret_cast<LPARAM>(&dialog_state));
    if (dialog_result != IDOK) {
      if (dialog_result == -1) {
        MessageBoxW(
            state.window,
            L"USB確認画面を開けませんでした。",
            kWindowTitle,
            MB_OK | MB_ICONERROR);
      }
      return;
    }

    auto inventory_provider =
        ytec::diskmodel::make_windows_disk_inventory_provider(
            state.logger.has_value() ? &state.logger.value() : nullptr);
    auto fresh_inventory = inventory_provider->enumerate();
    if (!fresh_inventory) {
      const std::wstring error =
          format_error_message(fresh_inventory.error());
      MessageBoxW(
          state.window,
          error.c_str(),
          L"USBを再確認できませんでした",
          MB_OK | MB_ICONERROR);
      return;
    }
    auto authorization =
        ytec::windowsapp::authorize_rescue_usb_target({
            .expected_target = expected,
            .fresh_inventory = &fresh_inventory.value(),
            .first_step_acknowledged = true,
            .typed_confirmation = plan.confirmation_token,
        });
    if (!authorization) {
      const std::wstring error =
          format_error_message(authorization.error());
      MessageBoxW(
          state.window,
          error.c_str(),
          L"USBの再確認で停止しました",
          MB_OK | MB_ICONWARNING);
      return;
    }
    const auto target = std::find_if(
        fresh_inventory.value().disks.begin(),
        fresh_inventory.value().disks.end(),
        [&](const auto& disk) {
          return disk.disk_number ==
                 authorization.value().target.disk_number;
        });
    if (target == fresh_inventory.value().disks.end()) {
      MessageBoxW(
          state.window,
          L"確認済みUSBを再列挙結果から取得できません。",
          L"USBの再確認で停止しました",
          MB_OK | MB_ICONWARNING);
      return;
    }
    auto mapping =
        ytec::windowsapp::
            resolve_windows_rescue_usb_drive_letter_read_only(*target);
    if (!mapping) {
      const std::wstring error =
          format_error_message(mapping.error());
      MessageBoxW(
          state.window,
          error.c_str(),
          L"USBのドライブ文字を確定できません",
          MB_OK | MB_ICONWARNING);
      return;
    }
    usb_authorization = authorization.take_value();
    usb_mapping = mapping.take_value();
  }

  state.media_creation_cancel_requested.store(false);
  state.media_creation_progress.reset();
  state.media_creation_report.reset();
  state.last_logged_media_stage.reset();
  state.media_creation_started_tick = GetTickCount64();
  state.media_creation_running.store(true);
  if (state.logger.has_value()) {
    state.logger->info(
        L"レスキューメディア作成開始 kind=" +
        std::wstring(
            kind == ytec::windowsapp::RescueMediaKind::usb_drive
                ? L"usb"
                : L"iso") +
        L" boot_profile=" +
        std::wstring(
            ytec::windowsapp::rescue_media_boot_profile_label(
                selected_media_profile(state))));
  }
  SetTimer(state.window, kUiRefreshTimerId, 1000U, nullptr);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);

  const HWND window = state.window;
  auto request = ytec::windowsapp::RescueMediaCreationRequest{
      .kind = kind,
      .boot_profile = selected_media_profile(state),
      .final_iso_path = final_path,
      .administrator = state.elevated,
      .usb_authorization = std::move(usb_authorization),
      .usb_mapping = std::move(usb_mapping),
      .callbacks =
          {
              .progress =
                  [window](const auto& progress) {
                    auto payload =
                        std::make_unique<MediaCreationProgressPayload>();
                    payload->progress = progress;
                    if (PostMessageW(
                            window,
                            kMediaCreationProgressMessage,
                            0,
                            reinterpret_cast<LPARAM>(
                                payload.get())) != FALSE) {
                      static_cast<void>(payload.release());
                    }
                  },
              .cancellation_requested =
                  [&state] {
                    return state.media_creation_cancel_requested.load();
                  },
          },
  };
  state.media_creation_thread = std::thread(
      [window, request = std::move(request)]() {
        auto payload = std::make_unique<MediaCreationPayload>();
        auto result =
            ytec::windowsapp::
                execute_rescue_media_creation_with_windows_apis(request);
        if (result) {
          payload->report = result.take_value();
        } else {
          payload->error = result.error();
        }
        if (PostMessageW(
                window,
                kMediaCreationCompleteMessage,
                0,
                reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
          static_cast<void>(payload.release());
        }
      });
}

void review_rescue_media_plan(AppState& state) {
  const auto plan = current_rescue_media_plan(state);
  if (!plan.ready_for_confirmation) {
    return;
  }

  std::wstring message =
      L"手順 3/4  作成内容の確認\n\n" + plan.summary;
  if (selected_media_kind(state) ==
      ytec::windowsapp::RescueMediaKind::usb_drive) {
    const auto target_index = combo_selection(state.target_combo);
    if (!state.inventory.has_value() ||
        !target_index.has_value() ||
        target_index.value() >= state.inventory->disks.size()) {
      return;
    }
    const auto mapping =
        ytec::windowsapp::
            resolve_windows_rescue_usb_drive_letter_read_only(
                state.inventory->disks[target_index.value()]);
    if (!mapping) {
      log_error_summary(
          state.logger,
          L"USB物理ディスクとドライブ文字の照合失敗",
          mapping.error());
      const std::wstring failure =
          L"選択したUSBとドライブ文字を一意に照合できないため、"
          L"安全側に停止しました。\n\n" +
          mapping.error().message +
          L"\n\nUSBの抜き差し後に「診断情報を更新」して再確認してください。";
      MessageBoxW(
          state.window,
          failure.c_str(),
          L"USBの安全照合で停止しました",
          MB_OK | MB_ICONWARNING);
      return;
    }
    message +=
        L"\n\n読み取り専用照合: ディスク " +
        std::to_wstring(
            mapping.value().target_identity.disk_number) +
        L" ↔ " + mapping.value().root_path +
        L"（パーティション " +
        std::to_wstring(mapping.value().partition_number) + L"）";
    if (state.logger.has_value()) {
      state.logger->info(
          L"USB物理ディスクとドライブ文字の読み取り専用照合完了 disk=" +
          std::to_wstring(
              mapping.value().target_identity.disk_number) +
          L" drive_letter=" +
          std::wstring(1U, mapping.value().drive_letter) +
          L" physical_write_started=false");
    }
  }
  if (!plan.confirmation_token.empty()) {
    message +=
        L"\n\n実行時に入力する対象固有の確認語:\n" +
        plan.confirmation_token;
  }
  message +=
      L"\n\nこの確認だけでは作成を開始しません。"
      L"\n続けて最終確認を表示します。";
  if (MessageBoxW(
          state.window,
          message.c_str(),
          L"レスキューメディアの作成内容",
          MB_OKCANCEL | MB_ICONINFORMATION) == IDOK) {
    start_rescue_media_creation(state, plan);
  }
}

void create_restore_preflight_flow(AppState& state) {
  if (state.restore_preflight_running.load()) {
    return;
  }

  std::vector<wchar_t> path(32U * 1024U, L'\0');
  constexpr wchar_t kFilter[] =
      L"Tsumugiイメージ (*.dcimg; manifest.dcmig)\0*.dcimg;manifest.dcmig\0"
      L"通常イメージ (*.dcimg)\0*.dcimg\0"
      L"縮小移行マニフェスト (manifest.dcmig)\0manifest.dcmig\0\0";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(OPENFILENAMEW);
  dialog.hwndOwner = state.window;
  dialog.lpstrFilter = kFilter;
  dialog.nFilterIndex = 1;
  dialog.lpstrFile = path.data();
  dialog.nMaxFile = static_cast<DWORD>(path.size());
  dialog.lpstrTitle =
      L"完全検証するdcimg、または.dcmigフォルダー内のmanifest.dcmigを選択";
  dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST |
                 OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                 OFN_DONTADDTORECENT;
  if (GetOpenFileNameW(&dialog) == FALSE) {
    const DWORD dialog_error = CommDlgExtendedError();
    if (dialog_error != 0) {
      const std::wstring message =
          L"イメージ選択画面でエラーが発生しました。\n"
          L"Common dialog error: " +
          std::to_wstring(dialog_error);
      MessageBoxW(
          state.window,
          message.c_str(),
          kWindowTitle,
          MB_OK | MB_ICONERROR);
    }
    return;
  }

  state.restore_preflight.reset();
  state.restore_preflight_cancel_requested.store(false);
  state.restore_preflight_running.store(true);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);

  const HWND window = state.window;
  const std::wstring selected_path(path.data());
  if (state.logger.has_value()) {
    state.logger->info(
        L"復元イメージの読み取り専用完全検証開始");
  }
  state.restore_preflight_thread = std::thread(
      [window, selected_path, &state]() {
        auto payload = std::make_unique<RestorePreflightPayload>();
        auto result = ytec::windowsapp::inspect_restore_image_file(
            selected_path,
            ytec::windowsapp::RestoreImagePreflightOptions{
                .cancellation_requested =
                    [&state]() {
                      return state
                          .restore_preflight_cancel_requested.load();
                    },
            });
        if (result) {
          payload->report = result.take_value();
        } else {
          payload->error = result.error();
        }
        if (PostMessageW(
                window,
                kRestorePreflightCompleteMessage,
                0,
                reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
          static_cast<void>(payload.release());
        }
      });
}

void offer_reboot_handoff(AppState& state) {
  const auto version = current_windows_version();
  if (!version) {
    if (state.logger.has_value()) {
      log_error_summary(
          state.logger, L"WinPE引き継ぎ再起動の環境確認失敗",
          version.error());
    }
    MessageBoxW(
        state.window,
        L"Windowsの起動オプション対応状況を確認できませんでした。\n"
        L"ジョブは保存済みです。作業を保存して通常再起動し、"
        L"電源投入直後のBoot Menuから作成済みWinPE USBを選んでください。",
        L"WinPEへの移動手順",
        MB_OK | MB_ICONINFORMATION);
    return;
  }
  const auto plan = ytec::windowsapp::build_reboot_handoff_plan(
      state.elevated, version.value()[0], version.value()[1]);
  if (plan.readiness !=
      ytec::windowsapp::RebootHandoffReadiness::ready) {
    MessageBoxW(
        state.window,
        plan.guidance.c_str(),
        L"WinPEへの移動手順",
        MB_OK | MB_ICONINFORMATION);
    return;
  }

  const std::wstring prompt =
      plan.guidance +
      L"\n\n今すぐMicrosoftの起動オプションへ再起動しますか？"
      L"\n開いている作業は先に保存してください。"
      L"\n他のアプリや別ユーザーを強制終了しません。";
  if (MessageBoxW(
          state.window,
          prompt.c_str(),
          L"WinPE起動オプションへ移動",
          MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    return;
  }

  auto service = ytec::windowsapp::make_windows_reboot_handoff_service();
  const auto requested = ytec::windowsapp::request_reboot_handoff(
      plan, *service);
  if (!requested) {
    log_error_summary(
        state.logger, L"WinPE引き継ぎ再起動要求失敗",
        requested.error());
    const std::wstring message =
        format_error_message(requested.error()) +
        L"\n\nジョブは保存済みです。通常再起動とBoot Menuから"
        L"作成済みWinPE USBを選んでください。";
    MessageBoxW(
        state.window,
        message.c_str(),
        L"WinPEへの再起動を開始できませんでした",
        MB_OK | MB_ICONERROR);
    return;
  }
  if (state.logger.has_value()) {
    state.logger->info(
        L"Microsoft起動オプションへの計画再起動を要求しました");
  }
}

void create_clone_job_flow(AppState& state) {
  const auto selection = current_clone_selection(state);
  const auto source_index = combo_selection(state.source_combo);
  const auto target_index = combo_selection(state.target_combo);
  if (!selection.ready || !state.inventory.has_value() ||
      !source_index.has_value() || !target_index.has_value() ||
      source_index.value() >= state.inventory->disks.size() ||
      target_index.value() >= state.inventory->disks.size()) {
    MessageBoxW(
        state.window,
        L"コピー元とコピー先をもう一度確認してください。",
        kWindowTitle,
        MB_OK | MB_ICONWARNING);
    return;
  }
  const auto& source = state.inventory->disks[source_index.value()];
  const auto& target = state.inventory->disks[target_index.value()];
  const auto transfer_mode = selected_transfer_mode(state);
  const bool shrink_mode =
      transfer_mode == ytec::imageformat::TransferMode::shrink;
  std::wstring shrink_bundle_directory;
  if (shrink_mode) {
    std::vector<wchar_t> bundle_path(32U * 1024U, L'\0');
    constexpr std::wstring_view kDefaultBundleName =
        L"Tsumugi-clone-staging.dcmig";
    std::copy(
        kDefaultBundleName.begin(),
        kDefaultBundleName.end(),
        bundle_path.begin());
    constexpr wchar_t kBundleFilter[] =
        L"Tsumugi縮小移行イメージ (manifest.dcmigを含むフォルダー)\0*.dcmig\0\0";
    OPENFILENAMEW bundle_dialog{};
    bundle_dialog.lStructSize = sizeof(OPENFILENAMEW);
    bundle_dialog.hwndOwner = state.window;
    bundle_dialog.lpstrFilter = kBundleFilter;
    bundle_dialog.nFilterIndex = 1;
    bundle_dialog.lpstrFile = bundle_path.data();
    bundle_dialog.nMaxFile = static_cast<DWORD>(bundle_path.size());
    bundle_dialog.lpstrDefExt = L"dcmig";
    bundle_dialog.lpstrTitle =
        L"原本・コピー先とは別の物理ディスクに作業イメージ名を指定";
    bundle_dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST |
                          OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    if (GetSaveFileNameW(&bundle_dialog) == FALSE) {
      const DWORD dialog_error = CommDlgExtendedError();
      if (dialog_error != 0U) {
        MessageBoxW(
            state.window,
            L"縮小移行の作業イメージ保存先を選べませんでした。",
            kWindowTitle,
            MB_OK | MB_ICONERROR);
      }
      return;
    }
    shrink_bundle_directory = bundle_path.data();
    const std::filesystem::path bundle(shrink_bundle_directory);
    if (!bundle.is_absolute() ||
        _wcsicmp(bundle.extension().c_str(), L".dcmig") != 0 ||
        GetFileAttributesW(shrink_bundle_directory.c_str()) !=
            INVALID_FILE_ATTRIBUTES) {
      MessageBoxW(
          state.window,
          L"未使用の絶対パスを.dcmig名で指定してください。"
          L"既存のファイルやフォルダーは上書きしません。",
          L"縮小移行の作業イメージ保存先",
          MB_OK | MB_ICONWARNING);
      return;
    }
  }
  ytec::imageformat::RequestedConversion requested_conversion =
      ytec::imageformat::RequestedConversion::preserve;
  if (!shrink_mode &&
      source.partition_style == ytec::diskmodel::PartitionStyle::mbr) {
    const int conversion_choice = MessageBoxW(
        state.window,
        L"コピー元はLegacy BIOS用のMBRディスクです。\n\n"
        L"［はい］ コピー先をGPT / UEFIへ移行\n"
        L"  まず別のコピー先へMBRのまま複製し、WinPEでMicrosoft MBR2GPTと"
        L"BCDBootを実行します。コピー元は変更しません。完了後はUEFI起動への切替が必要です。\n\n"
        L"［いいえ］ MBR / Legacy BIOSのままクローン\n\n"
        L"どちらにするか未確定なら［キャンセル］してください。",
        L"コピー先の起動方式を選択",
        MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON3);
    if (conversion_choice == IDCANCEL) {
      return;
    }
    if (conversion_choice == IDYES) {
      requested_conversion =
          ytec::imageformat::RequestedConversion::mbr_to_gpt;
    }
  }
  const bool mbr_to_gpt =
      requested_conversion ==
      ytec::imageformat::RequestedConversion::mbr_to_gpt;
  const std::wstring serial =
      target.serial_suffix.empty()
          ? L"取得できません"
          : std::wstring(
                target.serial_suffix.begin(), target.serial_suffix.end());
  const std::wstring first_confirmation =
      L"手順 1/2  コピー先を確認してください。\n\n"
      L"ディスク " + std::to_wstring(target.disk_number) + L"\n" +
      (target.model.empty() ? L"モデル不明" : target.model) + L"\n" +
      format_bytes(target.size_bytes) + L"\nシリアル末尾: " + serial +
      L"\n現在のパーティション数: " +
      std::to_wstring(target.partitions.size()) +
      L"\nモード: " +
      (shrink_mode ? L"縮小移行モード" : L"通常モード") +
      L"\n\nこのジョブはWinPEで実行します。実行時にはコピー先の全パーティションとデータが削除されます。" +
      (mbr_to_gpt
           ? std::wstring(
                 L"\nMBRクローン後、コピー先だけをGPT / UEFI構成へ変換します。")
           : std::wstring()) +
      (shrink_mode
           ? L"\n原本は読み取り専用のまま、第三ディスクへ検証済み作業イメージを作ってからコピー先だけを再構成します。"
           : L"") +
      L"\nWindows上ではディスクへ書き込まず、確認済みジョブだけを作成します。"
      L"\n\nこのコピー先でジョブ作成を続けますか？";
  if (MessageBoxW(
          state.window,
          first_confirmation.c_str(),
          L"コピー先の安全確認 1/2",
          MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    return;
  }

  const std::wstring token =
      ytec::windowsapp::clone_job_confirmation_token(target);
  if (token.empty()) {
    MessageBoxW(
        state.window,
        L"確認語を準備できません。"
        L"\nディスク情報を再読み込みしてください。",
        kWindowTitle,
        MB_OK | MB_ICONERROR);
    return;
  }
  ConfirmationDialogState dialog_state{
      .details =
          L"コピー先: ディスク " +
          std::to_wstring(target.disk_number) + L" / " +
          (target.model.empty() ? L"モデル不明" : target.model) +
          L"\r\n容量: " + format_bytes(target.size_bytes) +
          L" / シリアル末尾: " + serial +
          L"\r\n削除対象: " +
          std::to_wstring(target.partitions.size()) +
          L" パーティション（すべて）",
      .token = token,
      .offer_auto_execute_once = !mbr_to_gpt,
      .font = state.body_font,
  };
  const INT_PTR dialog_result = DialogBoxParamW(
      GetModuleHandleW(nullptr),
      MAKEINTRESOURCEW(IDD_CLONE_CONFIRMATION),
      state.window,
      confirmation_dialog_proc,
      reinterpret_cast<LPARAM>(&dialog_state));
  if (dialog_result != IDOK) {
    if (dialog_result == -1) {
      MessageBoxW(
          state.window,
          L"確認画面を開けませんでした。",
          kWindowTitle,
          MB_OK | MB_ICONERROR);
    }
    return;
  }

  auto job = ytec::windowsapp::create_confirmed_clone_job(
      ytec::windowsapp::CloneJobCreationRequest{
          .source = source,
          .target = target,
          .first_step_acknowledged = true,
          .typed_confirmation = token,
          .requested_conversion = requested_conversion,
          .transfer_mode = transfer_mode,
          .shrink_bundle_directory = shrink_bundle_directory,
          .auto_execute_once = dialog_state.auto_execute_once_selected,
          .created_utc = current_utc_timestamp(),
          .app_version = std::string(kAppVersion),
      });
  if (!job) {
    log_error_summary(
        state.logger, L"クローンジョブ作成失敗", job.error());
    const std::wstring message = format_error_message(job.error());
    MessageBoxW(
        state.window,
        message.c_str(),
        kWindowTitle,
        MB_OK | MB_ICONERROR);
    return;
  }

  std::vector<wchar_t> path(32U * 1024U, L'\0');
  const std::wstring default_job_path = shrink_mode
      ? (std::filesystem::path(shrink_bundle_directory).parent_path() /
         L"Tsumugi-clone-job.json")
            .wstring()
      : L"Tsumugi-clone-job.json";
  std::copy(
      default_job_path.begin(), default_job_path.end(), path.begin());
  constexpr wchar_t kFilter[] =
      L"Tsumugiジョブ (*.json)\0*.json\0"
      L"すべてのファイル (*.*)\0*.*\0\0";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(OPENFILENAMEW);
  dialog.hwndOwner = state.window;
  dialog.lpstrFilter = kFilter;
  dialog.nFilterIndex = 1;
  dialog.lpstrFile = path.data();
  dialog.nMaxFile = static_cast<DWORD>(path.size());
  dialog.lpstrDefExt = L"json";
  dialog.lpstrTitle = mbr_to_gpt
      ? L"WinPEへ渡すMBRからGPT移行ジョブを新規保存"
      : L"WinPEへ渡すクローンジョブを新規保存";
  dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST |
                 OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
  if (GetSaveFileNameW(&dialog) == FALSE) {
    const DWORD dialog_error = CommDlgExtendedError();
    if (dialog_error != 0) {
      const std::wstring message =
          L"保存先選択画面でエラーが発生しました。\nCommon dialog error: " +
          std::to_wstring(dialog_error);
      MessageBoxW(
          state.window,
          message.c_str(),
          kWindowTitle,
          MB_OK | MB_ICONERROR);
    }
    return;
  }
  if (shrink_mode &&
      (_wcsnicmp(path.data(), shrink_bundle_directory.c_str(), 2) != 0)) {
    MessageBoxW(
        state.window,
        L"WinPEで同じ第三ディスクを確実に再解決するため、縮小移行ジョブは作業イメージと同じドライブへ保存してください。",
        L"縮小移行ジョブの保存先",
        MB_OK | MB_ICONWARNING);
    return;
  }
  const auto saved = ytec::imageformat::write_new_verified_job_file(
      path.data(), job.value());
  if (!saved) {
    log_error_summary(
        state.logger, L"クローンジョブ保存・読戻し検証失敗",
        saved.error());
    const std::wstring message =
        format_error_message(saved.error()) +
        L"\n\n既存ファイルは上書きしません。別の新しい名前を指定してください。";
    MessageBoxW(
        state.window,
        message.c_str(),
        kWindowTitle,
        MB_OK | MB_ICONERROR);
    return;
  }
  if (state.logger.has_value()) {
    state.logger->info(
        L"クローンジョブ保存・読戻し検証完了 source_disk=" +
        std::to_wstring(source.disk_number) + L" target_disk=" +
        std::to_wstring(target.disk_number) + L" execution_mode=" +
        (dialog_state.auto_execute_once_selected
             ? L"auto-once"
             : L"review-required"));
  }
  const std::wstring completed =
      (mbr_to_gpt
           ? std::wstring(
                 L"確認済みMBRからGPT移行ジョブを作成し、読戻し検証しました。\n\n")
           : std::wstring(
                 L"確認済みクローンジョブを作成し、読戻し検証しました。\n\n")) +
      std::wstring(path.data()) +
      (mbr_to_gpt
           ? std::wstring(L"\n\nまだクローンもGPT変換も開始していません。")
           : std::wstring(L"\n\nまだクローンは開始していません。")) +
      (dialog_state.auto_execute_once_selected
           ? L"\nWinPEで改ざん・ディスク再識別・安全条件に合格した場合だけ、"
             L"一回限りの開始記録を保存してから自動実行します。"
           : L"\nWinPEで安全確認後、確認語を入力して手動で開始します。");
  MessageBoxW(
      state.window,
      completed.c_str(),
      kWindowTitle,
      MB_OK | MB_ICONINFORMATION);
  offer_reboot_handoff(state);
}

void create_restore_job_flow(AppState& state) {
  const auto selection = current_restore_target_selection(state);
  const auto target_index = combo_selection(state.target_combo);
  if (!selection.ready_for_confirmation ||
      !state.restore_preflight.has_value() ||
      !state.inventory.has_value() ||
      !target_index.has_value() ||
      target_index.value() >= state.inventory->disks.size()) {
    MessageBoxW(
        state.window,
        L"検証済みイメージと復元先候補をもう一度確認してください。",
        kWindowTitle,
        MB_OK | MB_ICONWARNING);
    return;
  }
  const auto& report = state.restore_preflight.value();
  const bool shrink_mode = report.shrink_manifest.has_value();
  const auto& target = state.inventory->disks[target_index.value()];
  const std::wstring serial =
      target.serial_suffix.empty()
          ? L"取得できません"
          : std::wstring(
                target.serial_suffix.begin(), target.serial_suffix.end());
  const std::wstring first_confirmation =
      L"手順 1/2  復元先を確認してください。\n\n"
      L"ディスク " + std::to_wstring(target.disk_number) + L"\n" +
      (target.model.empty() ? L"モデル不明" : target.model) + L"\n" +
      format_bytes(target.size_bytes) + L"\nシリアル末尾: " + serial +
      L"\n現在のパーティション数: " +
      std::to_wstring(target.partitions.size()) +
      L"\n\n検証済みイメージ:\n" + report.canonical_path +
      L"\n元ディスク容量: " +
      format_bytes(report.header.source_disk_size) +
      L"\nモード: " +
      (shrink_mode ? L"縮小移行モード" : L"通常モード") +
      L"\n\n後でWinPEから実行すると、復元先の全パーティションと"
      L"データが削除されます。"
      L"\n今はディスクへ書き込まず、確認済みジョブだけを作成します。"
      L"\n再起動やUACも行いません。"
      L"\n\nこの復元先でジョブ作成を続けますか？";
  if (MessageBoxW(
          state.window,
          first_confirmation.c_str(),
          L"復元先の安全確認 1/2",
          MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    return;
  }

  const std::wstring token =
      ytec::windowsapp::restore_job_confirmation_token(target);
  if (token.empty()) {
    MessageBoxW(
        state.window,
        L"復元用の確認語を準備できません。"
        L"\nディスク情報を再読み込みしてください。",
        kWindowTitle,
        MB_OK | MB_ICONERROR);
    return;
  }
  ConfirmationDialogState dialog_state{
      .details =
          L"復元先: ディスク " +
          std::to_wstring(target.disk_number) + L" / " +
          (target.model.empty() ? L"モデル不明" : target.model) +
          L"\r\n容量: " + format_bytes(target.size_bytes) +
          L" / シリアル末尾: " + serial +
          L"\r\n削除対象: " +
          std::to_wstring(target.partitions.size()) +
          L" パーティション（すべて）",
      .token = token,
      .offer_auto_execute_once = true,
      .font = state.body_font,
  };
  const INT_PTR dialog_result = DialogBoxParamW(
      GetModuleHandleW(nullptr),
      MAKEINTRESOURCEW(IDD_CLONE_CONFIRMATION),
      state.window,
      confirmation_dialog_proc,
      reinterpret_cast<LPARAM>(&dialog_state));
  if (dialog_result != IDOK) {
    if (dialog_result == -1) {
      MessageBoxW(
          state.window,
          L"確認画面を開けませんでした。",
          kWindowTitle,
          MB_OK | MB_ICONERROR);
    }
    return;
  }

  auto job = ytec::windowsapp::create_confirmed_restore_job(
      ytec::windowsapp::RestoreJobCreationRequest{
          .target = target,
          .verified_image_path = report.canonical_path,
          .verified_image_length = report.image_length,
          .verified_image_global_hash = report.global_hash,
          .first_step_acknowledged = true,
          .typed_confirmation = token,
          .auto_execute_once = dialog_state.auto_execute_once_selected,
          .transfer_mode = shrink_mode
              ? ytec::imageformat::TransferMode::shrink
              : ytec::imageformat::TransferMode::exact,
          .created_utc = current_utc_timestamp(),
          .app_version = std::string(kAppVersion),
      });
  if (!job) {
    log_error_summary(
        state.logger, L"復元ジョブ作成失敗", job.error());
    const std::wstring message = format_error_message(job.error());
    MessageBoxW(
        state.window,
        message.c_str(),
        kWindowTitle,
        MB_OK | MB_ICONERROR);
    return;
  }

  std::vector<wchar_t> path(32U * 1024U, L'\0');
  constexpr std::wstring_view kDefaultName =
      L"Tsumugi-restore-job.json";
  std::copy(kDefaultName.begin(), kDefaultName.end(), path.begin());
  constexpr wchar_t kFilter[] =
      L"Tsumugiジョブ (*.json)\0*.json\0"
      L"すべてのファイル (*.*)\0*.*\0\0";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(OPENFILENAMEW);
  dialog.hwndOwner = state.window;
  dialog.lpstrFilter = kFilter;
  dialog.nFilterIndex = 1;
  dialog.lpstrFile = path.data();
  dialog.nMaxFile = static_cast<DWORD>(path.size());
  dialog.lpstrDefExt = L"json";
  dialog.lpstrTitle = L"WinPEへ渡す復元ジョブを新規保存";
  dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST |
                 OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
  if (GetSaveFileNameW(&dialog) == FALSE) {
    const DWORD dialog_error = CommDlgExtendedError();
    if (dialog_error != 0) {
      const std::wstring message =
          L"保存先選択画面でエラーが発生しました。\nCommon dialog error: " +
          std::to_wstring(dialog_error);
      MessageBoxW(
          state.window,
          message.c_str(),
          kWindowTitle,
          MB_OK | MB_ICONERROR);
    }
    return;
  }
  const auto saved = ytec::imageformat::write_new_verified_job_file(
      path.data(), job.value());
  if (!saved) {
    log_error_summary(
        state.logger, L"復元ジョブ保存・読戻し検証失敗",
        saved.error());
    const std::wstring message =
        format_error_message(saved.error()) +
        L"\n\n既存ファイルは上書きしません。別の新しい名前を指定してください。";
    MessageBoxW(
        state.window,
        message.c_str(),
        kWindowTitle,
        MB_OK | MB_ICONERROR);
    return;
  }
  if (state.logger.has_value()) {
    state.logger->info(
        L"復元ジョブ保存・読戻し検証完了 target_disk=" +
        std::to_wstring(target.disk_number) + L" image_bytes=" +
        std::to_wstring(report.image_length) + L" execution_mode=" +
        (dialog_state.auto_execute_once_selected
             ? L"auto-once"
             : L"review-required"));
  }
  const std::wstring completed =
      L"確認済み復元ジョブを作成し、読戻し検証しました。\n\n" +
      std::wstring(path.data()) +
      L"\n\nまだ復元は開始していません。"
      L"\n復元先ディスクは開いておらず、再起動やUACも行っていません。" +
      (dialog_state.auto_execute_once_selected
           ? L"\nWinPEでイメージ再検証・ディスク再識別・安全条件に合格した"
             L"場合だけ、一回限りの開始記録を保存してから自動実行します。"
           : L"\nWinPEで安全確認後、確認語を入力して手動で開始します。");
  MessageBoxW(
      state.window,
      completed.c_str(),
      kWindowTitle,
      MB_OK | MB_ICONINFORMATION);
  offer_reboot_handoff(state);
}

void create_online_backup_flow(AppState& state) {
  if (state.backup_running.load()) {
    return;
  }
  if (!state.elevated) {
    MessageBoxW(
        state.window,
        L"オンラインイメージ作成には管理者権限が必要です。"
        L"\nこの画面からUAC昇格は自動実行しません。"
        L"\n管理者実行の確認は後ほど一緒に行います。",
        kWindowTitle,
        MB_OK | MB_ICONINFORMATION);
    return;
  }
  const auto source_index = combo_selection(state.source_combo);
  if (!state.inventory.has_value() || !source_index.has_value() ||
      source_index.value() >= state.inventory->disks.size()) {
    MessageBoxW(
        state.window,
        L"バックアップするディスクを選択してください。",
        kWindowTitle,
        MB_OK | MB_ICONWARNING);
    return;
  }
  const auto& source = state.inventory->disks[source_index.value()];
  if (source.partition_style != ytec::diskmodel::PartitionStyle::gpt &&
      source.partition_style != ytec::diskmodel::PartitionStyle::mbr) {
    MessageBoxW(
        state.window,
        L"GPTまたはMBRの基本ディスクだけをバックアップできます。"
        L"不明な形式や動的ディスクは安全のため処理しません。",
        kWindowTitle,
        MB_OK | MB_ICONWARNING);
    return;
  }

  const auto transfer_mode = selected_transfer_mode(state);
  const bool shrink_mode =
      transfer_mode == ytec::imageformat::TransferMode::shrink;
  std::vector<wchar_t> path(32U * 1024U, L'\0');
  const std::wstring_view default_name = shrink_mode
      ? source.is_system_disk
            ? L"Tsumugi-system-backup.dcmig"
            : L"Tsumugi-data-backup.dcmig"
      : source.is_system_disk
            ? L"Tsumugi-system-backup.dcimg"
            : L"Tsumugi-data-backup.dcimg";
  std::copy(default_name.begin(), default_name.end(), path.begin());
  constexpr wchar_t kExactFilter[] =
      L"Tsumugi通常イメージ (*.dcimg)\0*.dcimg\0\0";
  constexpr wchar_t kShrinkFilter[] =
      L"Tsumugi縮小移行イメージ (*.dcmig)\0*.dcmig\0\0";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(OPENFILENAMEW);
  dialog.hwndOwner = state.window;
  dialog.lpstrFilter = shrink_mode ? kShrinkFilter : kExactFilter;
  dialog.nFilterIndex = 1;
  dialog.lpstrFile = path.data();
  dialog.nMaxFile = static_cast<DWORD>(path.size());
  dialog.lpstrDefExt = shrink_mode ? L"dcmig" : L"dcimg";
  dialog.lpstrTitle = L"新しいバックアップイメージの保存先";
  dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST |
                 OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
  if (GetSaveFileNameW(&dialog) == FALSE) {
    const DWORD dialog_error = CommDlgExtendedError();
    if (dialog_error != 0) {
      const std::wstring message =
          L"保存先選択画面でエラーが発生しました。\nCommon dialog error: " +
          std::to_wstring(dialog_error);
      MessageBoxW(
          state.window,
          message.c_str(),
          kWindowTitle,
          MB_OK | MB_ICONERROR);
    }
    return;
  }

  const auto pending_restart =
      ytec::windowsapp::query_windows_update_pending_restart();
  if (state.logger.has_value() &&
      pending_restart.state !=
          ytec::windowsapp::PendingRestartState::absent) {
    state.logger->warning(
        pending_restart.state ==
                ytec::windowsapp::PendingRestartState::required
            ? L"Windows Updateの再起動保留を検出"
            : L"Windows Updateの再起動保留状態は不明 native_status=" +
                  std::to_wstring(pending_restart.native_status));
  }
  std::wstring confirmation =
      L"次の読み取り専用コピー元からオンラインイメージを作成します。\n\n"
      L"ディスク " + std::to_wstring(source.disk_number) + L" / " +
      (source.model.empty() ? L"モデル不明" : source.model) +
      L"\n容量: " + format_bytes(source.size_bytes) +
      L"\n形式: " + partition_style_text(source.partition_style) +
      L"\nモード: " +
      (shrink_mode ? L"縮小移行モード" : L"通常モード") +
      L"\n\n保存先:\n" + std::wstring(path.data()) +
      L"\n\nコピー元ディスクへの書き込みは行いません。"
      L"\n既存の保存先は上書きしません。" +
      (shrink_mode
           ? L"\n各NTFS領域をVSS SnapshotからWIMへ保存し、小容量ディスクへ再配置できる形式にします。"
           : L"");
  confirmation += ytec::windowsapp::pending_restart_confirmation_note(
      pending_restart.state);
  confirmation += L"\n\n開始しますか？";
  if (MessageBoxW(
          state.window,
          confirmation.c_str(),
          L"オンラインイメージの確認",
          MB_YESNO |
              (pending_restart.state ==
                       ytec::windowsapp::PendingRestartState::absent
                   ? MB_ICONINFORMATION
                   : MB_ICONWARNING) |
              MB_DEFBUTTON2) != IDYES) {
    return;
  }

  const auto version = current_windows_version();
  const std::string architecture = current_native_architecture();
  if (!version || architecture.empty()) {
    const std::wstring message =
        version
            ? L"AMD64版Windowsとして安全に識別できませんでした。"
            : format_error_message(version.error());
    MessageBoxW(
        state.window,
        message.c_str(),
        kWindowTitle,
        MB_OK | MB_ICONERROR);
    return;
  }

  state.backup_cancel_requested.store(false);
  state.backup_progress.reset();
  state.backup_elapsed = std::chrono::milliseconds::zero();
  state.backup_running.store(true);
  const ULONGLONG backup_started_tick = GetTickCount64();
  auto last_progress_tick =
      std::make_shared<std::atomic<ULONGLONG>>(0);
  auto last_progress_stage =
      std::make_shared<std::atomic<std::uint8_t>>(
          static_cast<std::uint8_t>(0xFFU));
  if (state.logger.has_value()) {
    state.logger->info(
        L"オンラインイメージ作成開始 source_disk=" +
        std::to_wstring(source.disk_number) + L" source_bytes=" +
        std::to_wstring(source.size_bytes));
  }
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);
  const HWND window = state.window;
  const auto async_wait = ytec::vssrequester::AsyncWaitOptions{
      .timeout_ms = 120'000,
      .poll_interval_ms = 250,
      .cancellation_requested =
          [&state]() {
            return state.backup_cancel_requested.load();
          },
  };
  const auto callbacks = ytec::clonecore::DiskOperationCallbacks{
      .progress =
          [window,
           backup_started_tick,
           last_progress_tick,
           last_progress_stage](
              const ytec::clonecore::DiskOperationProgress& progress) {
            const ULONGLONG now = GetTickCount64();
            const auto stage = static_cast<std::uint8_t>(progress.stage);
            const bool stage_changed =
                last_progress_stage->exchange(stage) != stage;
            const ULONGLONG previous = last_progress_tick->load();
            if (!stage_changed &&
                progress.stage !=
                    ytec::clonecore::DiskOperationStage::completed &&
                now - previous < 200U) {
              return;
            }
            last_progress_tick->store(now);
            auto update = std::make_unique<BackupProgressPayload>();
            update->progress = progress;
            update->elapsed =
                std::chrono::milliseconds(now - backup_started_tick);
            if (PostMessageW(
                    window,
                    kBackupProgressMessage,
                    0,
                    reinterpret_cast<LPARAM>(update.get())) != FALSE) {
              static_cast<void>(update.release());
            }
          },
      .cancellation_requested =
          [&state]() {
            return state.backup_cancel_requested.load();
          },
  };
  const auto* logger =
      state.logger.has_value() ? &state.logger.value() : nullptr;
  if (shrink_mode) {
    auto request = ytec::windowsapp::OnlineShrinkBackupJobRequest{
        .selected_source = source,
        .final_bundle_directory = path.data(),
        .scratch_directory =
            std::filesystem::path(path.data()).parent_path().wstring(),
        .administrator = state.elevated,
        .windows_major = version.value()[0],
        .windows_minor = version.value()[1],
        .windows_build = version.value()[2],
        .windows_architecture = architecture,
        .created_utc = current_utc_timestamp(),
        .app_version = std::string(kAppVersion),
        .async_wait = async_wait,
        .callbacks = callbacks,
        .logger = logger,
    };
    state.backup_thread = std::thread(
        [window, request = std::move(request)]() {
          auto payload = std::make_unique<BackupPayload>();
          payload->final_path = request.final_bundle_directory;
          payload->transfer_mode = ytec::imageformat::TransferMode::shrink;
          auto result = ytec::windowsapp::
              execute_online_shrink_backup_job_with_windows_apis(request);
          if (result) {
            payload->shrink_report = result.take_value();
          } else {
            payload->error = result.error();
          }
          if (PostMessageW(
                  window,
                  kBackupCompleteMessage,
                  0,
                  reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
            static_cast<void>(payload.release());
          }
        });
  } else {
    auto request = ytec::windowsapp::OnlineBackupJobRequest{
        .selected_source = source,
        .final_path = path.data(),
        .administrator = state.elevated,
        .windows_major = version.value()[0],
        .windows_minor = version.value()[1],
        .windows_build = version.value()[2],
        .windows_architecture = architecture,
        .created_utc = current_utc_timestamp(),
        .app_version = std::string(kAppVersion),
        .async_wait = async_wait,
        .callbacks = callbacks,
        .logger = logger,
    };
    state.backup_thread = std::thread(
        [window, request = std::move(request)]() {
          auto payload = std::make_unique<BackupPayload>();
          payload->final_path = request.final_path;
          auto result = ytec::windowsapp::
              execute_online_backup_job_with_windows_apis(request);
          if (result) {
            payload->report = result.take_value();
          } else {
            payload->error = result.error();
          }
          if (PostMessageW(
                  window,
                  kBackupCompleteMessage,
                  0,
                  reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
            static_cast<void>(payload.release());
          }
        });
  }
}

void update_action_state(AppState& state) {
  const bool source_page =
      state.page == Page::clone || state.page == Page::create_image;
  const bool clone_page = state.page == Page::clone;
  const bool restore_target_page =
      state.page == Page::restore_image &&
      state.restore_preflight.has_value() &&
      !state.restore_preflight_running.load();
  const bool rescue_page = state.page == Page::rescue_media;
  const bool rescue_options_visible =
      rescue_page && state.media_preflight.has_value() &&
      !state.media_creation_running.load() &&
      !state.media_creation_report.has_value();
  const bool media_environment_ready =
      rescue_page && state.media_preflight.has_value() &&
      state.media_preflight->media_creation_permitted &&
      !state.media_preflight_running.load();
  const bool media_iso_page =
      rescue_options_visible &&
      selected_media_kind(state) ==
          ytec::windowsapp::RescueMediaKind::iso_file;
  const bool media_usb_page =
      rescue_options_visible &&
      selected_media_kind(state) ==
          ytec::windowsapp::RescueMediaKind::usb_drive;
  RECT client{};
  GetClientRect(state.window, &client);
  const auto clone_layout =
      ytec::windowsapp::calculate_clone_column_layout(client.right);
  if (state.page == Page::create_image) {
    MoveWindow(
        state.source_combo,
        312,
        288,
        (std::max)(client.right - 374, 560L),
        280,
        TRUE);
  } else {
    MoveWindow(
        state.source_combo,
        clone_layout.source_control.left,
        236,
        clone_layout.source_control.width(),
        280,
        TRUE);
  }
  if (restore_target_page) {
    MoveWindow(
        state.target_combo,
        312,
        client.bottom - 188,
        (std::max)(client.right - 374, 560L),
        280,
        TRUE);
  } else if (media_usb_page) {
    MoveWindow(
        state.target_combo,
        312,
        382,
        (std::max)(client.right - 374, 560L),
        280,
        TRUE);
  } else {
    MoveWindow(
        state.target_combo,
        clone_layout.target_control.left,
        236,
        clone_layout.target_control.width(),
        280,
        TRUE);
  }
  ShowWindow(state.source_combo, source_page ? SW_SHOW : SW_HIDE);
  ShowWindow(
      state.transfer_mode_combo,
      source_page ? SW_SHOW : SW_HIDE);
  EnableWindow(
      state.transfer_mode_combo,
      source_page && !state.backup_running.load() &&
              !state.inventory_loading.load()
          ? TRUE
          : FALSE);
  EnableWindow(
      state.source_combo,
      source_page && !state.inventory_loading.load() &&
              !state.backup_running.load()
          ? TRUE
          : FALSE);
  ShowWindow(
      state.target_combo,
      clone_page || restore_target_page || media_usb_page
          ? SW_SHOW
          : SW_HIDE);
  ShowWindow(
      state.restore_change_image,
      restore_target_page ? SW_SHOW : SW_HIDE);
  const auto media_layout =
      ytec::windowsapp::calculate_rescue_media_control_layout(
          client.right);
  MoveWindow(
      state.media_kind_combo,
      media_layout.kind_control.left,
      318,
      media_layout.kind_control.width(),
      180,
      TRUE);
  MoveWindow(
      state.media_profile_combo,
      media_layout.profile_control.left,
      318,
      media_layout.profile_control.width(),
      180,
      TRUE);
  MoveWindow(
      state.media_output_edit,
      media_layout.output_edit.left,
      382,
      media_layout.output_edit.width(),
      34,
      TRUE);
  MoveWindow(
      state.media_browse,
      media_layout.browse_button.left,
      382,
      media_layout.browse_button.width(),
      34,
      TRUE);
  ShowWindow(
      state.media_kind_combo,
      rescue_options_visible ? SW_SHOW : SW_HIDE);
  ShowWindow(
      state.media_profile_combo,
      rescue_options_visible ? SW_SHOW : SW_HIDE);
  ShowWindow(
      state.media_output_edit,
      media_iso_page ? SW_SHOW : SW_HIDE);
  ShowWindow(
      state.media_browse,
      media_iso_page ? SW_SHOW : SW_HIDE);
  const bool media_controls_enabled =
      media_environment_ready && !state.backup_running.load() &&
      !state.media_creation_running.load();
  EnableWindow(
      state.media_kind_combo,
      media_controls_enabled ? TRUE : FALSE);
  EnableWindow(
      state.media_profile_combo,
      media_controls_enabled ? TRUE : FALSE);
  EnableWindow(
      state.media_output_edit,
      media_controls_enabled && media_iso_page ? TRUE : FALSE);
  EnableWindow(
      state.media_browse,
      media_controls_enabled && media_iso_page ? TRUE : FALSE);
  if (media_usb_page) {
    EnableWindow(
        state.target_combo,
        media_controls_enabled && !state.inventory_loading.load()
            ? TRUE
            : FALSE);
  } else {
    EnableWindow(state.target_combo, TRUE);
  }

  std::wstring action = L"安全確認へ";
  bool enabled = false;
  if (state.page == Page::clone) {
    enabled = current_clone_selection(state).ready;
  } else if (state.page == Page::create_image) {
    action = state.backup_running.load()
        ? state.backup_cancel_requested.load()
              ? L"安全な停止を待っています…"
              : L"安全に取り消す"
        : L"保存先を選ぶ";
    const auto source_index = combo_selection(state.source_combo);
    enabled = state.backup_running.load()
        ? !state.backup_cancel_requested.load() &&
              (!state.backup_progress.has_value() ||
               state.backup_progress->cancellation_allowed)
        : state.elevated && !state.inventory_loading.load() &&
              state.inventory.has_value() && source_index.has_value() &&
              source_index.value() < state.inventory->disks.size() &&
              (state.inventory->disks[source_index.value()]
                       .partition_style ==
                   ytec::diskmodel::PartitionStyle::gpt ||
               state.inventory->disks[source_index.value()]
                       .partition_style ==
                   ytec::diskmodel::PartitionStyle::mbr);
  } else if (state.page == Page::restore_image) {
    action = state.restore_preflight_running.load()
                 ? L"イメージを検証中…"
                 : state.restore_preflight.has_value()
                       ? L"復元ジョブの確認へ"
                       : L"イメージを選ぶ";
    enabled =
        !state.restore_preflight_running.load() &&
        !state.backup_running.load() &&
        !state.media_preflight_running.load() &&
        (!state.restore_preflight.has_value() ||
         current_restore_target_selection(state)
             .ready_for_confirmation);
    EnableWindow(
        state.restore_change_image,
        !state.restore_preflight_running.load() ? TRUE : FALSE);
  } else if (state.page == Page::boot_repair) {
    action = L"レスキューメディアを作る";
    enabled = !state.backup_running.load() &&
              !state.media_creation_running.load();
  } else if (state.page == Page::rescue_media) {
    if (state.media_creation_running.load()) {
      const bool can_cancel =
          state.media_creation_progress.has_value() &&
          state.media_creation_progress->cancellation_allowed &&
          !state.media_creation_cancel_requested.load();
      action = state.media_creation_cancel_requested.load()
                   ? L"安全な停止を待っています…"
                   : can_cancel
                         ? L"安全に取り消す"
                         : selected_media_kind(state) ==
                                   ytec::windowsapp::RescueMediaKind::usb_drive
                               ? L"USBを作成中…"
                               : L"ISOを作成中…";
      enabled = can_cancel;
    } else if (state.media_creation_report.has_value()) {
      action =
          state.media_creation_report->complete_usb_verified
              ? L"別のUSBを作成"
              : L"別のISOを作成";
      enabled = true;
    } else if (state.media_preflight_running.load()) {
      action = L"ADK／WinPEを確認中…";
    } else if (!state.media_preflight.has_value() ||
               !state.media_preflight->media_creation_permitted) {
      action = state.media_preflight.has_value()
                   ? L"ADK導入ガイド／再確認"
                   : L"作成環境を確認";
      enabled = !state.backup_running.load();
    } else {
      const auto plan = current_rescue_media_plan(state);
      if (plan.issue ==
              ytec::windowsapp::RescueMediaPlanIssue::
                  iso_destination_missing ||
          plan.issue ==
              ytec::windowsapp::RescueMediaPlanIssue::
                  iso_destination_invalid) {
        action = L"ISOの保存先を選ぶ";
        enabled = !state.backup_running.load();
      } else if (plan.ready_for_confirmation) {
        action =
            selected_media_kind(state) ==
                    ytec::windowsapp::RescueMediaKind::usb_drive
                ? state.elevated
                      ? L"USB作成内容を確認"
                      : L"USB内容と管理者要件を確認"
                : state.elevated
                      ? L"作成内容を確認"
                      : L"作成内容と管理者要件を確認";
        enabled = !state.backup_running.load();
      } else {
        action = L"選択内容を確認してください";
      }
    }
  } else {
    action = L"診断情報を更新";
    enabled = !state.inventory_loading.load();
  }
  SetWindowTextW(state.primary_action, action.c_str());
  EnableWindow(state.primary_action, enabled ? TRUE : FALSE);
}

void populate_disk_combos(AppState& state) {
  SendMessageW(state.source_combo, CB_RESETCONTENT, 0, 0);
  SendMessageW(state.target_combo, CB_RESETCONTENT, 0, 0);
  if (!state.inventory.has_value()) {
    update_action_state(state);
    return;
  }
  for (const auto& disk : state.inventory->disks) {
    const std::wstring label = disk_label(disk);
    SendMessageW(
        state.source_combo,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(label.c_str()));
    SendMessageW(
        state.target_combo,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(label.c_str()));
  }
  std::optional<std::size_t> source_index;
  for (std::size_t index = 0; index < state.inventory->disks.size(); ++index) {
    if (state.inventory->disks[index].is_system_disk) {
      source_index = index;
      break;
    }
  }
  if (!source_index.has_value() && !state.inventory->disks.empty()) {
    source_index = 0;
  }
  if (source_index.has_value()) {
    SendMessageW(
        state.source_combo,
        CB_SETCURSEL,
        static_cast<WPARAM>(source_index.value()),
        0);
    const auto& source = state.inventory->disks[source_index.value()];
    for (std::size_t index = 0;
         index < state.inventory->disks.size();
         ++index) {
      const auto& candidate = state.inventory->disks[index];
      if (index != source_index.value() && !candidate.is_system_disk &&
          candidate.read_only == false &&
          candidate.size_bytes >= source.size_bytes) {
        SendMessageW(
            state.target_combo,
            CB_SETCURSEL,
            static_cast<WPARAM>(index),
            0);
        break;
      }
    }
  }
  if (state.page == Page::restore_image &&
      state.restore_preflight.has_value()) {
    select_default_restore_target(state);
  } else if (
      state.page == Page::rescue_media &&
      selected_media_kind(state) ==
          ytec::windowsapp::RescueMediaKind::usb_drive) {
    select_default_media_usb_target(state);
  }
  update_action_state(state);
}

void start_inventory(AppState& state) {
  if (state.inventory_loading.exchange(true)) {
    return;
  }
  if (state.inventory_thread.joinable()) {
    state.inventory_thread.join();
  }
  state.inventory_error.clear();
  EnableWindow(state.refresh, FALSE);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);

  const HWND window = state.window;
  const auto logger = state.logger;
  if (logger.has_value()) {
    logger->info(L"読み取り専用ディスク列挙開始");
  }
  state.inventory_thread = std::thread([window, logger]() {
    auto payload = std::make_unique<InventoryPayload>();
    auto provider =
        ytec::diskmodel::make_windows_disk_inventory_provider(
            logger.has_value() ? &logger.value() : nullptr);
    const auto result = provider->enumerate();
    if (result) {
      payload->report = result.value();
      log_inventory_summary(logger, result.value());
    } else {
      payload->error =
          L"ディスク情報を取得できませんでした: " +
          result.error().message;
      log_error_summary(
          logger, L"読み取り専用ディスク列挙失敗", result.error());
    }
    auto result_candidates =
        ytec::windowsapp::make_windows_job_result_candidate_provider();
    auto result_loader = ytec::windowsapp::make_windows_job_result_loader();
    auto imported = ytec::windowsapp::import_verified_job_results(
        *result_candidates, *result_loader);
    if (imported) {
      payload->job_results = imported.take_value();
      if (logger.has_value()) {
        logger->info(
            L"WinPE結果ログ読取り専用検証完了 count=" +
            std::to_wstring(payload->job_results.size()));
      }
    } else {
      payload->job_result_error =
          imported.error().operation + L": " + imported.error().message;
      log_error_summary(
          logger, L"WinPE結果ログ読取り専用検証失敗", imported.error());
    }
    if (PostMessageW(
            window,
            kInventoryCompleteMessage,
            0,
            reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
      static_cast<void>(payload.release());
    }
  });
}

void paint_sidebar(const AppState& state, HDC dc, const RECT& client) {
  RECT sidebar{0, 0, 250, client.bottom};
  const HBRUSH brush = CreateSolidBrush(kSidebar);
  FillRect(dc, &sidebar, brush);
  DeleteObject(brush);

  RECT brand{24, 28, 226, 64};
  draw_text(
      dc,
      L"Y-TEC",
      brand,
      RGB(202, 215, 226),
      DT_LEFT | DT_SINGLELINE | DT_VCENTER,
      state.small_font);
  RECT title{24, 57, 226, 105};
  draw_text(
      dc,
      L"Tsumugi Drive",
      title,
      RGB(255, 255, 255),
      DT_LEFT | DT_SINGLELINE | DT_VCENTER,
      state.brand_font);
  draw_strands(dc, 25, 108, 172);
  RECT concept_bounds{25, 133, 226, 153};
  draw_text(
      dc,
      L"3つの工程を、ひとつに",
      concept_bounds,
      RGB(154, 177, 192),
      DT_LEFT | DT_SINGLELINE | DT_TOP | DT_END_ELLIPSIS,
      state.small_font);

  RECT mode{24, client.bottom - 69, 226, client.bottom - 25};
  draw_text(
      dc,
      state.elevated
          ? L"管理者権限\n実行操作は安全確認後のみ"
          : L"標準権限・診断モード\n書き込み操作は行いません",
      mode,
      RGB(173, 191, 205),
      DT_LEFT | DT_WORDBREAK,
      state.small_font);
}

void paint_header(const AppState& state, HDC dc, const RECT& client) {
  const std::size_t index = static_cast<std::size_t>(state.page);
  RECT heading{286, 24, client.right - 172, 64};
  draw_text(
      dc,
      kNavigationLabels[index],
      heading,
      kInk,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER,
      state.heading_font);

  const bool shows_transfer_mode =
      state.page == Page::clone || state.page == Page::create_image;
  RECT lead{
      286,
      68,
      shows_transfer_mode ? client.right - 322 : client.right - 36,
      106};
  std::wstring text;
  switch (state.page) {
    case Page::clone:
      text =
          L"コピー元とコピー先を選び、内容を確認してから進めます。";
      break;
    case Page::create_image:
      text =
          L"Windowsを使いながら、システムまたはデータディスクの復元可能なイメージを作成します。";
      break;
    case Page::restore_image:
      text =
          L"イメージの整合性とコピー先を確認してから復元します。";
      break;
    case Page::boot_repair:
      text =
          L"クローンとは別に、Windowsの起動構成だけを診断・修復します。";
      break;
    case Page::rescue_media:
      text =
          L"このPCのADKを使い、BIOS／UEFI対応のUSBまたはISOを作成します。";
      break;
    case Page::diagnostics:
      text =
          L"ディスク列挙結果、権限状態、ログを安全に確認します。";
      break;
  }
  draw_text(
      dc,
      text,
      lead,
      kMuted,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
      state.body_font);
}

void paint_stepper(const AppState& state, HDC dc, const RECT& client) {
  const RECT card{286, 118, client.right - 36, 188};
  fill_rounded_rect(dc, card, kCard, kBorder);
  std::array<std::wstring_view, 4> labels{
      L"1  選択", L"2  安全確認", L"3  実行", L"4  完了"};
  int active_step{};
  if (state.page == Page::rescue_media) {
    labels = {
        L"1  環境確認",
        L"2  形式・出力先",
        L"3  内容確認",
        L"4  作成"};
    const auto plan = current_rescue_media_plan(state);
    active_step =
        (std::min)(
            static_cast<int>(plan.current_step) - 1,
            3);
  }
  const int width = (card.right - card.left - 36) / 4;
  for (int index = 0; index < 4; ++index) {
    RECT label{
        card.left + 18 + index * width,
        card.top + 17,
        card.left + 18 + (index + 1) * width,
        card.bottom - 13};
    draw_text(
        dc,
        labels[static_cast<std::size_t>(index)],
        label,
        index < active_step
            ? kSafeGreen
            : index == active_step ? kTsumugiBlue : kMuted,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER,
        state.body_font);
    if (index < 3) {
      const HPEN pen = CreatePen(PS_SOLID, 1, kBorder);
      const auto old = SelectObject(dc, pen);
      const int x = label.right - 20;
      MoveToEx(dc, x, card.top + 25, nullptr);
      LineTo(dc, x, card.bottom - 25);
      SelectObject(dc, old);
      DeleteObject(pen);
    }
  }
}

void paint_partition_bar(
    HDC dc,
    const ytec::diskmodel::DiskInfo& disk,
    RECT area) {
  if (disk.size_bytes == 0 || disk.partitions.empty()) {
    fill_rounded_rect(dc, area, RGB(235, 239, 242), kBorder, 8);
    return;
  }
  constexpr std::array<COLORREF, 5> colors{
      RGB(61, 151, 169),
      RGB(121, 91, 174),
      RGB(72, 135, 197),
      RGB(76, 159, 111),
      RGB(196, 137, 56)};
  int left = area.left;
  for (std::size_t index = 0; index < disk.partitions.size(); ++index) {
    const auto& partition = disk.partitions[index];
    const long double ratio =
        static_cast<long double>(partition.size_bytes) /
        static_cast<long double>(disk.size_bytes);
    int width = static_cast<int>(
        ratio * static_cast<long double>(area.right - area.left));
    width = (std::max)(width, 5);
    const int right =
        index + 1 == disk.partitions.size()
        ? area.right
        : (std::min)(left + width, static_cast<int>(area.right));
    RECT segment{left, area.top, right, area.bottom};
    const HBRUSH brush =
        CreateSolidBrush(colors[index % colors.size()]);
    FillRect(dc, &segment, brush);
    DeleteObject(brush);
    left = right;
    if (left >= area.right) {
      break;
    }
  }
  FrameRect(dc, &area, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
}

void paint_disk_details(
    const AppState& state,
    HDC dc,
    const RECT& client) {
  const auto layout =
      ytec::windowsapp::calculate_clone_column_layout(client.right);
  const std::array<int, 2> selections{
      static_cast<int>(SendMessageW(
          state.source_combo, CB_GETCURSEL, 0, 0)),
      static_cast<int>(SendMessageW(
          state.target_combo, CB_GETCURSEL, 0, 0))};
  const std::array<std::wstring_view, 2> roles{
      L"コピー元（読み取りのみ）", L"コピー先（上書き対象）"};
  for (int column = 0; column < 2; ++column) {
    const auto bounds = column == 0
        ? layout.source_card
        : layout.target_card;
    const int left = bounds.left;
    const int width = bounds.width();
    const RECT card{bounds.left, 206, bounds.right, 420};
    fill_rounded_rect(dc, card, kCard, kBorder);
    RECT role{left + 18, 213, left + width - 18, 236};
    draw_text(
        dc,
        roles[static_cast<std::size_t>(column)],
        role,
        column == 0 ? kTsumugiBlue : kWarning,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER,
        state.small_font);

    if (state.inventory_loading.load()) {
      RECT loading{left + 18, 277, left + width - 18, 350};
      draw_text(
          dc,
          L"ディスクを読み取り専用で確認しています…",
          loading,
          kMuted,
          DT_CENTER | DT_SINGLELINE | DT_VCENTER,
          state.body_font);
      continue;
    }
    const int selected = selections[static_cast<std::size_t>(column)];
    if (!state.inventory.has_value() || selected < 0 ||
        static_cast<std::size_t>(selected) >=
            state.inventory->disks.size()) {
      RECT empty{left + 18, 277, left + width - 18, 350};
      draw_text(
          dc,
          state.inventory_error.empty()
              ? L"対象のディスクがありません"
              : state.inventory_error,
          empty,
          state.inventory_error.empty() ? kMuted : RGB(176, 56, 56),
          DT_CENTER | DT_WORDBREAK | DT_VCENTER,
          state.body_font);
      continue;
    }
    const auto& disk =
        state.inventory->disks[static_cast<std::size_t>(selected)];
    RECT summary{left + 18, 277, left + width - 18, 310};
    const std::wstring summary_text =
        partition_style_text(disk.partition_style) + L"  •  " +
        (disk.bus_type.empty() ? L"Bus不明" : disk.bus_type) + L"  •  " +
        std::to_wstring(disk.logical_sector_size) + L" / " +
        std::to_wstring(disk.physical_sector_size) + L" bytes";
    draw_text(
        dc,
        summary_text,
        summary,
        kMuted,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
        state.small_font);
    RECT bar{left + 18, 321, left + width - 18, 345};
    paint_partition_bar(dc, disk, bar);
    RECT parts{left + 18, 352, left + width - 18, 382};
    const std::wstring partition_text =
        std::to_wstring(disk.partitions.size()) + L" パーティション  •  " +
        (disk.serial_suffix.empty()
             ? L"シリアル末尾なし"
             : L"シリアル末尾 " +
                   std::wstring(
                       disk.serial_suffix.begin(),
                       disk.serial_suffix.end()));
    draw_text(
        dc,
        partition_text,
        parts,
        kInk,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
        state.small_font);
    if (disk.is_system_disk) {
      RECT badge{left + 18, 383, left + width - 18, 406};
      draw_text(
          dc,
          L"● 現在のWindowsディスク",
          badge,
          kSafeGreen,
          DT_LEFT | DT_SINGLELINE | DT_VCENTER,
          state.small_font);
    }
  }
}

void paint_progress_preview(
    const AppState& state,
    HDC dc,
    const RECT& client) {
  const RECT card{286, 438, client.right - 36, client.bottom - 92};
  fill_rounded_rect(dc, card, kCard, kBorder);
  RECT title{card.left + 20, card.top + 12, card.right - 20, card.top + 40};
  draw_text(
      dc,
      L"進行状況",
      title,
      kInk,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER,
      state.body_font);

  const auto progress = ytec::windowsapp::calculate_progress(
      ytec::windowsapp::ProgressInput{
          .stage = ytec::windowsapp::JobStage::waiting});
  RECT stage{
      card.left + 20, card.top + 45, card.right - 20, card.top + 72};
  draw_text(
      dc,
      progress.stage_label + L"  —  実行開始前",
      stage,
      kMuted,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER,
      state.small_font);
  RECT track{
      card.left + 20, card.top + 79, card.right - 20, card.top + 94};
  fill_rounded_rect(dc, track, RGB(231, 236, 240), RGB(231, 236, 240), 8);

  const int detail_width = (card.right - card.left - 40) / 4;
  const std::array<std::wstring, 4> details{
      L"処理量\n—",
      L"速度\n—",
      L"経過時間\n—",
      L"残り時間\n—"};
  for (int index = 0; index < 4; ++index) {
    RECT detail{
        card.left + 20 + detail_width * index,
        card.top + 103,
        card.left + 20 + detail_width * (index + 1),
        card.bottom - 8};
    draw_text(
        dc,
        details[static_cast<std::size_t>(index)],
        detail,
        kMuted,
        DT_LEFT | DT_WORDBREAK,
        state.small_font);
  }
}

void paint_online_backup_progress(
    const AppState& state,
    HDC dc,
    const RECT& card) {
  const ytec::clonecore::DiskOperationProgress observed =
      state.backup_progress.value_or(
          ytec::clonecore::DiskOperationProgress{
              .stage = ytec::clonecore::DiskOperationStage::planning,
              .cancellation_allowed = true,
          });
  const auto progress =
      ytec::windowsapp::build_online_image_progress_view(
          observed, state.backup_elapsed);

  RECT stage{
      card.left + 26,
      card.top + 116,
      card.right - 150,
      card.top + 144};
  draw_text(
      dc,
      progress.stage_label,
      stage,
      progress.cancellation_allowed ? kTsumugiBlue : kWarning,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
      state.body_font);
  RECT percentage{
      card.right - 142,
      card.top + 116,
      card.right - 26,
      card.top + 144};
  draw_text(
      dc,
      progress.percentage_label,
      percentage,
      kSafeGreen,
      DT_RIGHT | DT_SINGLELINE | DT_VCENTER,
      state.body_font);

  RECT track{
      card.left + 26,
      card.top + 151,
      card.right - 26,
      card.top + 168};
  fill_rounded_rect(
      dc, track, RGB(230, 235, 240), RGB(230, 235, 240), 9);
  RECT fill = track;
  fill.right =
      fill.left + static_cast<LONG>(
                      static_cast<double>(fill.right - fill.left) *
                      progress.fraction);
  if (fill.right > fill.left) {
    fill_rounded_rect(dc, fill, kTsumugiBlue, kTsumugiBlue, 9);
  }

  const int inner_left = card.left + 26;
  const int inner_right = card.right - 26;
  constexpr int kGap = 8;
  const int metric_width =
      (inner_right - inner_left - kGap * 3) / 4;
  const int metric_top = card.top + 181;
  const int metric_bottom =
      (std::min)(card.top + 240, card.bottom - 82);
  const std::array<std::wstring_view, 4> labels{
      L"読込", L"書込", L"検証", L"残り時間"};
  const std::array<std::wstring, 4> values{
      progress.read_label,
      progress.write_label,
      progress.verified_label,
      progress.remaining_label};
  const std::array<COLORREF, 4> accents{
      kTsumugiBlue, kTsumugiPurple, kSafeGreen, kWarning};
  const std::array<COLORREF, 4> fills{
      RGB(242, 249, 250),
      RGB(247, 244, 251),
      RGB(243, 250, 247),
      RGB(253, 248, 240)};
  for (int index = 0; index < 4; ++index) {
    const int left =
        inner_left + index * (metric_width + kGap);
    draw_progress_metric(
        state,
        dc,
        RECT{
            left,
            metric_top,
            left + metric_width,
            metric_bottom},
        labels[static_cast<std::size_t>(index)],
        values[static_cast<std::size_t>(index)],
        accents[static_cast<std::size_t>(index)],
        fills[static_cast<std::size_t>(index)]);
  }

  RECT details{
      card.left + 26,
      metric_bottom + 10,
      card.right - 26,
      metric_bottom + 36};
  draw_text(
      dc,
      L"総合処理速度 " + progress.speed_label +
          L"  •  経過 " + progress.elapsed_label +
          (state.backup_cancel_requested.load()
               ? L"  •  安全な停止を待機中"
               : progress.cancellation_allowed
                     ? L"  •  安全な境界で取消可能"
                     : L"  •  最終確定中のため取消不可"),
      details,
      progress.cancellation_allowed ? kMuted : kWarning,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
      state.small_font);
  RECT safety{
      card.left + 26,
      metric_bottom + 42,
      card.right - 26,
      card.bottom - 16};
  draw_text(
      dc,
      selected_transfer_mode(state) ==
              ytec::imageformat::TransferMode::shrink
          ? L"VSS状態: Snapshotを使用中  •  コピー元: 読み取り専用  •  全WIMとmanifestのSHA-256合格後だけ完成名へ確定"
          : L"VSS状態: Snapshotを使用中  •  コピー元: 読み取り専用  •  全チャンクと全体SHA-256の読戻し合格後だけ完成名へ確定",
      safety,
      kMuted,
      DT_LEFT | DT_WORDBREAK,
      state.small_font);
}

void paint_clone_safety(
    const AppState& state,
    HDC dc,
    const RECT& client) {
  const auto selection = current_clone_selection(state);
  RECT area{
      306,
      client.bottom - 78,
      client.right - 270,
      client.bottom - 28};
  const std::wstring text = L"● " + selection.message;
  draw_text(
      dc,
      text,
      area,
      selection.ready ? kSafeGreen : kWarning,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
      state.small_font);
}

void paint_rescue_media_creation(
    const AppState& state,
    HDC dc,
    const RECT& card) {
  const bool completed = state.media_creation_report.has_value();
  const bool usb =
      completed
          ? state.media_creation_report->complete_usb_verified
          : selected_media_kind(state) ==
                ytec::windowsapp::RescueMediaKind::usb_drive;
  const std::uint8_t percent =
      completed
          ? 100U
          : state.media_creation_progress.has_value()
                ? state.media_creation_progress->percent
                : 0U;
  const std::wstring stage =
      completed
          ? L"作成完了"
          : state.media_creation_progress.has_value()
                ? std::wstring(
                      ytec::windowsapp::rescue_media_creation_stage_label(
                          state.media_creation_progress->stage))
                : L"作成を開始しています";
  const std::wstring message =
      completed
          ? usb
                ? L"USB作成後に全ファイルを読み戻し、SHA-256検証まで完了しました。"
                : L"ISOを完成名へ確定し、全量SHA-256検証まで完了しました。"
          : state.media_creation_progress.has_value()
                ? state.media_creation_progress->message
                : L"製品ファイルとADK環境を再確認しています。";

  RECT stage_area{
      card.left + 26, card.top + 66, card.right - 26, card.top + 98};
  draw_text(
      dc,
      L"手順 4/4  " + stage,
      stage_area,
      completed ? kSafeGreen : kTsumugiPurple,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
      state.body_font);
  RECT message_area{
      card.left + 26, card.top + 101, card.right - 26, card.top + 145};
  draw_text(
      dc,
      message,
      message_area,
      kMuted,
      DT_LEFT | DT_WORDBREAK | DT_VCENTER,
      state.small_font);

  const RECT track{
      card.left + 26, card.top + 157, card.right - 26, card.top + 175};
  fill_rounded_rect(
      dc, track, RGB(230, 235, 240), RGB(230, 235, 240), 9);
  if (percent > 0U) {
    RECT fill = track;
    const LONG width = track.right - track.left;
    fill.right = fill.left +
                 static_cast<LONG>(
                     static_cast<long long>(width) * percent / 100LL);
    fill_rounded_rect(
        dc,
        fill,
        completed ? kSafeGreen : kTsumugiBlue,
        completed ? kSafeGreen : kTsumugiBlue,
        9);
  }

  const ULONGLONG elapsed_ms =
      state.media_creation_started_tick == 0
          ? 0
          : GetTickCount64() - state.media_creation_started_tick;
  const std::wstring elapsed =
      ytec::windowsapp::format_duration(
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::milliseconds(elapsed_ms)));
  std::wstring remaining = L"計算中";
  if (completed) {
    remaining = L"0秒";
  } else if (percent > 0U && elapsed_ms >= 1'000U) {
    const ULONGLONG remaining_ms =
        (elapsed_ms / percent) * (100U - percent);
    remaining = ytec::windowsapp::format_duration(
        std::chrono::seconds(
            static_cast<std::chrono::seconds::rep>(
                remaining_ms / 1'000U)));
  }
  const std::array<std::wstring, 3U> details{
      L"進捗\n" + std::to_wstring(percent) + L"%",
      L"経過時間\n" + elapsed,
      L"目安残り時間\n" + remaining,
  };
  const int column_width = (card.right - card.left - 52) / 3;
  for (int index = 0; index < 3; ++index) {
    RECT detail{
        card.left + 26 + column_width * index,
        card.top + 190,
        card.left + 26 + column_width * (index + 1),
        card.top + 239};
    draw_text(
        dc,
        details[static_cast<std::size_t>(index)],
        detail,
        kInk,
        DT_LEFT | DT_WORDBREAK,
        state.small_font);
  }

  RECT result{
      card.left + 20, card.top + 252, card.right - 20, card.bottom - 18};
  fill_rounded_rect(
      dc,
      result,
      completed ? RGB(243, 250, 247) : RGB(247, 248, 252),
      completed ? RGB(178, 220, 199) : kBorder,
      10);
  std::wstring result_text;
  if (completed) {
    const auto& report = state.media_creation_report.value();
    if (report.complete_usb_verified) {
      result_text =
          L"作成先: " + report.usb_root_path +
          L"\nboot.wim SHA-256: " +
          std::wstring(
              report.usb_boot_wim_sha256.begin(),
              report.usb_boot_wim_sha256.end()) +
          L"\n全ファイル検証記録: " + report.manifest_path;
    } else {
      result_text =
          L"保存先: " + report.final_iso_path +
          L"\n容量: " + format_bytes(report.iso_length) +
          L"\nSHA-256: " +
          std::wstring(report.iso_sha256.begin(), report.iso_sha256.end()) +
          L"\n検証記録: " + report.manifest_path;
    }
  } else {
    result_text =
        L"Microsoft製ファイルは配布物へコピーせず、このPCのADKだけを使用します。"
        L"\nWIMのマウント／コミット" +
        std::wstring(usb ? L"とUSB書込み" : L"") +
        L"中は、破損防止のためアプリを終了しないでください。";
  }
  RECT result_text_area{
      result.left + 16,
      result.top + 10,
      result.right - 16,
      result.bottom - 8};
  draw_text(
      dc,
      result_text,
      result_text_area,
      completed ? kSafeGreen : kMuted,
      DT_LEFT | DT_WORDBREAK,
      state.small_font);
}

void paint_rescue_media_page(
    const AppState& state,
    HDC dc,
    const RECT& client) {
  const RECT card{286, 206, client.right - 36, client.bottom - 92};
  fill_rounded_rect(dc, card, kCard, kBorder);

  RECT title_area{
      card.left + 26, card.top + 16, card.right - 26, card.top + 50};
  draw_text(
      dc,
      L"レスキューUSB／ISO作成ウィザード",
      title_area,
      kInk,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER,
      state.heading_font);

  if (state.media_creation_running.load() ||
      state.media_creation_report.has_value()) {
    paint_rescue_media_creation(state, dc, card);
    return;
  }

  if (state.media_preflight_running.load()) {
    RECT loading{
        card.left + 26,
        card.top + 70,
        card.right - 26,
        card.bottom - 24};
    draw_text(
        dc,
        L"Microsoft ADK、WinPE Add-on、署名、バージョン、"
        L"必須更新を読み取り専用で確認しています。\n\n"
        L"この確認ではWIM、ISO、USBを作成せず、UACも要求しません。",
        loading,
        kMuted,
        DT_LEFT | DT_WORDBREAK,
        state.body_font);
    return;
  }

  const bool environment_ready =
      state.media_preflight.has_value() &&
      state.media_preflight->media_creation_permitted;
  RECT environment{
      card.left + 26,
      card.top + 53,
      card.right - 26,
      card.top + 85};
  const std::wstring environment_text =
      state.media_preflight.has_value()
          ? environment_ready
                ? L"● 作成環境を確認済み — ADK／WinPE／署名／必須更新: 合格"
                : L"● 作成環境は未完了 — 診断内容を確認して再検査してください"
          : L"● 手順1 — このPCのADK／WinPE作成環境を確認してください";
  draw_text(
      dc,
      environment_text,
      environment,
      environment_ready ? kSafeGreen : kWarning,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
      state.small_font);

  if (!state.media_preflight.has_value()) {
    RECT introduction{
        card.left + 26,
        card.top + 102,
        card.right - 26,
        card.bottom - 24};
    draw_text(
        dc,
        L"Microsoft公式のADKとWinPE Add-onを、このPCに"
        L"インストールした状態で利用します。\n\n"
        L"製品にはMicrosoft製EXE、DLL、WIM、ISOを同梱しません。\n\n"
        L"不足している場合は、診断後にMicrosoft公式の導入ページと"
        L"必須更新ページを直接開けます。最初の確認は読み取り専用です。",
        introduction,
        kMuted,
        DT_LEFT | DT_WORDBREAK,
        state.body_font);
    return;
  }

  RECT kind_label{
      card.left + 26,
      card.top + 88,
      card.left + (card.right - card.left) / 2 - 4,
      card.top + 111};
  draw_text(
      dc,
      L"作成する種類",
      kind_label,
      kTsumugiBlue,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER,
      state.small_font);
  RECT profile_label{
      card.left + (card.right - card.left) / 2 + 6,
      card.top + 88,
      card.right - 26,
      card.top + 111};
  draw_text(
      dc,
      L"起動互換性",
      profile_label,
      kTsumugiPurple,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER,
      state.small_font);

  RECT destination_label{
      card.left + 26,
      card.top + 153,
      card.right - 26,
      card.top + 176};
  draw_text(
      dc,
      selected_media_kind(state) ==
              ytec::windowsapp::RescueMediaKind::iso_file
          ? L"ISOの保存先（既存ファイルは上書きしません）"
          : L"作成先USB（GPT/MBRをMBR・単一FAT32へ自動初期化）",
      destination_label,
      selected_media_kind(state) ==
              ytec::windowsapp::RescueMediaKind::iso_file
          ? kTsumugiBlue
          : kWarning,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER,
      state.small_font);

  const auto plan = current_rescue_media_plan(state);
  const RECT result_card{
      card.left + 20,
      card.top + 224,
      card.right - 20,
      card.bottom - 18};
  fill_rounded_rect(
      dc,
      result_card,
      plan.ready_for_confirmation
          ? RGB(243, 250, 247)
          : RGB(253, 248, 241),
      plan.ready_for_confirmation ? RGB(178, 220, 199)
                                  : RGB(232, 205, 166),
      10);
  RECT result_text{
      result_card.left + 16,
      result_card.top + 10,
      result_card.right - 16,
      result_card.bottom - 8};
  std::wstring message = L"● " + plan.message;
  if (plan.ready_for_confirmation) {
    message +=
        L"\n次は作成内容を確認します。まだISO／USBへの書き込みは行いません。";
  } else if (!environment_ready) {
    message +=
        L"\n詳細は診断結果に表示しています。下のボタンから公式導入ガイドを開けます。";
  }
  draw_text(
      dc,
      message,
      result_text,
      plan.ready_for_confirmation ? kSafeGreen : kWarning,
      DT_LEFT | DT_WORDBREAK,
      state.small_font);
}

void paint_non_clone_page(
    const AppState& state,
    HDC dc,
    const RECT& client) {
  const bool full_height_card =
      state.page == Page::boot_repair ||
      state.page == Page::diagnostics;
  const RECT card{
      286,
      full_height_card ? 118 : 206,
      client.right - 36,
      client.bottom - 92};
  fill_rounded_rect(dc, card, kCard, kBorder);

  std::wstring title;
  std::wstring body;
  switch (state.page) {
    case Page::create_image:
      title = L"オンライン・イメージバックアップ";
      if (state.backup_running.load()) {
        body = selected_transfer_mode(state) ==
                ytec::imageformat::TransferMode::shrink
            ? L"VSSスナップショットから縮小移行用WIMを作成しています。全WIMとマニフェストをSHA-256で確認し、VSSの完了と削除後だけ最終名へ確定します。\n\n完了結果が表示されるまで、このアプリを終了しないでください。"
            : L"VSSスナップショットからイメージを作成しています。全チャンクと全体SHA-256を読み戻し、VSSの完了と削除後だけ最終ファイル名へ確定します。\n\n完了結果が表示されるまで、このアプリを終了しないでください。";
      } else {
        body = selected_transfer_mode(state) ==
                ytec::imageformat::TransferMode::shrink
            ? L"NTFSの使用データをVSS Snapshotから検証済みWIMへ保存します。復元時は安全余白を加えた必要容量を計算し、元より小さい空のSSDにも再配置できます。\n\nWindowsディスクとデータ専用ディスクの両方に対応し、原本は読み取り専用、既存保存先は非上書きです。"
            : L"VSSで整合したスナップショットを作成し、保存先の空き容量と同一ディスク誤指定を確認してから開始します。\n\nWindowsディスクとデータ専用ディスクの両方に対応し、コピー元は読み取り専用、既存ファイルは上書きしません。";
      }
      break;
    case Page::restore_image:
      title = L"イメージから復元";
      if (state.restore_preflight_running.load()) {
        body =
            L"選択したイメージを読み取り専用で完全検証しています。\n\n"
            L"通常イメージは全チャンク、縮小移行イメージは全WIMをSHA-256で照合し、マニフェストと復元構成も確認します。この処理では復元先ディスクを開きません。";
      } else if (state.restore_preflight.has_value()) {
        const auto& report = state.restore_preflight.value();
        const bool shrink = report.shrink_manifest.has_value();
        const bool gpt =
            report.manifest.partition_style ==
            ytec::imageformat::BackupPartitionStyle::gpt;
        const bool uefi =
            report.manifest.boot_mode ==
            ytec::imageformat::BackupBootMode::uefi;
        const bool contains_windows = report.manifest.source.is_system_disk;
        const std::wstring boot_text = contains_windows
            ? (uefi ? L"UEFI" : L"Legacy BIOS")
            : L"起動領域なし（データ専用）";
        const std::wstring version_text = contains_windows
            ? L" / Windows " +
                  std::to_wstring(report.manifest.windows_major) + L"." +
                  std::to_wstring(report.manifest.windows_minor) + L"." +
                  std::to_wstring(report.manifest.windows_build) + L" / " +
                  std::wstring(
                      report.manifest.windows_architecture.begin(),
                      report.manifest.windows_architecture.end())
            : L"";
        const std::size_t payload_count = shrink
            ? static_cast<std::size_t>(std::count_if(
                  report.shrink_manifest->partitions.begin(),
                  report.shrink_manifest->partitions.end(),
                  [](const auto& partition) {
                    return !partition.payload_file_name.empty();
                  }))
            : static_cast<std::size_t>(report.header.chunk_count);
        body =
            std::wstring(
                L"イメージの完全検証に合格しました。\n"
                L"モード: ") +
            (shrink ? L"縮小移行モード" : L"通常モード") +
            L"\n"
            L"コピー元: " +
            (report.manifest.source.model.empty()
                 ? L"モデル不明"
                 : report.manifest.source.model) +
            L" / " +
            format_bytes(report.header.source_disk_size) +
            L"\n形式: " + (gpt ? L"GPT" : L"MBR") +
            L" / " + boot_text + version_text +
            L"\n構成: パーティション " +
            std::to_wstring(report.manifest.partitions.size()) +
            (shrink ? L" / WIM " : L" / チャンク ") +
            std::to_wstring(payload_count);
      } else {
        body =
            L"バックアップイメージが壊れていないか、"
            L"最初から最後まで読み取り専用で確認します。\n\n"
            L"通常モード（.dcimg）と縮小移行モード（.dcmig）の両方に対応します。"
            L"Windowsディスクとデータ専用ディスクを復元できます。\n\n"
            L"合格後は復元先を選び、WinPEへ渡す復元予約を保存できます。"
            L"この画面では復元を開始せず、復元先ディスクも開きません。\n\n"
            L"全体と分割データのSHA-256、内部構成、容量境界も検査し、"
            L"特殊なリンク、ネットワーク上のファイル、"
            L"デバイス直指定は安全側に拒否します。";
      }
      break;
    case Page::boot_repair:
      title = L"Windowsの起動だけを直す";
      body =
          L"クローンや復元をせず、壊れたWindowsの起動情報（BCD）だけを"
          L"診断・再構築できます。\n\n"
          L"1. このPCでレスキューISOまたはUSBを作る\n"
          L"2. 起動できないPCをそのメディアから起動する\n"
          L"3. 「起動を修復」を選び、対象を確認して実行する\n\n"
          L"UEFI/GPTとレガシーBIOS/MBRを自動判定し、"
          L"Microsoft署名済みのBCDBootだけを使用します。"
          L"パーティションの作成・移動・フォーマットは自動実行しません。";
      break;
    case Page::rescue_media:
      title = L"レスキューUSB／ISO作成";
      if (state.media_preflight_running.load()) {
        body =
            L"Microsoft ADK、WinPE Add-on、Microsoft署名、"
            L"バージョン、必須更新を読み取り専用で確認しています。\n\n"
            L"この操作ではWIM、ISO、USBを作成せず、"
            L"UACも要求しません。";
      } else if (state.media_preflight.has_value()) {
        body = state.media_preflight->status + L"\n\n" +
               state.media_preflight->screen_details;
      } else {
        body =
            L"Microsoft ADKとWinPE Add-onをこのPC上で"
            L"読み取り専用診断します。\n\n"
            L"Microsoft製EXE、DLL、WIM、ISOはリポジトリや"
            L"配布物へ同梱しません。診断ではファイルを変更せず、"
            L"UACも要求しません。\n\n"
            L"実際のISO／USB作成は、UACが必要な確認項目と"
            L"まとめて後ほど実施します。";
      }
      break;
    case Page::diagnostics:
      title = L"PCとディスクの安全診断";
      body =
          (state.elevated
               ? L"現在は管理者権限で起動しています。"
               : L"現在は標準権限で起動しています。") +
          std::wstring(
              L" この画面と「診断情報を更新」はディスクへ書き込みません。");
      if (state.inventory_loading.load()) {
        body += L"\n\nディスクを読み取り専用で確認しています…";
      } else if (state.inventory.has_value()) {
        body +=
            L"\n\n検出: " +
            std::to_wstring(state.inventory->disks.size()) +
            L" 台　注意: " +
            std::to_wstring(state.inventory->issues.size()) + L" 件";
        constexpr std::size_t kMaximumVisibleDisks = 5U;
        const std::size_t count =
            (std::min)(
                state.inventory->disks.size(), kMaximumVisibleDisks);
        for (std::size_t index = 0; index < count; ++index) {
          const auto& disk = state.inventory->disks[index];
          body +=
              L"\n• ディスク " +
              std::to_wstring(disk.disk_number) + L"　" +
              (disk.model.empty() ? L"モデル不明" : disk.model) +
              L"　" + format_bytes(disk.size_bytes) + L"　" +
              partition_style_text(disk.partition_style) + L" / " +
              (disk.bus_type.empty() ? L"Bus不明" : disk.bus_type) +
              L"　パーティション " +
              std::to_wstring(disk.partitions.size()) +
              (disk.is_system_disk ? L"　[Windows]" : L"");
        }
        if (state.inventory->disks.size() > count) {
          body +=
              L"\n• ほか " +
              std::to_wstring(
                  state.inventory->disks.size() - count) +
              L" 台";
        }
      } else {
        body += L"\n\nディスク情報はまだ取得できていません。";
      }
      if (!state.inventory_error.empty()) {
        body +=
            L"\n\n最新の列挙エラー:\n" + state.inventory_error;
      }
      if (!state.log_path.empty()) {
        const std::size_t log_name_separator =
            state.log_path.find_last_of(L"\\/");
        const std::wstring log_name =
            log_name_separator == std::wstring::npos
                ? state.log_path
                : state.log_path.substr(log_name_separator + 1U);
        body +=
            L"\n\n診断ログ（UTF-8／アプリ実行中も閲覧可能）\n"
            L"保存先: アプリと同じフォルダーの「logs」\n"
            L"ログ名: " +
            log_name;
      } else if (!state.log_error.empty()) {
        body +=
            L"\n\n診断ログを作成できませんでした:\n" +
            state.log_error;
      }
      if (!state.job_result_error.empty()) {
        body +=
            L"\n\nWinPE実行結果: 検証できませんでした\n" +
            state.job_result_error;
      } else if (state.job_results.empty()) {
        body +=
            L"\n\nWinPE実行結果: 固定位置に結果ログはありません";
      } else {
        const auto& latest = state.job_results.front();
        body +=
            L"\n\nWinPE実行結果（読取り専用で検証済み）\n"
            L"検出: " + std::to_wstring(state.job_results.size()) +
            L" 件 / 最新: " +
            ascii_to_wide(latest.record.completed_utc) + L" / " +
            job_result_type_text(latest.record.job_type) + L" / " +
            (latest.record.outcome ==
                     ytec::imageformat::JobResultOutcome::passed
                 ? L"成功"
                 : L"失敗") +
            L"\nジョブSHA-256先頭: " +
            job_hash_prefix(latest.record.job_payload_hash) +
            L" / ファイル: " + latest.file_name;
      }
      break;
    case Page::clone:
      break;
  }

  RECT title_area{
      card.left + 26, card.top + 20, card.right - 26, card.top + 58};
  draw_text(
      dc,
      title,
      title_area,
      kInk,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER,
      state.heading_font);
  if (state.page == Page::create_image) {
    RECT selector_label{
        card.left + 26,
        card.top + 58,
        card.right - 26,
        card.top + 80};
    draw_text(
        dc,
        L"バックアップ元（Windows／データ専用ディスク・読み取り専用）",
        selector_label,
        kTsumugiBlue,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER,
        state.small_font);
  }
  if (state.page == Page::create_image &&
      state.backup_running.load()) {
    paint_online_backup_progress(state, dc, card);
    return;
  }
  RECT body_area{
      card.left + 26,
      card.top +
          (state.page == Page::create_image ? 116 : 75),
      card.right - 26,
      state.page == Page::restore_image &&
              state.restore_preflight.has_value()
          ? card.bottom - 142
          : card.bottom - 24};
  draw_text(
      dc,
      body,
      body_area,
      kMuted,
      DT_LEFT | DT_WORDBREAK,
      (state.page == Page::restore_image &&
               state.restore_preflight.has_value()) ||
              state.page == Page::diagnostics
          ? state.small_font
          : state.body_font);
  if (state.page == Page::restore_image &&
      state.restore_preflight.has_value()) {
    RECT selector_label{
        card.left + 26,
        card.bottom - 132,
        card.right - 26,
        card.bottom - 105};
    draw_text(
        dc,
        L"復元先候補（ディスクを開かない読み取り専用の基礎確認）",
        selector_label,
        kTsumugiBlue,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
        state.small_font);

    const auto target = current_restore_target_selection(state);
    RECT status_area{
        card.left + 26,
        card.bottom - 60,
        card.right - 26,
        card.bottom - 16};
    const std::wstring status =
        L"● " + target.message +
        (target.ready_for_confirmation
             ? L"　復元ジョブ: 作成可能"
             : L"　復元ジョブ: 作成不可") +
        L"　復元実行: 無効";
    draw_text(
        dc,
        status,
        status_area,
        target.ready_for_confirmation ? kSafeGreen : kWarning,
        DT_LEFT | DT_WORDBREAK,
        state.small_font);
  }
}

void paint_window(const AppState& state, HDC dc) {
  RECT client{};
  GetClientRect(state.window, &client);
  const HBRUSH canvas = CreateSolidBrush(kCanvas);
  FillRect(dc, &client, canvas);
  DeleteObject(canvas);
  paint_sidebar(state, dc, client);
  paint_header(state, dc, client);
  if (state.page == Page::clone ||
      state.page == Page::restore_image ||
      state.page == Page::rescue_media) {
    paint_stepper(state, dc, client);
  }
  if (state.page == Page::clone) {
    paint_disk_details(state, dc, client);
    paint_progress_preview(state, dc, client);
    paint_clone_safety(state, dc, client);
  } else if (state.page == Page::rescue_media) {
    paint_rescue_media_page(state, dc, client);
  } else {
    paint_non_clone_page(state, dc, client);
  }
}

void draw_navigation_button(
    const AppState& state,
    const DRAWITEMSTRUCT& item) {
  const int index = static_cast<int>(item.CtlID) - kNavFirstId;
  if (index < 0 ||
      static_cast<std::size_t>(index) >= state.navigation.size()) {
    return;
  }
  const bool selected =
      static_cast<std::size_t>(index) ==
      static_cast<std::size_t>(state.page);
  const bool pressed = (item.itemState & ODS_SELECTED) != 0;
  const COLORREF fill =
      pressed ? RGB(62, 77, 96)
              : selected ? kSidebarSelected : kSidebar;
  const HBRUSH brush = CreateSolidBrush(fill);
  FillRect(item.hDC, &item.rcItem, brush);
  DeleteObject(brush);
  if (selected) {
    RECT accent = item.rcItem;
    accent.right = accent.left + 4;
    const HBRUSH accent_brush = CreateSolidBrush(kTsumugiBlue);
    FillRect(item.hDC, &accent, accent_brush);
    DeleteObject(accent_brush);
  }
  RECT text = item.rcItem;
  text.left += 16;
  draw_text(
      item.hDC,
      kNavigationLabels[static_cast<std::size_t>(index)],
      text,
      selected ? RGB(255, 255, 255) : RGB(190, 205, 216),
      DT_LEFT | DT_SINGLELINE | DT_VCENTER,
      state.body_font);
  if ((item.itemState & ODS_FOCUS) != 0) {
    RECT focus = item.rcItem;
    InflateRect(&focus, -6, -5);
    DrawFocusRect(item.hDC, &focus);
  }
}

LRESULT CALLBACK window_proc(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
  auto* state = reinterpret_cast<AppState*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  switch (message) {
    case WM_NCCREATE: {
      const auto* create =
          reinterpret_cast<const CREATESTRUCTW*>(lparam);
      state = static_cast<AppState*>(create->lpCreateParams);
      state->window = window;
      SetWindowLongPtrW(
          window,
          GWLP_USERDATA,
          reinterpret_cast<LONG_PTR>(state));
      return TRUE;
    }
    case WM_CREATE: {
      if (state == nullptr) {
        return -1;
      }
      state->elevated = process_is_elevated();
      auto log_session = create_product_log_session();
      if (log_session) {
        auto session = log_session.take_value();
        state->logger = std::move(session.logger);
        state->log_path = std::move(session.path);
        state->logger->info(
            L"Y-TEC Tsumugi Drive 起動 version=" +
            std::wstring(kAppVersionWide) + L" permission=" +
            (state->elevated ? L"administrator" : L"standard"));
        const auto windows_version = current_windows_version();
        if (windows_version) {
          state->logger->info(
              L"実行環境 Windows=" +
              std::to_wstring(windows_version.value()[0]) + L"." +
              std::to_wstring(windows_version.value()[1]) + L"." +
              std::to_wstring(windows_version.value()[2]) +
              L" architecture=" +
              (current_native_architecture() == "AMD64"
                   ? L"AMD64"
                   : L"unsupported"));
        } else {
          log_error_summary(
              state->logger,
              L"実行環境バージョン取得失敗",
              windows_version.error());
        }
      } else {
        state->log_error =
            log_session.error().operation + L": " +
            log_session.error().message + L" (Windows error " +
            std::to_wstring(log_session.error().native_code) + L")";
      }
      SetWindowTextW(window, kWindowTitle);
      const bool line_seed_loaded = state->private_fonts.load_line_seed_jp(
          reinterpret_cast<HMODULE>(
              GetWindowLongPtrW(window, GWLP_HINSTANCE)));
      if (state->logger.has_value()) {
        state->logger->info(
            line_seed_loaded
                ? L"UIフォント LINE Seed JP 読込み成功"
                : L"UIフォント LINE Seed JP 読込み失敗: Yu Gothic UIへフォールバック");
      }
      state->body_font = CreateFontW(
          -18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
          CLEARTYPE_QUALITY, DEFAULT_PITCH,
          state->private_fonts.regular_face());
      state->small_font = CreateFontW(
          -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
          CLEARTYPE_QUALITY, DEFAULT_PITCH,
          state->private_fonts.regular_face());
      state->heading_font = CreateFontW(
          -25, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
          CLEARTYPE_QUALITY, DEFAULT_PITCH,
          state->private_fonts.bold_face());
      state->brand_font = CreateFontW(
          -24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
          CLEARTYPE_QUALITY, DEFAULT_PITCH,
          state->private_fonts.bold_face());
      if (state->body_font == nullptr || state->small_font == nullptr ||
          state->heading_font == nullptr || state->brand_font == nullptr) {
        return -1;
      }

      for (std::size_t index = 0;
           index < state->navigation.size();
           ++index) {
        state->navigation[index] = CreateWindowExW(
            0,
            L"BUTTON",
            kNavigationLabels[index].data(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0,
            0,
            0,
            0,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(
                    kNavFirstId + static_cast<int>(index))),
            nullptr,
            nullptr);
      }
      state->refresh = CreateWindowExW(
          0,
          L"BUTTON",
          L"再読み込み",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshId)),
          nullptr,
          nullptr);
      state->source_combo = CreateWindowExW(
          WS_EX_CLIENTEDGE,
          WC_COMBOBOXW,
          L"",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
              WS_VSCROLL,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kSourceComboId)),
          nullptr,
          nullptr);
      state->transfer_mode_combo = CreateWindowExW(
          WS_EX_CLIENTEDGE,
          WC_COMBOBOXW,
          L"",
          WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kTransferModeComboId)),
          nullptr,
          nullptr);
      state->target_combo = CreateWindowExW(
          WS_EX_CLIENTEDGE,
          WC_COMBOBOXW,
          L"",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
              WS_VSCROLL,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kTargetComboId)),
          nullptr,
          nullptr);
      state->restore_change_image = CreateWindowExW(
          0,
          L"BUTTON",
          L"別のイメージを選ぶ",
          WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kRestoreChangeImageId)),
          nullptr,
          nullptr);
      state->media_kind_combo = CreateWindowExW(
          WS_EX_CLIENTEDGE,
          WC_COMBOBOXW,
          L"",
          WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kMediaKindComboId)),
          nullptr,
          nullptr);
      state->media_profile_combo = CreateWindowExW(
          WS_EX_CLIENTEDGE,
          WC_COMBOBOXW,
          L"",
          WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kMediaProfileComboId)),
          nullptr,
          nullptr);
      state->media_output_edit = CreateWindowExW(
          WS_EX_CLIENTEDGE,
          L"EDIT",
          L"",
          WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kMediaOutputEditId)),
          nullptr,
          nullptr);
      state->media_browse = CreateWindowExW(
          0,
          L"BUTTON",
          L"保存先を選ぶ…",
          WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kMediaBrowseId)),
          nullptr,
          nullptr);
      state->primary_action = CreateWindowExW(
          0,
          L"BUTTON",
          L"安全確認へ",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kPrimaryActionId)),
          nullptr,
          nullptr);
      if (state->transfer_mode_combo == nullptr ||
          state->media_kind_combo == nullptr ||
          state->media_profile_combo == nullptr ||
          state->media_output_edit == nullptr ||
          state->media_browse == nullptr) {
        return -1;
      }
      constexpr std::array<std::wstring_view, 2> transfer_modes{
          L"通常モード（完全複製）",
          L"縮小移行モード（小容量へ）",
      };
      for (const auto label : transfer_modes) {
        SendMessageW(
            state->transfer_mode_combo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(label.data()));
      }
      SendMessageW(state->transfer_mode_combo, CB_SETCURSEL, 0, 0);
      const std::array<ytec::windowsapp::RescueMediaKind, 2>
          media_kinds{
              ytec::windowsapp::RescueMediaKind::iso_file,
              ytec::windowsapp::RescueMediaKind::usb_drive,
          };
      for (const auto kind : media_kinds) {
        const std::wstring label =
            ytec::windowsapp::rescue_media_kind_label(kind);
        SendMessageW(
            state->media_kind_combo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(label.c_str()));
      }
      SendMessageW(state->media_kind_combo, CB_SETCURSEL, 0, 0);
      const std::array<
          ytec::windowsapp::RescueMediaBootProfile,
          2>
          media_profiles{
              ytec::windowsapp::RescueMediaBootProfile::
                  windows_uefi_2011_ca,
              ytec::windowsapp::RescueMediaBootProfile::
                  windows_uefi_2023_ca,
          };
      for (const auto profile : media_profiles) {
        const std::wstring label =
            ytec::windowsapp::rescue_media_boot_profile_label(profile);
        SendMessageW(
            state->media_profile_combo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(label.c_str()));
      }
      SendMessageW(state->media_profile_combo, CB_SETCURSEL, 0, 0);
      SendMessageW(
          state->media_output_edit, EM_SETLIMITTEXT, 32767, 0);
      for (const HWND button : state->navigation) {
        set_control_font(button, state->body_font);
      }
      set_control_font(state->refresh, state->small_font);
      set_control_font(state->source_combo, state->small_font);
      set_control_font(state->target_combo, state->small_font);
      set_control_font(state->transfer_mode_combo, state->small_font);
      set_control_font(state->restore_change_image, state->body_font);
      set_control_font(state->media_kind_combo, state->small_font);
      set_control_font(state->media_profile_combo, state->small_font);
      set_control_font(state->media_output_edit, state->small_font);
      set_control_font(state->media_browse, state->small_font);
      set_control_font(state->primary_action, state->body_font);
      layout_controls(*state);
      update_action_state(*state);
      start_inventory(*state);
      return 0;
    }
    case WM_SIZE:
      if (state != nullptr) {
        layout_controls(*state);
        update_action_state(*state);
        InvalidateRect(window, nullptr, TRUE);
      }
      return 0;
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      info->ptMinTrackSize.x = 980;
      info->ptMinTrackSize.y = 640;
      return 0;
    }
    case WM_COMMAND:
      if (state == nullptr) {
        return 0;
      }
      if (HIWORD(wparam) == BN_CLICKED) {
        const int identifier = LOWORD(wparam);
        if (identifier >= kNavFirstId &&
            identifier <
                kNavFirstId +
                    static_cast<int>(state->navigation.size())) {
          if (state->media_creation_running.load()) {
            MessageBoxW(
                window,
                L"レスキューメディアの構成が完了するまで、この画面で進捗を確認してください。",
                L"レスキューメディアを作成中です",
                MB_OK | MB_ICONINFORMATION);
            return 0;
          }
          state->page =
              static_cast<Page>(identifier - kNavFirstId);
          if (state->page == Page::restore_image &&
              state->restore_preflight.has_value()) {
            select_default_restore_target(*state);
          } else if (
              state->page == Page::rescue_media &&
              selected_media_kind(*state) ==
                  ytec::windowsapp::RescueMediaKind::usb_drive) {
            select_default_media_usb_target(*state);
          }
          update_navigation_state(*state);
          update_action_state(*state);
          InvalidateRect(window, nullptr, TRUE);
          SetFocus(state->navigation[
              static_cast<std::size_t>(identifier - kNavFirstId)]);
          return 0;
        }
        if (identifier == kRefreshId) {
          start_inventory(*state);
          return 0;
        }
        if (identifier == kPrimaryActionId &&
            state->page == Page::clone) {
          create_clone_job_flow(*state);
          return 0;
        }
        if (identifier == kPrimaryActionId &&
            state->page == Page::create_image) {
          if (state->backup_running.load()) {
            state->backup_cancel_requested.store(true);
            if (state->logger.has_value()) {
              state->logger->info(
                  L"オンラインイメージ作成の安全な取消を要求");
            }
            update_action_state(*state);
            InvalidateRect(window, nullptr, FALSE);
          } else {
            create_online_backup_flow(*state);
          }
          return 0;
        }
        if (identifier == kPrimaryActionId &&
            state->page == Page::restore_image) {
          if (state->restore_preflight.has_value()) {
            create_restore_job_flow(*state);
          } else {
            create_restore_preflight_flow(*state);
          }
          return 0;
        }
        if (identifier == kRestoreChangeImageId &&
            state->page == Page::restore_image) {
          create_restore_preflight_flow(*state);
          return 0;
        }
        if (identifier == kPrimaryActionId &&
            state->page == Page::boot_repair) {
          state->page = Page::rescue_media;
          if (selected_media_kind(*state) ==
              ytec::windowsapp::RescueMediaKind::usb_drive) {
            select_default_media_usb_target(*state);
          }
          update_navigation_state(*state);
          update_action_state(*state);
          InvalidateRect(window, nullptr, TRUE);
          SetFocus(state->navigation[
              static_cast<std::size_t>(Page::rescue_media)]);
          return 0;
        }
        if (identifier == kPrimaryActionId &&
            state->page == Page::rescue_media) {
          if (state->media_creation_running.load()) {
            if (state->media_creation_progress.has_value() &&
                state->media_creation_progress->cancellation_allowed &&
                !state->media_creation_cancel_requested.exchange(true)) {
              if (state->logger.has_value()) {
                state->logger->info(
                    L"レスキューメディア作成の安全な取消を要求");
              }
              update_action_state(*state);
              InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
          }
          if (state->media_creation_report.has_value()) {
            const bool completed_usb =
                state->media_creation_report->complete_usb_verified;
            state->media_creation_report.reset();
            if (completed_usb) {
              start_inventory(*state);
              update_action_state(*state);
              InvalidateRect(window, nullptr, TRUE);
            } else {
              SetWindowTextW(state->media_output_edit, L"");
              choose_media_iso_destination(*state);
            }
          } else if (!state->media_preflight.has_value() ||
              !state->media_preflight->media_creation_permitted) {
            if (state->media_preflight.has_value()) {
              show_adk_install_guide(*state);
            } else {
              start_media_preflight(*state);
            }
          } else {
            const auto plan = current_rescue_media_plan(*state);
            if (plan.issue ==
                    ytec::windowsapp::RescueMediaPlanIssue::
                        iso_destination_missing ||
                plan.issue ==
                    ytec::windowsapp::RescueMediaPlanIssue::
                        iso_destination_invalid) {
              choose_media_iso_destination(*state);
            } else if (plan.ready_for_confirmation) {
              review_rescue_media_plan(*state);
            }
          }
          return 0;
        }
        if (identifier == kMediaBrowseId &&
            state->page == Page::rescue_media) {
          choose_media_iso_destination(*state);
          return 0;
        }
        if (identifier == kPrimaryActionId &&
            state->page == Page::diagnostics) {
          start_inventory(*state);
          return 0;
        }
      }
      if ((LOWORD(wparam) == kSourceComboId ||
           LOWORD(wparam) == kTargetComboId ||
           LOWORD(wparam) == kTransferModeComboId) &&
          HIWORD(wparam) == CBN_SELCHANGE) {
        update_action_state(*state);
        InvalidateRect(window, nullptr, FALSE);
      }
      if ((LOWORD(wparam) == kMediaKindComboId ||
           LOWORD(wparam) == kMediaProfileComboId) &&
          HIWORD(wparam) == CBN_SELCHANGE) {
        if (LOWORD(wparam) == kMediaKindComboId &&
            selected_media_kind(*state) ==
                ytec::windowsapp::RescueMediaKind::usb_drive) {
          select_default_media_usb_target(*state);
        }
        update_action_state(*state);
        InvalidateRect(window, nullptr, TRUE);
      }
      if (LOWORD(wparam) == kMediaOutputEditId &&
          HIWORD(wparam) == EN_CHANGE) {
        update_action_state(*state);
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    case WM_DRAWITEM:
      if (state != nullptr) {
        const auto* item =
            reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (item->CtlID >= kNavFirstId &&
            item->CtlID <
                static_cast<UINT>(
                    kNavFirstId + state->navigation.size())) {
          draw_navigation_button(*state, *item);
          return TRUE;
        }
      }
      break;
    case kInventoryCompleteMessage: {
      std::unique_ptr<InventoryPayload> payload(
          reinterpret_cast<InventoryPayload*>(lparam));
      if (state != nullptr) {
        state->inventory_loading.store(false);
        if (state->inventory_thread.joinable()) {
          state->inventory_thread.join();
        }
        state->inventory = std::move(payload->report);
        state->inventory_error = std::move(payload->error);
        state->job_results = std::move(payload->job_results);
        state->job_result_error = std::move(payload->job_result_error);
        EnableWindow(state->refresh, TRUE);
        populate_disk_combos(*state);
        InvalidateRect(window, nullptr, TRUE);
      }
      return 0;
    }
    case kBackupProgressMessage: {
      std::unique_ptr<BackupProgressPayload> payload(
          reinterpret_cast<BackupProgressPayload*>(lparam));
      if (state != nullptr) {
        state->backup_progress = std::move(payload->progress);
        state->backup_elapsed = payload->elapsed;
        update_action_state(*state);
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }
    case kBackupCompleteMessage: {
      std::unique_ptr<BackupPayload> payload(
          reinterpret_cast<BackupPayload*>(lparam));
      if (state != nullptr) {
        state->backup_running.store(false);
        if (state->backup_thread.joinable()) {
          state->backup_thread.join();
        }
        update_action_state(*state);
        InvalidateRect(window, nullptr, TRUE);
        if (payload->report.has_value()) {
          const auto& report = payload->report.value();
          if (state->logger.has_value()) {
            state->logger->info(
                L"オンラインイメージ作成完了 image_bytes=" +
                std::to_wstring(report.image.image_length) +
                L" final_committed_after_vss=" +
                (report.final_file_committed_after_vss
                     ? L"true"
                     : L"false"));
          }
          const std::wstring completion_text =
              L"オンラインイメージを作成し、読戻し検証しました。\n\n" +
              payload->final_path +
              L"\n\nイメージ容量: " +
              format_bytes(report.image.image_length) +
              L"\nVSS Snapshot削除後の確定: " +
              (report.final_file_committed_after_vss ? L"完了" : L"未完了");
          MessageBoxW(
              window,
              completion_text.c_str(),
              kWindowTitle,
              MB_OK | MB_ICONINFORMATION);
        } else if (payload->shrink_report.has_value()) {
          const auto& report = payload->shrink_report.value();
          if (state->logger.has_value()) {
            state->logger->info(
                L"オンライン縮小移行イメージ作成完了 payload_bytes=" +
                std::to_wstring(report.bundle.total_payload_bytes) +
                L" volumes=" +
                std::to_wstring(report.bundle.captured_volume_count) +
                L" final_committed_after_vss_cleanup=" +
                (report.final_bundle_committed_after_vss_cleanup
                     ? L"true"
                     : L"false"));
          }
          const std::wstring completion_text =
              L"縮小移行イメージを作成し、全WIMとマニフェストをSHA-256で検証しました。\n\n" +
              payload->final_path +
              L"\n\n保存データ: " +
              format_bytes(report.bundle.total_payload_bytes) +
              L"\n対象ボリューム: " +
              std::to_wstring(report.bundle.captured_volume_count) +
              L"\nVSS Snapshot削除後の確定: " +
              (report.final_bundle_committed_after_vss_cleanup
                   ? L"完了"
                   : L"未完了");
          MessageBoxW(
              window,
              completion_text.c_str(),
              L"縮小移行イメージの作成完了",
              MB_OK | MB_ICONINFORMATION);
        } else if (payload->error.has_value()) {
          log_error_summary(
              state->logger,
              payload->error->native_code == ERROR_CANCELLED
                  ? L"オンラインイメージ作成キャンセル"
                  : L"オンラインイメージ作成失敗",
              payload->error.value());
          const std::wstring error_text =
              format_error_message(payload->error.value());
          MessageBoxW(
              window,
              error_text.c_str(),
              kWindowTitle,
              MB_OK | MB_ICONERROR);
        }
      }
      return 0;
    }
    case kMediaPreflightCompleteMessage: {
      std::unique_ptr<MediaPreflightPayload> payload(
          reinterpret_cast<MediaPreflightPayload*>(lparam));
      if (state != nullptr) {
        state->media_preflight_running.store(false);
        if (state->media_preflight_thread.joinable()) {
          state->media_preflight_thread.join();
        }
        state->media_preflight = std::move(payload->view);
        if (state->logger.has_value()) {
          const auto& view = state->media_preflight.value();
          state->logger->info(
              L"レスキューメディア作成環境診断完了 permitted=" +
              std::wstring(
                  view.media_creation_permitted ? L"true" : L"false") +
              L" base_layout=" +
              (view.base_layout_ready ? L"true" : L"false") +
              L" bootex_layout=" +
              (view.bootex_layout_ready ? L"true" : L"false"));
        }
        if (selected_media_kind(*state) ==
            ytec::windowsapp::RescueMediaKind::usb_drive) {
          select_default_media_usb_target(*state);
        }
        update_action_state(*state);
        InvalidateRect(window, nullptr, TRUE);
        const UINT icon =
            state->media_preflight->media_creation_permitted
                ? MB_ICONINFORMATION
                : MB_ICONWARNING;
        const std::wstring message_text =
            state->media_preflight->status + L"\n\n" +
            state->media_preflight->details +
            L"\n\nこの確認ではメディアを作成していません。";
        MessageBoxW(
            window,
            message_text.c_str(),
            L"レスキューメディア作成前診断",
            MB_OK | icon);
      }
      return 0;
    }
    case kMediaCreationProgressMessage: {
      std::unique_ptr<MediaCreationProgressPayload> payload(
          reinterpret_cast<MediaCreationProgressPayload*>(lparam));
      if (state != nullptr) {
        if (state->logger.has_value() &&
            (!state->last_logged_media_stage.has_value() ||
             state->last_logged_media_stage.value() !=
                 payload->progress.stage)) {
          state->last_logged_media_stage = payload->progress.stage;
          state->logger->info(
              L"レスキューメディア作成進捗 stage=" +
              std::wstring(
                  ytec::windowsapp::
                      rescue_media_creation_stage_label(
                          payload->progress.stage)) +
              L" percent=" +
              std::to_wstring(payload->progress.percent));
        }
        state->media_creation_progress =
            std::move(payload->progress);
        update_action_state(*state);
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }
    case kMediaCreationCompleteMessage: {
      std::unique_ptr<MediaCreationPayload> payload(
          reinterpret_cast<MediaCreationPayload*>(lparam));
      if (state != nullptr) {
        state->media_creation_running.store(false);
        KillTimer(window, kUiRefreshTimerId);
        if (state->media_creation_thread.joinable()) {
          state->media_creation_thread.join();
        }
        if (payload->report.has_value()) {
          state->media_creation_report =
              std::move(payload->report.value());
          const auto& report = state->media_creation_report.value();
          const bool usb = report.complete_usb_verified;
          if (state->logger.has_value()) {
            if (usb) {
              state->logger->info(
                  L"レスキューUSB作成・全ファイル検証完了 root=" +
                  report.usb_root_path + L" boot_wim_sha256=" +
                  std::wstring(
                      report.usb_boot_wim_sha256.begin(),
                      report.usb_boot_wim_sha256.end()));
            } else {
              state->logger->info(
                  L"レスキューISO作成・全量検証完了 iso_bytes=" +
                  std::to_wstring(report.iso_length) + L" sha256=" +
                  std::wstring(
                      report.iso_sha256.begin(),
                      report.iso_sha256.end()));
            }
          }
          const std::wstring completion = usb
              ? L"レスキューUSBを作成し、全ファイルをSHA-256で"
                L"読み戻し検証しました。\n\n作成先: " +
                    report.usb_root_path +
                    L"\nboot.wim SHA-256:\n" +
                    std::wstring(
                        report.usb_boot_wim_sha256.begin(),
                        report.usb_boot_wim_sha256.end()) +
                    L"\n\n検証記録:\n" + report.manifest_path +
                    L"\n\n一時作業フォルダー:\n" +
                    report.retained_work_root +
                    L"\n現在は監査用に保持しています。"
              : L"レスキューISOを作成し、全量SHA-256検証しました。\n\n" +
                    report.final_iso_path +
                    L"\n\n容量: " + format_bytes(report.iso_length) +
                    L"\nSHA-256:\n" +
                    std::wstring(
                        report.iso_sha256.begin(),
                        report.iso_sha256.end()) +
                    L"\n\n検証記録:\n" + report.manifest_path +
                    L"\n\n一時作業フォルダー:\n" +
                    report.retained_work_root +
                    L"\n現在は監査用に保持しています。";
          MessageBoxW(
              window,
              completion.c_str(),
              usb ? L"レスキューUSBの作成完了"
                  : L"レスキューISOの作成完了",
              MB_OK | MB_ICONINFORMATION);
        } else if (payload->error.has_value() &&
                   payload->error->native_code != ERROR_CANCELLED) {
          log_error_summary(
              state->logger,
              L"レスキューメディア作成失敗",
              payload->error.value());
          const bool usb =
              selected_media_kind(*state) ==
              ytec::windowsapp::RescueMediaKind::usb_drive;
          const std::wstring error =
              format_error_message(payload->error.value()) +
              (usb
                   ? L"\n\n完了扱いにはしていません。USBと監査ログを確認してください。"
                   : L"\n\n既存ISOは上書きしていません。"
                     L"\n途中作業がある場合は選択した保存先とログを確認してください。");
          MessageBoxW(
              window,
              error.c_str(),
              usb ? L"レスキューUSBを作成できませんでした"
                  : L"レスキューISOを作成できませんでした",
              MB_OK | MB_ICONERROR);
        } else if (payload->error.has_value()) {
          log_error_summary(
              state->logger,
              L"レスキューメディア作成キャンセル",
              payload->error.value());
        }
        update_action_state(*state);
        InvalidateRect(window, nullptr, TRUE);
      }
      return 0;
    }
    case kRestorePreflightCompleteMessage: {
      std::unique_ptr<RestorePreflightPayload> payload(
          reinterpret_cast<RestorePreflightPayload*>(lparam));
      if (state != nullptr) {
        state->restore_preflight_running.store(false);
        if (state->restore_preflight_thread.joinable()) {
          state->restore_preflight_thread.join();
        }
        if (payload->report.has_value()) {
          state->restore_preflight =
              std::move(payload->report.value());
          select_default_restore_target(*state);
          update_action_state(*state);
          const auto& report = state->restore_preflight.value();
          const bool shrink = report.shrink_manifest.has_value();
          const std::size_t payload_count = shrink
              ? static_cast<std::size_t>(std::count_if(
                    report.shrink_manifest->partitions.begin(),
                    report.shrink_manifest->partitions.end(),
                    [](const auto& partition) {
                      return !partition.payload_file_name.empty();
                    }))
              : static_cast<std::size_t>(report.header.chunk_count);
          if (state->logger.has_value()) {
            state->logger->info(
                L"復元イメージの読み取り専用完全検証完了 image_bytes=" +
                std::to_wstring(report.image_length) +
                L" partitions=" +
                std::to_wstring(report.manifest.partitions.size()) +
                (shrink ? L" wims=" : L" chunks=") +
                std::to_wstring(payload_count));
          }
          const std::wstring completion_text =
              std::wstring(
                  shrink
                      ? L"縮小移行イメージの全WIM・マニフェスト検証に合格しました。\n\n"
                      : L"通常イメージの読み取り専用完全検証に合格しました。\n\n") +
              report.canonical_path +
              L"\n\nイメージ容量: " +
              format_bytes(report.image_length) +
              L"\n元ディスク容量: " +
              format_bytes(report.header.source_disk_size) +
              L"\nパーティション: " +
              std::to_wstring(report.manifest.partitions.size()) +
              (shrink ? L"\nWIM: " : L"\nチャンク: ") +
              std::to_wstring(payload_count) +
              L"\n\n復元先ディスクは開いていません。"
              L"\n復元実行はまだ無効です。";
          MessageBoxW(
              window,
              completion_text.c_str(),
              L"イメージ復元前の完全検証",
              MB_OK | MB_ICONINFORMATION);
        } else if (payload->error.has_value() &&
                   payload->error->native_code != ERROR_CANCELLED) {
          log_error_summary(
              state->logger,
              L"復元イメージの読み取り専用完全検証失敗",
              payload->error.value());
          update_action_state(*state);
          const std::wstring error_text =
              format_error_message(payload->error.value());
          MessageBoxW(
              window,
              error_text.c_str(),
              L"イメージを検証できませんでした",
              MB_OK | MB_ICONERROR);
        } else {
          if (payload->error.has_value()) {
            log_error_summary(
                state->logger,
                L"復元イメージの読み取り専用完全検証キャンセル",
                payload->error.value());
          }
          update_action_state(*state);
        }
        InvalidateRect(window, nullptr, TRUE);
      }
      return 0;
    }
    case WM_TIMER:
      if (state != nullptr && wparam == kUiRefreshTimerId &&
          state->media_creation_running.load()) {
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      break;
    case WM_CLOSE:
      if (state != nullptr &&
          state->media_creation_running.load()) {
        MessageBoxW(
            window,
            L"WinPEイメージまたはISOを構成中です。"
            L"\n安全に完了するまでこのアプリを終了できません。",
            L"レスキューISOを作成中です",
            MB_OK | MB_ICONWARNING);
        return 0;
      }
      break;
    case WM_ERASEBKGND:
      return TRUE;
    case WM_PAINT: {
      if (state == nullptr) {
        break;
      }
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(window, &paint);
      RECT client{};
      GetClientRect(window, &client);
      HDC buffer = CreateCompatibleDC(dc);
      HBITMAP bitmap = CreateCompatibleBitmap(
          dc, client.right, client.bottom);
      const auto previous =
          static_cast<HBITMAP>(SelectObject(buffer, bitmap));
      paint_window(*state, buffer);
      BitBlt(
          dc,
          0,
          0,
          client.right,
          client.bottom,
          buffer,
          0,
          0,
          SRCCOPY);
      SelectObject(buffer, previous);
      DeleteObject(bitmap);
      DeleteDC(buffer);
      EndPaint(window, &paint);
      return 0;
    }
    case WM_DESTROY:
      if (state != nullptr) {
        state->backup_cancel_requested.store(true);
        state->restore_preflight_cancel_requested.store(true);
        state->media_creation_cancel_requested.store(true);
        KillTimer(window, kUiRefreshTimerId);
        if (state->inventory_thread.joinable()) {
          state->inventory_thread.join();
        }
        if (state->backup_thread.joinable()) {
          state->backup_thread.join();
        }
        if (state->media_preflight_thread.joinable()) {
          state->media_preflight_thread.join();
        }
        if (state->media_creation_thread.joinable()) {
          state->media_creation_thread.join();
        }
        if (state->restore_preflight_thread.joinable()) {
          state->restore_preflight_thread.join();
        }
        if (state->logger.has_value()) {
          state->logger->info(L"Y-TEC Tsumugi Drive 終了");
        }
        DeleteObject(state->body_font);
        DeleteObject(state->small_font);
        DeleteObject(state->heading_font);
        DeleteObject(state->brand_font);
      }
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(
    _In_ const HINSTANCE instance,
    _In_opt_ HINSTANCE,
    _In_ PWSTR,
    _In_ const int show_command) {
  const UiComApartment com_apartment;
  if (!com_apartment.initialized()) {
    const std::wstring error =
        L"アプリケーションのCOMを初期化できませんでした。\nWindows error: " +
        std::to_wstring(
            static_cast<std::uint32_t>(com_apartment.result()));
    MessageBoxW(
        nullptr,
        error.c_str(),
        kWindowTitle,
        MB_OK | MB_ICONERROR);
    return 1;
  }
  const auto com_security =
      ytec::vssrequester::initialize_vss_process_security();
  if (!com_security) {
    const std::wstring error =
        format_error_message(com_security.error());
    MessageBoxW(
        nullptr,
        error.c_str(),
        kWindowTitle,
        MB_OK | MB_ICONERROR);
    return 1;
  }

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  INITCOMMONCONTROLSEX common_controls{
      .dwSize = sizeof(INITCOMMONCONTROLSEX),
      .dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
  InitCommonControlsEx(&common_controls);

  WNDCLASSEXW window_class{
      .cbSize = sizeof(WNDCLASSEXW),
      .style = CS_HREDRAW | CS_VREDRAW,
      .lpfnWndProc = window_proc,
      .hInstance = instance,
      .hIcon = LoadIconW(nullptr, IDI_APPLICATION),
      .hCursor = LoadCursorW(nullptr, IDC_ARROW),
      .hbrBackground =
          static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)),
      .lpszClassName = kWindowClass,
      .hIconSm = LoadIconW(nullptr, IDI_APPLICATION)};
  if (RegisterClassExW(&window_class) == 0) {
    MessageBoxW(
        nullptr,
        L"アプリケーション画面を初期化できませんでした。",
        kWindowTitle,
        MB_OK | MB_ICONERROR);
    return 1;
  }

  AppState state;
  const HWND window = CreateWindowExW(
      0,
      kWindowClass,
      kWindowTitle,
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      1280,
      720,
      nullptr,
      nullptr,
      instance,
      &state);
  if (window == nullptr) {
    MessageBoxW(
        nullptr,
        L"メイン画面を作成できませんでした。",
        kWindowTitle,
        MB_OK | MB_ICONERROR);
    return 1;
  }

  constexpr BOOL enable_dark_title = TRUE;
  static_cast<void>(DwmSetWindowAttribute(
      window,
      20,
      &enable_dark_title,
      sizeof(enable_dark_title)));
  ShowWindow(window, show_command);
  UpdateWindow(window);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (IsDialogMessageW(window, &message) == FALSE) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  return static_cast<int>(message.wParam);
}
