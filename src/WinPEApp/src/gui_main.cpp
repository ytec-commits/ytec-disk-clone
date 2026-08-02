#include "ytec/bootrepair/standalone_repair.h"
#include "ytec/bootrepair/winre_diagnostic.h"
#include "ytec/clonecore/log.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/imageformat/job_file.h"
#include "ytec/imageformat/job_manifest.h"
#include "ytec/uisupport/private_fonts.h"
#include "ytec/winpeapp/app_runner.h"
#include "ytec/winpeapp/dashboard.h"
#include "ytec/winpeapp/job_result.h"
#include "ytec/winpeapp/repair_layout.h"
#include "ytec/winpeapp/winre_diagnostic_view.h"

#include <Windows.h>
#include <commdlg.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef YTEC_WINPE_PRODUCT_BOUNDARY
#error WinPE GUI must be built with the product safety boundary enabled.
#endif

namespace {

constexpr wchar_t kWindowClass[] = L"YtecTsumugiDriveWinPeWindow";
// WinPE's non-client title bar cannot reliably use the bundled Japanese font.
// Keep the OS-managed caption ASCII-only; Japanese remains available inside
// the client area where the bundled LINE Seed JP font is applied.
constexpr wchar_t kWindowTitle[] = L"Y-TEC Tsumugi Drive - WinPE";
static_assert([] {
  for (const auto character : kWindowTitle) {
    if (character > 0x7f) {
      return false;
    }
  }
  return true;
}(), "The WinPE non-client caption must remain ASCII-only.");
constexpr wchar_t kAutoSystemPartitionLabel[] = L"自動：未割当領域";
constexpr int kJobPathLabelTop = 179;
constexpr int kJobPathLabelBottom = 200;
constexpr int kJobPathControlTop = 210;
static_assert(
    kJobPathControlTop - kJobPathLabelBottom >= 8,
    "The WinPE job label and path controls need a visible vertical gap.");
constexpr int kSidebarConceptTop = 134;
constexpr int kSidebarConceptBottom = 160;
constexpr int kSidebarNavTop = 176;
constexpr int kSidebarNavHeight = 44;
constexpr int kSidebarNavPitch = 52;
constexpr int kSidebarModeTop = 342;
constexpr int kRepairConfirmTitleTop = 328;
constexpr int kRepairConfirmTitleBottom = 350;
constexpr int kRepairAcknowledgeTop = 358;
constexpr int kRepairAcknowledgeHeight = 24;
constexpr int kRepairTokenLabelTop = 390;
constexpr int kRepairTokenLabelBottom = 411;
constexpr int kRepairTokenControlTop = 419;
constexpr int kRepairTokenControlHeight = 30;
constexpr int kRepairResultLabelTop = 461;
constexpr int kRepairResultLabelBottom = 484;
constexpr int kRepairOutputTop = 490;
static_assert(
    kSidebarNavTop - kSidebarConceptBottom >= 12,
    "The sidebar concept line and first navigation button need a visible gap.");
static_assert(
    kSidebarModeTop -
            (kSidebarNavTop + 2 * kSidebarNavPitch + kSidebarNavHeight) >=
        12,
    "The sidebar navigation and environment label need a visible gap.");
static_assert(
    kRepairAcknowledgeTop - kRepairConfirmTitleBottom >= 8 &&
        kRepairTokenLabelTop -
                (kRepairAcknowledgeTop + kRepairAcknowledgeHeight) >=
            8 &&
        kRepairTokenControlTop - kRepairTokenLabelBottom >= 8 &&
        kRepairResultLabelTop -
                (kRepairTokenControlTop + kRepairTokenControlHeight) >=
            8 &&
        kRepairOutputTop - kRepairResultLabelBottom >= 6,
    "The WinPE repair confirmation text and controls must not overlap.");

constexpr UINT kInventoryCompleteMessage = WM_APP + 1U;
constexpr UINT kJobCheckCompleteMessage = WM_APP + 2U;
constexpr UINT kBootInspectCompleteMessage = WM_APP + 3U;
constexpr UINT kBootExecuteCompleteMessage = WM_APP + 4U;
constexpr UINT kJobProgressMessage = WM_APP + 5U;
constexpr UINT kJobExecuteCompleteMessage = WM_APP + 6U;
constexpr UINT kWinReDiagnosticCompleteMessage = WM_APP + 7U;
constexpr UINT kJobDiscoveryCompleteMessage = WM_APP + 8U;

constexpr int kNavJobId = 100;
constexpr int kNavRepairId = 101;
constexpr int kNavDiskId = 102;
constexpr int kRefreshId = 200;
constexpr int kJobPathId = 201;
constexpr int kJobBrowseId = 202;
constexpr int kJobCheckId = 203;
constexpr int kJobOutputId = 204;
constexpr int kJobAcknowledgeId = 205;
constexpr int kJobTokenId = 206;
constexpr int kJobExecuteId = 207;
constexpr int kJobCancelId = 208;
constexpr int kDiskListId = 210;
constexpr int kDiskDetailsId = 211;
constexpr int kRepairDiskId = 220;
constexpr int kWindowsRootId = 221;
constexpr int kSystemRootId = 222;
constexpr int kFirmwareId = 223;
constexpr int kRepairInspectId = 224;
constexpr int kRepairAcknowledgeId = 225;
constexpr int kRepairTokenId = 226;
constexpr int kRepairExecuteId = 227;
constexpr int kRepairOutputId = 228;
constexpr int kWinReDiagnosticId = 229;

constexpr COLORREF kCanvas = RGB(244, 247, 249);
constexpr COLORREF kSidebar = RGB(29, 37, 49);
constexpr COLORREF kSidebarSelected = RGB(51, 64, 81);
constexpr COLORREF kCard = RGB(255, 255, 255);
constexpr COLORREF kInk = RGB(29, 40, 52);
constexpr COLORREF kMuted = RGB(91, 105, 118);
constexpr COLORREF kBorder = RGB(214, 222, 228);
constexpr COLORREF kTsumugiBlue = RGB(30, 145, 160);
constexpr COLORREF kTsumugiPurple = RGB(121, 91, 174);
constexpr COLORREF kSafeGreen = RGB(42, 137, 93);
constexpr COLORREF kWarning = RGB(183, 112, 26);
constexpr COLORREF kDanger = RGB(179, 59, 59);

enum class Page : std::uint8_t {
  job,
  boot_repair,
  disk_diagnostics,
};

enum class BrowseResult : std::uint8_t {
  selected,
  cancelled,
  unavailable,
};

enum class JobExecutionKind : std::uint8_t {
  none,
  clone,
  mbr_to_gpt,
  restore,
};

bool is_copy_job(const JobExecutionKind kind) noexcept {
  return kind == JobExecutionKind::clone ||
      kind == JobExecutionKind::mbr_to_gpt;
}

std::wstring_view job_operation_label(
    const JobExecutionKind kind) noexcept {
  if (kind == JobExecutionKind::mbr_to_gpt) {
    return L"MBR→GPT移行";
  }
  return kind == JobExecutionKind::clone ? L"クローン" : L"復元";
}

std::wstring_view job_target_label(
    const JobExecutionKind kind) noexcept {
  return is_copy_job(kind) ? L"コピー先" : L"復元先";
}

struct InventoryPayload final {
  std::optional<ytec::diskmodel::InventoryReport> inventory;
  std::wstring error;
};

struct JobDiscoveryPayload final {
  std::optional<std::wstring> path;
  std::wstring error;
};

struct JobCheckPayload final {
  bool success{};
  JobExecutionKind kind{JobExecutionKind::none};
  bool execution_ready{};
  ytec::imageformat::JobExecutionMode execution_mode{
      ytec::imageformat::JobExecutionMode::review_required};
  std::optional<ytec::imageformat::Sha256Digest> job_payload_hash;
  std::wstring confirmation_token;
  std::wstring output;
};

struct JobProgressPayload final {
  ytec::clonecore::DiskOperationProgress progress;
  std::chrono::milliseconds elapsed{};
};

struct JobExecutePayload final {
  bool success{};
  JobExecutionKind kind{JobExecutionKind::none};
  std::wstring output;
  std::wstring result_log_path;
  std::wstring result_log_error;
};

struct BootInspectPayload final {
  std::optional<ytec::bootrepair::BootRepairTargetSelection> selection;
  std::wstring error;
};

struct BootExecutePayload final {
  bool success{};
  std::wstring output;
};

struct WinReDiagnosticPayload final {
  std::wstring output;
};

struct AppState final {
  HWND window{};
  Page page{Page::job};
  bool inventory_busy{};
  bool job_discovery_busy{};
  bool operation_busy{};
  int job_step{1};
  int repair_step{1};
  ytec::winpeapp::DashboardView dashboard;
  std::optional<ytec::diskmodel::InventoryReport> inventory;
  std::optional<ytec::bootrepair::BootRepairTargetRequest>
      inspected_boot_request;
  std::optional<ytec::bootrepair::BootRepairTargetSelection>
      inspected_boot_selection;
  bool job_execution_ready{};
  JobExecutionKind job_execution_kind{JobExecutionKind::none};
  ytec::imageformat::JobExecutionMode job_execution_mode{
      ytec::imageformat::JobExecutionMode::review_required};
  bool job_progress_active{};
  ytec::clonecore::DiskOperationProgress job_progress;
  std::chrono::milliseconds job_elapsed{};
  std::optional<ytec::imageformat::Sha256Digest> job_payload_hash;
  std::wstring job_confirmation_token;
  std::shared_ptr<std::atomic_bool> job_cancellation;
  std::wstring boot_confirmation_token;
  std::vector<std::wstring> roots;

  HWND nav_job{};
  HWND nav_repair{};
  HWND nav_disk{};
  HWND refresh{};
  HWND job_path{};
  HWND job_browse{};
  HWND job_check{};
  HWND job_acknowledge{};
  HWND job_token{};
  HWND job_execute{};
  HWND job_cancel{};
  HWND job_output{};
  HWND disk_list{};
  HWND disk_details{};
  HWND repair_disk{};
  HWND windows_root{};
  HWND system_root{};
  HWND firmware{};
  HWND repair_inspect{};
  HWND winre_diagnostic{};
  HWND repair_acknowledge{};
  HWND repair_token{};
  HWND repair_execute{};
  HWND repair_output{};

  HFONT title_font{};
  HFONT heading_font{};
  HFONT body_font{};
  HFONT small_font{};
  HFONT mono_font{};
  ytec::uisupport::PrivateFontCollection private_fonts;
  HBRUSH canvas_brush{};
  HBRUSH card_brush{};
};

std::wstring format_error(const ytec::clonecore::Error& error) {
  std::wstring text = error.operation;
  if (!error.message.empty()) {
    if (!text.empty()) {
      text += L"\r\n";
    }
    text += error.message;
  }
  if (text.empty()) {
    text = L"安全確認に失敗しました。";
  }
  return text;
}

std::wstring utf8_to_wide(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return L"表示できる上限を超えた診断結果です。";
  }
  const int input_length = static_cast<int>(text.size());
  const int length = MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      text.data(),
      input_length,
      nullptr,
      0);
  if (length <= 0) {
    return L"診断結果の文字コードを安全に解釈できません。";
  }
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  if (MultiByteToWideChar(
          CP_UTF8,
          MB_ERR_INVALID_CHARS,
          text.data(),
          input_length,
          result.data(),
          length) != length) {
    return L"診断結果の文字コードを安全に解釈できません。";
  }
  return result;
}

bool required_restore_checks_passed(const std::string& output) {
  constexpr std::string_view kMarker = "\n必須検査合格: ";
  const std::size_t marker = output.rfind(kMarker);
  if (marker == std::string::npos) {
    return false;
  }
  constexpr std::string_view kPassed = "はい\n";
  const std::size_t value = marker + kMarker.size();
  return output.size() >= value + kPassed.size() &&
      output.compare(value, kPassed.size(), kPassed) == 0;
}

void post_payload(
    const HWND window,
    const UINT message,
    std::unique_ptr<InventoryPayload> payload) {
  InventoryPayload* const raw = payload.release();
  if (PostMessageW(
          window,
          message,
          0,
          reinterpret_cast<LPARAM>(raw)) == FALSE) {
    delete raw;
  }
}

void post_payload(
    const HWND window,
    const UINT message,
    std::unique_ptr<JobDiscoveryPayload> payload) {
  JobDiscoveryPayload* const raw = payload.release();
  if (PostMessageW(
          window,
          message,
          0,
          reinterpret_cast<LPARAM>(raw)) == FALSE) {
    delete raw;
  }
}

void post_payload(
    const HWND window,
    const UINT message,
    std::unique_ptr<JobCheckPayload> payload) {
  JobCheckPayload* const raw = payload.release();
  if (PostMessageW(
          window,
          message,
          0,
          reinterpret_cast<LPARAM>(raw)) == FALSE) {
    delete raw;
  }
}

void post_payload(
    const HWND window,
    const UINT message,
    std::unique_ptr<JobProgressPayload> payload) {
  JobProgressPayload* const raw = payload.release();
  if (PostMessageW(
          window,
          message,
          0,
          reinterpret_cast<LPARAM>(raw)) == FALSE) {
    delete raw;
  }
}

void post_payload(
    const HWND window,
    const UINT message,
    std::unique_ptr<JobExecutePayload> payload) {
  JobExecutePayload* const raw = payload.release();
  if (PostMessageW(
          window,
          message,
          0,
          reinterpret_cast<LPARAM>(raw)) == FALSE) {
    delete raw;
  }
}

void post_payload(
    const HWND window,
    const UINT message,
    std::unique_ptr<BootInspectPayload> payload) {
  BootInspectPayload* const raw = payload.release();
  if (PostMessageW(
          window,
          message,
          0,
          reinterpret_cast<LPARAM>(raw)) == FALSE) {
    delete raw;
  }
}

void post_payload(
    const HWND window,
    const UINT message,
    std::unique_ptr<BootExecutePayload> payload) {
  BootExecutePayload* const raw = payload.release();
  if (PostMessageW(
          window,
          message,
          0,
          reinterpret_cast<LPARAM>(raw)) == FALSE) {
    delete raw;
  }
}

void post_payload(
    const HWND window,
    const UINT message,
    std::unique_ptr<WinReDiagnosticPayload> payload) {
  WinReDiagnosticPayload* const raw = payload.release();
  if (PostMessageW(
          window,
          message,
          0,
          reinterpret_cast<LPARAM>(raw)) == FALSE) {
    delete raw;
  }
}

std::wstring control_text(const HWND control) {
  const int length = GetWindowTextLengthW(control);
  if (length <= 0) {
    return {};
  }
  std::wstring result(static_cast<std::size_t>(length) + 1U, L'\0');
  const int copied =
      GetWindowTextW(control, result.data(), length + 1);
  result.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0U);
  return result;
}

void set_control_font(const HWND control, const HFONT font) {
  SendMessageW(
      control,
      WM_SETFONT,
      reinterpret_cast<WPARAM>(font),
      TRUE);
}

void set_control_visible(const HWND control, const bool visible) {
  ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
}

void set_control_enabled(const HWND control, const bool enabled) {
  EnableWindow(control, enabled ? TRUE : FALSE);
}

HWND create_control(
    const AppState& state,
    const wchar_t* const class_name,
    const wchar_t* const text,
    const DWORD style,
    const int id) {
  return CreateWindowExW(
      0,
      class_name,
      text,
      WS_CHILD | WS_VISIBLE | style,
      0,
      0,
      10,
      10,
      state.window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandleW(nullptr),
      nullptr);
}

void draw_text(
    HDC dc,
    const std::wstring_view text,
    RECT bounds,
    const HFONT font,
    const COLORREF color,
    const UINT format) {
  const HGDIOBJ old_font = SelectObject(dc, font);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, color);
  DrawTextW(
      dc,
      text.data(),
      static_cast<int>(text.size()),
      &bounds,
      format);
  SelectObject(dc, old_font);
}

void fill_rounded_rect(
    HDC dc,
    const RECT& bounds,
    const COLORREF fill,
    const COLORREF border) {
  const HBRUSH brush = CreateSolidBrush(fill);
  const HPEN pen = CreatePen(PS_SOLID, 1, border);
  const HGDIOBJ old_brush = SelectObject(dc, brush);
  const HGDIOBJ old_pen = SelectObject(dc, pen);
  RoundRect(
      dc,
      bounds.left,
      bounds.top,
      bounds.right,
      bounds.bottom,
      14,
      14);
  SelectObject(dc, old_pen);
  SelectObject(dc, old_brush);
  DeleteObject(pen);
  DeleteObject(brush);
}

void draw_metric_tile(
    const AppState& state,
    HDC dc,
    const RECT& bounds,
    const std::wstring_view label,
    const std::wstring_view value,
    const COLORREF accent,
    const COLORREF fill) {
  fill_rounded_rect(dc, bounds, fill, accent);
  RECT accent_bar = bounds;
  accent_bar.right = accent_bar.left + 4;
  const HBRUSH accent_brush = CreateSolidBrush(accent);
  FillRect(dc, &accent_bar, accent_brush);
  DeleteObject(accent_brush);

  RECT label_bounds{
      bounds.left + 12,
      bounds.top + 5,
      bounds.right - 8,
      bounds.top + 22};
  draw_text(
      dc,
      label,
      label_bounds,
      state.small_font,
      accent,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
  RECT value_bounds{
      bounds.left + 12,
      bounds.top + 23,
      bounds.right - 8,
      bounds.bottom - 4};
  draw_text(
      dc,
      value,
      value_bounds,
      state.small_font,
      kInk,
      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void draw_strands(
    HDC dc,
    const int left,
    const int top,
    const int width) {
  constexpr std::array<COLORREF, 3> kColors{
      kTsumugiBlue,
      kTsumugiPurple,
      RGB(82, 176, 132)};
  for (std::size_t index = 0; index < kColors.size(); ++index) {
    const HPEN pen = CreatePen(
        PS_SOLID,
        2,
        kColors[index]);
    const HGDIOBJ old_pen = SelectObject(dc, pen);
    const int offset = static_cast<int>(index) * 7;
    POINT points[4]{
        {left, top + offset},
        {left + width / 3, top + 18 - offset},
        {left + width * 2 / 3, top - 8 + offset},
        {left + width, top + 8 + offset},
    };
    PolyBezier(dc, points, 4);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
  }
}

RECT content_rect(const AppState& state) {
  RECT client{};
  GetClientRect(state.window, &client);
  return RECT{
      260L,
      94L,
      (std::max)(client.right - 28L, 800L),
      client.bottom - 24L};
}

void draw_stepper(
    const AppState& state,
    HDC dc,
    const std::array<std::wstring_view, 4>& labels,
    const int active_step) {
  const RECT content = content_rect(state);
  const int left = content.left + 16;
  const int right = content.right - 16;
  const int y = 120;
  const int step_width = (right - left) / 4;
  const HPEN line_pen = CreatePen(PS_SOLID, 2, kBorder);
  const HGDIOBJ old_pen = SelectObject(dc, line_pen);
  MoveToEx(dc, left + step_width / 2, y, nullptr);
  LineTo(dc, right - step_width / 2, y);
  SelectObject(dc, old_pen);
  DeleteObject(line_pen);

  for (int index = 0; index < 4; ++index) {
    const int center = left + step_width * index + step_width / 2;
    const bool reached = index + 1 <= active_step;
    const HBRUSH brush = CreateSolidBrush(reached ? kTsumugiBlue : kCard);
    const HPEN pen = CreatePen(PS_SOLID, 2, reached ? kTsumugiBlue : kBorder);
    const HGDIOBJ old_brush = SelectObject(dc, brush);
    const HGDIOBJ step_old_pen = SelectObject(dc, pen);
    Ellipse(dc, center - 11, y - 11, center + 11, y + 11);
    SelectObject(dc, step_old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);

    RECT number_bounds{center - 10, y - 9, center + 11, y + 10};
    draw_text(
        dc,
        std::to_wstring(index + 1),
        number_bounds,
        state.small_font,
        reached ? RGB(255, 255, 255) : kMuted,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT label_bounds{
        center - step_width / 2,
        y + 17,
        center + step_width / 2,
        y + 40};
    draw_text(
        dc,
        labels[static_cast<std::size_t>(index)],
        label_bounds,
        state.small_font,
        reached ? kInk : kMuted,
        DT_CENTER | DT_TOP | DT_SINGLELINE);
  }
}

void show_page_controls(AppState& state) {
  const bool job = state.page == Page::job;
  const bool repair = state.page == Page::boot_repair;
  const bool disk = state.page == Page::disk_diagnostics;

  for (const HWND control :
       {state.job_path, state.job_browse, state.job_check, state.job_output}) {
    set_control_visible(control, job);
  }
  const bool show_job_confirmation =
      job && state.job_execution_ready && !state.job_progress_active;
  for (const HWND control :
       {state.job_acknowledge, state.job_token, state.job_execute}) {
    set_control_visible(control, show_job_confirmation);
  }
  set_control_visible(
      state.job_cancel, job && state.job_progress_active);
  for (const HWND control :
       {state.repair_disk,
        state.windows_root,
        state.system_root,
        state.firmware,
        state.winre_diagnostic,
        state.repair_inspect,
        state.repair_acknowledge,
        state.repair_token,
        state.repair_execute,
        state.repair_output}) {
    set_control_visible(control, repair);
  }
  for (const HWND control :
       {state.refresh, state.disk_list, state.disk_details}) {
    set_control_visible(control, disk);
  }
}

void layout_controls(AppState& state) {
  RECT client{};
  GetClientRect(state.window, &client);
  const RECT content = content_rect(state);
  const int width = content.right - content.left;
  const int bottom = content.bottom;
  const auto repair_actions =
      ytec::winpeapp::build_winpe_repair_action_layout(client.right);

  SetWindowPos(
      state.nav_job,
      nullptr,
      16,
      kSidebarNavTop,
      198,
      kSidebarNavHeight,
      SWP_NOZORDER);
  SetWindowPos(
      state.nav_repair,
      nullptr,
      16,
      kSidebarNavTop + kSidebarNavPitch,
      198,
      kSidebarNavHeight,
      SWP_NOZORDER);
  SetWindowPos(
      state.nav_disk,
      nullptr,
      16,
      kSidebarNavTop + 2 * kSidebarNavPitch,
      198,
      kSidebarNavHeight,
      SWP_NOZORDER);

  SetWindowPos(
      state.job_path,
      nullptr,
      content.left + 22,
      kJobPathControlTop,
      (std::max)(width - 340, 300),
      30,
      SWP_NOZORDER);
  SetWindowPos(
      state.job_browse,
      nullptr,
      content.right - 296,
      kJobPathControlTop,
      112,
      32,
      SWP_NOZORDER);
  SetWindowPos(
      state.job_check,
      nullptr,
      content.right - 174,
      kJobPathControlTop,
      142,
      32,
      SWP_NOZORDER);
  SetWindowPos(
      state.job_acknowledge,
      nullptr,
      content.left + 22,
      340,
      width - 44,
      24,
      SWP_NOZORDER);
  SetWindowPos(
      state.job_token,
      nullptr,
      content.left + 22,
      375,
      (std::max)(width - 246, 300),
      30,
      SWP_NOZORDER);
  SetWindowPos(
      state.job_execute,
      nullptr,
      content.right - 196,
      374,
      164,
      32,
      SWP_NOZORDER);
  SetWindowPos(
      state.job_cancel,
      nullptr,
      content.right - 176,
      420,
      144,
      32,
      SWP_NOZORDER);
  SetWindowPos(
      state.job_output,
      nullptr,
      content.left + 22,
      514,
      width - 44,
      (std::max)(bottom - 536, 84),
      SWP_NOZORDER);

  SetWindowPos(
      state.refresh,
      nullptr,
      content.right - 144,
      108,
      124,
      32,
      SWP_NOZORDER);
  const int list_width = (std::max)(330, width * 38 / 100);
  SetWindowPos(
      state.disk_list,
      nullptr,
      content.left + 16,
      164,
      list_width,
      (std::max)(bottom - 180, 300),
      SWP_NOZORDER);
  SetWindowPos(
      state.disk_details,
      nullptr,
      content.left + 30 + list_width,
      164,
      width - list_width - 46,
      (std::max)(bottom - 180, 300),
      SWP_NOZORDER);

  const int field_top = 199;
  const int field_gap = 14;
  const int field_width = (width - 44 - field_gap * 3) / 4;
  SetWindowPos(
      state.repair_disk,
      nullptr,
      content.left + 22,
      field_top,
      field_width,
      300,
      SWP_NOZORDER);
  SetWindowPos(
      state.windows_root,
      nullptr,
      content.left + 22 + field_width + field_gap,
      field_top,
      field_width,
      300,
      SWP_NOZORDER);
  SetWindowPos(
      state.system_root,
      nullptr,
      content.left + 22 + (field_width + field_gap) * 2,
      field_top,
      field_width,
      300,
      SWP_NOZORDER);
  SetWindowPos(
      state.firmware,
      nullptr,
      content.left + 22 + (field_width + field_gap) * 3,
      field_top,
      field_width,
      300,
      SWP_NOZORDER);
  SetWindowPos(
      state.winre_diagnostic,
      nullptr,
      repair_actions.winre_diagnostic_button.left,
      repair_actions.winre_diagnostic_button.top,
      repair_actions.winre_diagnostic_button.width(),
      repair_actions.winre_diagnostic_button.height(),
      SWP_NOZORDER);
  SetWindowPos(
      state.repair_inspect,
      nullptr,
      repair_actions.boot_inspect_button.left,
      repair_actions.boot_inspect_button.top,
      repair_actions.boot_inspect_button.width(),
      repair_actions.boot_inspect_button.height(),
      SWP_NOZORDER);
  SetWindowPos(
      state.repair_acknowledge,
      nullptr,
      content.left + 22,
      kRepairAcknowledgeTop,
      width - 44,
      kRepairAcknowledgeHeight,
      SWP_NOZORDER);
  SetWindowPos(
      state.repair_token,
      nullptr,
      content.left + 22,
      kRepairTokenControlTop,
      width - 250,
      kRepairTokenControlHeight,
      SWP_NOZORDER);
  SetWindowPos(
      state.repair_execute,
      nullptr,
      content.right - 212,
      kRepairTokenControlTop,
      180,
      32,
      SWP_NOZORDER);
  SetWindowPos(
      state.repair_output,
      nullptr,
      content.left + 22,
      kRepairOutputTop,
      width - 44,
      (std::max)(bottom - (kRepairOutputTop + 22), 110),
      SWP_NOZORDER);

  InvalidateRect(state.window, nullptr, TRUE);
  static_cast<void>(client);
}

void update_disk_details(AppState& state) {
  const LRESULT selected =
      SendMessageW(state.disk_list, LB_GETCURSEL, 0, 0);
  if (selected == LB_ERR ||
      static_cast<std::size_t>(selected) >= state.dashboard.disks.size()) {
    SetWindowTextW(
        state.disk_details,
        L"ディスクを選ぶと、読み取り専用の詳細を表示します。");
    return;
  }
  SetWindowTextW(
      state.disk_details,
      state.dashboard.disks[static_cast<std::size_t>(selected)]
          .details.c_str());
}

void invalidate_boot_review(AppState& state) {
  state.inspected_boot_request.reset();
  state.inspected_boot_selection.reset();
  state.boot_confirmation_token.clear();
  state.repair_step = 1;
  SendMessageW(
      state.repair_acknowledge,
      BM_SETCHECK,
      BST_UNCHECKED,
      0);
  SetWindowTextW(state.repair_token, L"");
  SetWindowTextW(
      state.repair_output,
      L"対象を選び、「読み取り専用で確認」を押してください。"
      L"\r\nこの段階ではBCDやディスクを変更しません。");
  InvalidateRect(state.window, nullptr, FALSE);
}

std::optional<std::uint32_t> selected_disk_number(const HWND combo) {
  const LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
  if (selected == CB_ERR) {
    return std::nullopt;
  }
  const LRESULT data =
      SendMessageW(combo, CB_GETITEMDATA, selected, 0);
  if (data == CB_ERR || data < 0) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(data);
}

ytec::bootrepair::BcdBootFirmware selected_firmware(
    const AppState& state) {
  const LRESULT selected =
      SendMessageW(state.firmware, CB_GETCURSEL, 0, 0);
  return selected == 1
      ? ytec::bootrepair::BcdBootFirmware::bios
      : ytec::bootrepair::BcdBootFirmware::uefi;
}

bool is_drive_root(const std::wstring& root) {
  return root.size() == 3 && std::iswalpha(root[0]) != 0 &&
         root[1] == L':' && root[2] == L'\\';
}

std::wstring root_file_system(const std::wstring& root) {
  if (!is_drive_root(root)) {
    return {};
  }
  std::array<wchar_t, 64> file_system{};
  if (GetVolumeInformationW(
          root.c_str(),
          nullptr,
          0,
          nullptr,
          nullptr,
          nullptr,
          file_system.data(),
          static_cast<DWORD>(file_system.size())) == FALSE) {
    return {};
  }
  return file_system.data();
}

bool root_contains_windows(const std::wstring& root) {
  const std::wstring system_hive =
      root + L"Windows\\System32\\Config\\SYSTEM";
  const DWORD attributes = GetFileAttributesW(system_hive.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & (FILE_ATTRIBUTE_DIRECTORY |
                        FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

std::vector<std::wstring> enumerate_local_roots() {
  std::vector<std::wstring> roots;
  const DWORD mask = GetLogicalDrives();
  for (wchar_t letter = L'C'; letter <= L'Z'; ++letter) {
    if (letter == L'X') {
      continue;
    }
    const DWORD bit = 1UL << static_cast<unsigned int>(letter - L'A');
    if ((mask & bit) == 0) {
      continue;
    }
    std::wstring root{letter, L':', L'\\'};
    const UINT type = GetDriveTypeW(root.c_str());
    if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE ||
        type == DRIVE_RAMDISK) {
      roots.push_back(std::move(root));
    }
  }
  return roots;
}

void select_combo_text(const HWND combo, const std::wstring& text) {
  const LRESULT index =
      SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                   reinterpret_cast<LPARAM>(text.c_str()));
  if (index != CB_ERR) {
    SendMessageW(combo, CB_SETCURSEL, index, 0);
  } else {
    SetWindowTextW(combo, text.c_str());
  }
}

void select_default_roots(AppState& state) {
  std::wstring windows_root;
  for (const auto& root : state.roots) {
    if (root_contains_windows(root)) {
      windows_root = root;
      break;
    }
  }
  if (windows_root.empty() && !state.roots.empty()) {
    windows_root = state.roots.front();
  }
  if (!windows_root.empty()) {
    select_combo_text(state.windows_root, windows_root);
  }

  const bool uefi =
      selected_firmware(state) == ytec::bootrepair::BcdBootFirmware::uefi;
  std::wstring system_root;
  for (const auto& root : state.roots) {
    const std::wstring file_system = root_file_system(root);
    if ((uefi && _wcsicmp(file_system.c_str(), L"FAT32") == 0 &&
         _wcsicmp(root.c_str(), windows_root.c_str()) != 0) ||
        (!uefi && _wcsicmp(file_system.c_str(), L"NTFS") == 0)) {
      system_root = root;
      break;
    }
  }
  if (system_root.empty()) {
    system_root = kAutoSystemPartitionLabel;
  }
  if (!system_root.empty()) {
    select_combo_text(state.system_root, system_root);
  }
}

void populate_root_controls(AppState& state) {
  state.roots = enumerate_local_roots();
  SendMessageW(state.windows_root, CB_RESETCONTENT, 0, 0);
  SendMessageW(state.system_root, CB_RESETCONTENT, 0, 0);
  SendMessageW(
      state.system_root,
      CB_ADDSTRING,
      0,
      reinterpret_cast<LPARAM>(kAutoSystemPartitionLabel));
  for (const auto& root : state.roots) {
    SendMessageW(
        state.windows_root,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(root.c_str()));
    SendMessageW(
        state.system_root,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(root.c_str()));
  }
  select_default_roots(state);
}

void populate_disk_controls(AppState& state) {
  SendMessageW(state.disk_list, LB_RESETCONTENT, 0, 0);
  SendMessageW(state.repair_disk, CB_RESETCONTENT, 0, 0);
  for (const auto& disk : state.dashboard.disks) {
    SendMessageW(
        state.disk_list,
        LB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(disk.list_label.c_str()));
    if (disk.selectable_as_target) {
      const LRESULT index = SendMessageW(
          state.repair_disk,
          CB_ADDSTRING,
          0,
          reinterpret_cast<LPARAM>(disk.title.c_str()));
      if (index != CB_ERR && index != CB_ERRSPACE) {
        SendMessageW(
            state.repair_disk,
            CB_SETITEMDATA,
            static_cast<WPARAM>(index),
            static_cast<LPARAM>(disk.disk_number));
      }
    }
  }
  if (!state.dashboard.disks.empty()) {
    SendMessageW(state.disk_list, LB_SETCURSEL, 0, 0);
  }
  if (SendMessageW(state.repair_disk, CB_GETCOUNT, 0, 0) > 0) {
    SendMessageW(state.repair_disk, CB_SETCURSEL, 0, 0);
  }
  update_disk_details(state);
}

BrowseResult browse_for_job(HWND owner, std::wstring& selected_path) {
  std::vector<wchar_t> path(32768U, L'\0');
  if (!selected_path.empty() && selected_path.size() < path.size()) {
    std::copy(selected_path.begin(), selected_path.end(), path.begin());
  }

  std::array<wchar_t, MAX_PATH> system_directory{};
  const UINT system_length = GetSystemDirectoryW(
      system_directory.data(),
      static_cast<UINT>(system_directory.size()));
  if (system_length == 0 ||
      system_length >= system_directory.size() - 16U) {
    return BrowseResult::unavailable;
  }
  std::wstring library_path(
      system_directory.data(),
      static_cast<std::size_t>(system_length));
  library_path += L"\\comdlg32.dll";
  const HMODULE module =
      LoadLibraryExW(library_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (module == nullptr) {
    return BrowseResult::unavailable;
  }
  using GetOpenFileNameFunction = BOOL(WINAPI*)(LPOPENFILENAMEW);
  using CommDlgExtendedErrorFunction = DWORD(WINAPI*)();
  const auto function = reinterpret_cast<GetOpenFileNameFunction>(
      GetProcAddress(module, "GetOpenFileNameW"));
  const auto extended_error =
      reinterpret_cast<CommDlgExtendedErrorFunction>(
          GetProcAddress(module, "CommDlgExtendedError"));
  if (function == nullptr) {
    FreeLibrary(module);
    return BrowseResult::unavailable;
  }

  constexpr wchar_t kFilter[] =
      L"Tsumugi予約ジョブ (*.json)\0*.json\0すべてのファイル (*.*)\0*.*\0\0";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  dialog.lpstrFilter = kFilter;
  dialog.lpstrFile = path.data();
  dialog.nMaxFile = static_cast<DWORD>(path.size());
  dialog.lpstrTitle = L"Windowsで作成した予約ジョブを選択";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
      OFN_DONTADDTORECENT | OFN_EXPLORER;
  const BOOL accepted = function(&dialog);
  const DWORD dialog_error =
      accepted == FALSE && extended_error != nullptr ? extended_error() : 0U;
  FreeLibrary(module);
  if (accepted == FALSE) {
    if (extended_error == nullptr) {
      return BrowseResult::unavailable;
    }
    return dialog_error == 0U
        ? BrowseResult::cancelled
        : BrowseResult::unavailable;
  }
  selected_path = path.data();
  return BrowseResult::selected;
}

void update_action_state(AppState& state) {
  const bool inventory_ready =
      state.inventory.has_value() &&
      state.dashboard.readiness == ytec::winpeapp::DashboardReadiness::ready;
  const bool idle = !state.operation_busy && !state.inventory_busy &&
      !state.job_discovery_busy;
  const bool job_has_path = !control_text(state.job_path).empty();
  set_control_enabled(
      state.job_check,
      idle && inventory_ready && state.dashboard.job_review_available &&
          job_has_path);
  set_control_enabled(state.job_path, idle);
  set_control_enabled(state.job_browse, idle);
  const bool job_acknowledged =
      SendMessageW(state.job_acknowledge, BM_GETCHECK, 0, 0) ==
      BST_CHECKED;
  const bool job_token_matches =
      !state.job_confirmation_token.empty() &&
      control_text(state.job_token) == state.job_confirmation_token;
  set_control_enabled(
      state.job_acknowledge,
      idle && state.job_execution_ready);
  set_control_enabled(
      state.job_token,
      idle && state.job_execution_ready);
  set_control_enabled(
      state.job_execute,
      idle && state.job_execution_ready && job_acknowledged &&
          job_token_matches);
  const bool cancellation_already_requested =
      state.job_cancellation != nullptr &&
      state.job_cancellation->load(std::memory_order_relaxed);
  set_control_enabled(
      state.job_cancel,
      state.operation_busy && state.job_progress_active &&
          state.job_progress.cancellation_allowed &&
          !cancellation_already_requested);
  set_control_enabled(
      state.refresh,
      !state.inventory_busy && !state.job_discovery_busy);
  set_control_enabled(state.repair_disk, idle);
  set_control_enabled(state.windows_root, idle);
  set_control_enabled(state.system_root, idle);
  set_control_enabled(state.firmware, idle);
  set_control_enabled(
      state.winre_diagnostic,
      idle && inventory_ready &&
          selected_disk_number(state.repair_disk).has_value() &&
          is_drive_root(control_text(state.windows_root)));
  set_control_enabled(
      state.repair_inspect,
      idle && inventory_ready &&
          state.dashboard.boot_repair_review_available &&
          selected_disk_number(state.repair_disk).has_value() &&
          is_drive_root(control_text(state.windows_root)) &&
          (is_drive_root(control_text(state.system_root)) ||
           control_text(state.system_root) == kAutoSystemPartitionLabel));

  const bool acknowledged =
      SendMessageW(state.repair_acknowledge, BM_GETCHECK, 0, 0) ==
      BST_CHECKED;
  const bool token_matches =
      !state.boot_confirmation_token.empty() &&
      control_text(state.repair_token) == state.boot_confirmation_token;
  const bool reviewed = state.inspected_boot_request.has_value() &&
      state.inspected_boot_selection.has_value();
  set_control_enabled(
      state.repair_acknowledge,
      idle && reviewed);
  set_control_enabled(
      state.repair_token,
      idle && reviewed);
  set_control_enabled(
      state.repair_execute,
      idle && reviewed && acknowledged && token_matches);
}

void invalidate_job_review(
    AppState& state,
    const std::wstring_view message) {
  state.job_execution_ready = false;
  state.job_execution_kind = JobExecutionKind::none;
  state.job_execution_mode =
      ytec::imageformat::JobExecutionMode::review_required;
  state.job_progress_active = false;
  state.job_confirmation_token.clear();
  state.job_cancellation.reset();
  state.job_elapsed = std::chrono::milliseconds{};
  state.job_progress = ytec::clonecore::DiskOperationProgress{};
  state.job_payload_hash.reset();
  SendMessageW(
      state.job_acknowledge,
      BM_SETCHECK,
      BST_UNCHECKED,
      0);
  SetWindowTextW(state.job_token, L"");
  SetWindowTextW(state.job_cancel, L"安全に取消");
  if (!message.empty()) {
    SetWindowTextW(state.job_output, std::wstring(message).c_str());
  }
  show_page_controls(state);
}

void start_inventory(AppState& state) {
  if (state.inventory_busy || state.job_discovery_busy ||
      state.operation_busy) {
    return;
  }
  state.job_step = 1;
  invalidate_job_review(
      state,
      L"ディスク情報が更新されました。予約ジョブをもう一度確認してください。"
      L"\r\nこの画面だけではクローン／復元を開始しません。");
  invalidate_boot_review(state);
  state.inventory_busy = true;
  state.inventory.reset();
  state.dashboard = ytec::winpeapp::DashboardView{
      .readiness = ytec::winpeapp::DashboardReadiness::scanning,
      .headline = L"物理ディスクを確認しています",
      .guidance = L"読み取り専用で情報を集めています。しばらくお待ちください。"};
  SetWindowTextW(
      state.disk_details,
      L"ディスク情報を読み取り専用で確認しています…");
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);
  const HWND window = state.window;
  std::thread([window]() {
    auto payload = std::make_unique<InventoryPayload>();
    auto provider =
        ytec::diskmodel::make_windows_disk_inventory_provider();
    auto result = provider->enumerate();
    if (result) {
      payload->inventory = result.take_value();
    } else {
      payload->error = format_error(result.error());
    }
    post_payload(window, kInventoryCompleteMessage, std::move(payload));
  }).detach();
}

void start_job_discovery(AppState& state) {
  if (state.inventory_busy || state.job_discovery_busy ||
      state.operation_busy || !control_text(state.job_path).empty() ||
      !state.inventory.has_value() ||
      state.dashboard.readiness !=
          ytec::winpeapp::DashboardReadiness::ready ||
      !state.dashboard.job_review_available) {
    return;
  }

  state.job_discovery_busy = true;
  SetWindowTextW(
      state.job_output,
      L"固定候補名の予約ジョブを読み取り専用で探しています。\r\n"
      L"検索はローカルドライブ直下と Tsumugi フォルダー直下だけです。\r\n"
      L"この確認ではクローン／復元を開始しません。");
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);

  const HWND window = state.window;
  std::thread([window]() {
    auto payload = std::make_unique<JobDiscoveryPayload>();
    auto candidates =
        ytec::winpeapp::make_windows_job_manifest_candidate_provider();
    auto loader = ytec::winpeapp::make_windows_job_manifest_loader();
    auto discovered = ytec::winpeapp::discover_unique_job_manifest(
        *candidates, *loader);
    if (!discovered) {
      payload->error = format_error(discovered.error());
    } else if (discovered.value().has_value()) {
      payload->path = discovered.value()->path;
    }
    post_payload(
        window, kJobDiscoveryCompleteMessage, std::move(payload));
  }).detach();
}

void start_job_check(AppState& state) {
  const std::wstring path = control_text(state.job_path);
  if (state.operation_busy || path.empty()) {
    return;
  }
  invalidate_job_review(state, L"");
  state.operation_busy = true;
  state.job_step = 2;
  SetWindowTextW(
      state.job_output,
      L"1. ジョブの改ざん検査\r\n"
      L"2. 復元イメージの完全検証（復元ジョブのみ）\r\n"
      L"3. 物理ディスクの再識別\r\n\r\n"
      L"確認中です。ディスクへの書き込みは行っていません。");
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);
  const HWND window = state.window;
  std::thread([window, path]() {
    auto payload = std::make_unique<JobCheckPayload>();
    auto loader = ytec::winpeapp::make_windows_job_manifest_loader();
    auto initial_bytes = loader->load(path);
    JobExecutionKind kind = JobExecutionKind::none;
    std::optional<ytec::clonecore::StableDiskIdentity> expected_target;
    if (initial_bytes) {
      auto verified =
          ytec::imageformat::parse_and_verify_hashed_job_manifest(
              initial_bytes.value());
      if (verified) {
        payload->job_payload_hash = verified.value().payload_hash;
        payload->execution_mode =
            verified.value().manifest.execution_mode;
        if (verified.value().manifest.job_type ==
            ytec::imageformat::JobType::clone) {
          kind = JobExecutionKind::clone;
        } else if (verified.value().manifest.job_type ==
                   ytec::imageformat::JobType::mbr_to_gpt) {
          kind = JobExecutionKind::mbr_to_gpt;
        } else if (verified.value().manifest.job_type ==
                   ytec::imageformat::JobType::restore_image) {
          kind = JobExecutionKind::restore;
        }
        payload->kind = kind;
        if (kind != JobExecutionKind::none &&
            verified.value().manifest.target.has_value()) {
          expected_target = *verified.value().manifest.target;
        }
      }
    }

    auto provider =
        ytec::diskmodel::make_windows_disk_inventory_provider();
    auto verifier =
        ytec::winpeapp::make_windows_restore_image_verifier();
    auto safety_probe =
        ytec::winpeapp::make_windows_restore_execution_safety_probe();
    auto candidates =
        ytec::winpeapp::make_windows_restore_image_candidate_provider();
    std::vector<std::wstring> arguments{
        L"--job-preflight", L"--job-path", path};
    if (kind == JobExecutionKind::restore) {
      arguments.push_back(L"--search-image");
    }
    std::ostringstream output;
    std::ostringstream errors;
    const int exit_code = ytec::winpeapp::run_winpe_app(
        arguments,
        *provider,
        output,
        errors,
        nullptr,
        nullptr,
        loader.get(),
        verifier.get(),
        safety_probe.get(),
        candidates.get(),
        nullptr,
        nullptr);
    payload->success = exit_code == 0;
    const std::string rendered =
        payload->success ? output.str() : errors.str();
    if (payload->success && kind != JobExecutionKind::none &&
        expected_target.has_value()) {
      auto fresh_inventory = provider->enumerate();
      if (fresh_inventory && fresh_inventory.value().issues.empty()) {
        for (const auto& disk : fresh_inventory.value().disks) {
          auto observed = ytec::diskmodel::make_stable_disk_identity(
              disk, disk.is_system_disk);
          if (!observed ||
              !ytec::clonecore::validate_stable_identity(
                  *expected_target,
                  observed.value(), job_target_label(kind))) {
            continue;
          }
          if (!payload->confirmation_token.empty()) {
            payload->confirmation_token.clear();
            break;
          }
          payload->confirmation_token =
              ytec::clonecore::make_target_confirmation_token(
                  observed.value());
        }
      }
    }
    payload->execution_ready =
        payload->success && kind != JobExecutionKind::none &&
        !payload->confirmation_token.empty() &&
        (is_copy_job(kind) ||
         required_restore_checks_passed(rendered));
    payload->output = utf8_to_wide(rendered);
    if (payload->output.empty()) {
      payload->output = payload->success
          ? L"安全確認が完了しました。"
          : L"安全確認に失敗しました。";
    }
    post_payload(window, kJobCheckCompleteMessage, std::move(payload));
  }).detach();
}

void start_job_execute(AppState& state, const bool automatic = false) {
  const std::wstring path = control_text(state.job_path);
  const std::wstring confirmation = automatic
      ? state.job_confirmation_token
      : control_text(state.job_token);
  const JobExecutionKind kind = state.job_execution_kind;
  const auto job_payload_hash = state.job_payload_hash;
  const bool acknowledged = automatic ||
      SendMessageW(state.job_acknowledge, BM_GETCHECK, 0, 0) ==
          BST_CHECKED;
  if (state.operation_busy || !state.job_execution_ready || path.empty() ||
      kind == JobExecutionKind::none || !acknowledged ||
      confirmation != state.job_confirmation_token ||
      !job_payload_hash.has_value() ||
      (automatic &&
       state.job_execution_mode !=
           ytec::imageformat::JobExecutionMode::auto_once)) {
    return;
  }

  if (automatic) {
    const auto claimed =
        ytec::imageformat::claim_job_auto_execution_once(
            path, *job_payload_hash);
    if (!claimed) {
      state.job_execution_mode =
          ytec::imageformat::JobExecutionMode::review_required;
      std::wstring text = control_text(state.job_output);
      text +=
          L"\r\n\r\n一回限り自動実行は開始しませんでした。\r\n" +
          format_error(claimed.error()) +
          L"\r\n対象ディスクは変更していません。必要なら内容を確認し、"
          L"確認語を入力して手動で開始してください。";
      SetWindowTextW(state.job_output, text.c_str());
      update_action_state(state);
      InvalidateRect(state.window, nullptr, FALSE);
      return;
    }
    SendMessageW(
        state.job_acknowledge, BM_SETCHECK, BST_CHECKED, 0);
    SetWindowTextW(state.job_token, confirmation.c_str());
  }

  state.operation_busy = true;
  state.job_execution_ready = false;
  state.job_progress_active = true;
  state.job_step = 4;
  state.job_elapsed = std::chrono::milliseconds{};
  state.job_progress = ytec::clonecore::DiskOperationProgress{
      .stage = ytec::clonecore::DiskOperationStage::planning,
      .cancellation_allowed = true,
  };
  state.job_cancellation = std::make_shared<std::atomic_bool>(false);
  const auto cancellation = state.job_cancellation;
  const bool copy_job = is_copy_job(kind);
  const bool migration_job = kind == JobExecutionKind::mbr_to_gpt;
  std::wstring preparing_message;
  if (copy_job) {
    preparing_message = migration_job
        ? L"実行直前にMBRコピー元、空のコピー先、変換条件をすべて再確認しています。"
          L"\r\nMBRクローン後はMicrosoft MBR2GPT、BCDBoot、最終検証まで連続実行します。"
        : L"実行直前にジョブ、コピー元、コピー先、安全条件をすべて再確認しています。";
    preparing_message +=
        L"\r\nこの読み取り専用再確認中に取消した場合、コピー先は変更しません。"
        L"\r\n書込み開始後の取消・失敗では、安全のためコピー先を"
        L"オフラインのまま保護する場合があります。";
  } else {
    preparing_message =
        L"実行直前にジョブ、dcimg、復元先、安全条件をすべて再確認しています。"
        L"\r\nこの読み取り専用再確認中に取消した場合、復元先は変更しません。"
        L"\r\n書込み開始後の取消・失敗では、安全のため復元先を"
        L"オフラインのまま保護する場合があります。";
  }
  const std::wstring execution_message = automatic
      ? L"Windows側で明示許可された一回限り自動実行です。"
        L"\r\n実行開始記録を新規保存・読戻し確認しました。"
        L"同じジョブは自動で再実行しません。\r\n" +
            preparing_message
      : preparing_message;
  SetWindowTextW(state.job_output, execution_message.c_str());
  SetWindowTextW(state.job_cancel, L"安全に取消");
  show_page_controls(state);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);

  const HWND window = state.window;
  std::thread([
      window,
      path,
      confirmation,
      cancellation,
      kind,
      job_payload_hash]() {
    auto payload = std::make_unique<JobExecutePayload>();
    payload->kind = kind;
    auto provider =
        ytec::diskmodel::make_windows_disk_inventory_provider();
    auto loader = ytec::winpeapp::make_hash_locked_job_manifest_loader(
        ytec::winpeapp::make_windows_job_manifest_loader(),
        *job_payload_hash);
    auto verifier =
        ytec::winpeapp::make_windows_restore_image_verifier();
    auto safety_probe =
        ytec::winpeapp::make_windows_restore_execution_safety_probe();
    auto candidates =
        ytec::winpeapp::make_windows_restore_image_candidate_provider();
    auto restore_service =
        ytec::winpeapp::make_windows_restore_execution_service();
    auto clone_service =
        ytec::winpeapp::make_windows_clone_job_execution_service();
    auto mbr2gpt_service =
        ytec::winpeapp::make_windows_mbr2gpt_job_execution_service();

    const auto started = std::chrono::steady_clock::now();
    auto last_posted = std::make_shared<std::chrono::steady_clock::time_point>(
        started - std::chrono::seconds(1));
    auto last_stage =
        std::make_shared<ytec::clonecore::DiskOperationStage>(
            ytec::clonecore::DiskOperationStage::planning);
    auto last_cancellation_allowed = std::make_shared<bool>(true);
    auto received_progress = std::make_shared<bool>(false);
    ytec::clonecore::DiskOperationCallbacks callbacks{
        .progress =
            [window,
             started,
             last_posted,
             last_stage,
             last_cancellation_allowed,
             received_progress](
                const ytec::clonecore::DiskOperationProgress& progress) {
              const auto now = std::chrono::steady_clock::now();
              const bool state_changed =
                  !*received_progress ||
                  progress.stage != *last_stage ||
                  progress.cancellation_allowed !=
                      *last_cancellation_allowed;
              if (!state_changed &&
                  now - *last_posted < std::chrono::milliseconds(100)) {
                return;
              }
              *received_progress = true;
              *last_posted = now;
              *last_stage = progress.stage;
              *last_cancellation_allowed =
                  progress.cancellation_allowed;
              auto update = std::make_unique<JobProgressPayload>();
              update->progress = progress;
              update->elapsed =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - started);
              post_payload(
                  window, kJobProgressMessage, std::move(update));
            },
        .cancellation_requested =
            [cancellation]() {
              return cancellation->load(std::memory_order_relaxed);
            },
    };

    std::vector<std::wstring> arguments{
        L"--job-execute",
        L"--job-path",
        path,
        L"--acknowledge-target-erasure",
        L"--confirmation",
        confirmation,
    };
    if (kind == JobExecutionKind::restore) {
      arguments.insert(arguments.begin() + 3, L"--search-image");
    }
    std::ostringstream output;
    std::ostringstream errors;
    const int exit_code = ytec::winpeapp::run_winpe_app(
        arguments,
        *provider,
        output,
        errors,
        nullptr,
        nullptr,
        loader.get(),
        verifier.get(),
        safety_probe.get(),
        candidates.get(),
        &callbacks,
        restore_service.get(),
        clone_service.get(),
        nullptr,
        mbr2gpt_service.get());
    payload->success = exit_code == 0;
    std::string rendered = payload->success ? output.str() : errors.str();
    if (rendered.empty()) {
      rendered = payload->success
          ? (kind == JobExecutionKind::mbr_to_gpt
                 ? "MBRからGPTへの移行が完了しました。\n"
                 : kind == JobExecutionKind::clone
                 ? "クローンが完了しました。\n"
                 : "復元が完了しました。\n")
          : (kind == JobExecutionKind::mbr_to_gpt
                 ? "MBRからGPTへの移行を安全に完了できませんでした。\n"
                 : kind == JobExecutionKind::clone
                 ? "クローンを安全に完了できませんでした。\n"
                 : "復元を安全に完了できませんでした。\n");
    }
    payload->output = utf8_to_wide(rendered);

    SYSTEMTIME completed{};
    GetSystemTime(&completed);
    std::array<char, 21> completed_utc{};
    std::array<wchar_t, 17> compact_utc{};
    const int completed_length = sprintf_s(
        completed_utc.data(),
        completed_utc.size(),
        "%04u-%02u-%02uT%02u:%02u:%02uZ",
        completed.wYear,
        completed.wMonth,
        completed.wDay,
        completed.wHour,
        completed.wMinute,
        completed.wSecond);
    const int compact_length = swprintf_s(
        compact_utc.data(),
        compact_utc.size(),
        L"%04u%02u%02u-%02u%02u%02uZ",
        completed.wYear,
        completed.wMonth,
        completed.wDay,
        completed.wHour,
        completed.wMinute,
        completed.wSecond);
    if (completed_length == 20 && compact_length == 16) {
      auto bytes = ytec::winpeapp::serialize_job_result_log(
          ytec::winpeapp::JobResultRecord{
              .job_payload_hash = *job_payload_hash,
              .job_type = kind == JobExecutionKind::mbr_to_gpt
                  ? ytec::imageformat::JobType::mbr_to_gpt
                  : kind == JobExecutionKind::clone
                  ? ytec::imageformat::JobType::clone
                  : ytec::imageformat::JobType::restore_image,
              .outcome = payload->success
                  ? ytec::winpeapp::JobResultOutcome::passed
                  : ytec::winpeapp::JobResultOutcome::failed,
              .completed_utc = completed_utc.data(),
              .app_version = YTEC_PROJECT_VERSION,
              .details_utf8 = rendered,
          });
      auto result_path = ytec::winpeapp::build_job_result_log_path(
          path, compact_utc.data());
      if (bytes && result_path) {
        const auto saved = ytec::winpeapp::write_new_job_result_log(
            result_path.value(), bytes.value());
        if (saved) {
          payload->result_log_path = result_path.take_value();
        } else {
          payload->result_log_error = format_error(saved.error());
        }
      } else {
        payload->result_log_error = format_error(
            bytes ? result_path.error() : bytes.error());
      }
    } else {
      payload->result_log_error =
          L"WinPE実行結果ログ用のUTC時刻を生成できませんでした。";
    }
    post_payload(
        window, kJobExecuteCompleteMessage, std::move(payload));
  }).detach();
}

void request_job_cancellation(AppState& state) {
  if (!state.job_progress_active ||
      !state.job_progress.cancellation_allowed ||
      state.job_cancellation == nullptr) {
    return;
  }
  state.job_cancellation->store(true, std::memory_order_relaxed);
  SetWindowTextW(state.job_cancel, L"取消要求済み");
  std::wstring output = control_text(state.job_output);
  output +=
      L"\r\n\r\n取消要求を受け付けました。安全な境界で停止します。"
      L"\r\n最終確定中は中断せず、整合性を守ってから終了します。";
  SetWindowTextW(state.job_output, output.c_str());
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);
}

std::optional<ytec::bootrepair::BootRepairTargetRequest>
current_boot_request(const AppState& state) {
  const auto disk_number = selected_disk_number(state.repair_disk);
  const std::wstring windows_root = control_text(state.windows_root);
  const std::wstring system_root = control_text(state.system_root);
  const bool auto_system_partition =
      system_root == kAutoSystemPartitionLabel;
  if (!disk_number.has_value() || !is_drive_root(windows_root) ||
      (!auto_system_partition && !is_drive_root(system_root))) {
    return std::nullopt;
  }
  return ytec::bootrepair::BootRepairTargetRequest{
      .disk_number = disk_number.value(),
      .windows_root = windows_root,
      .system_root = auto_system_partition ? L"" : system_root,
      .firmware = selected_firmware(state),
      .auto_mount_system_partition = auto_system_partition,
  };
}

void start_winre_diagnostic(AppState& state) {
  const auto disk_number = selected_disk_number(state.repair_disk);
  const std::wstring windows_root = control_text(state.windows_root);
  if (state.operation_busy || !disk_number.has_value() ||
      !is_drive_root(windows_root)) {
    return;
  }

  invalidate_boot_review(state);
  state.operation_busy = true;
  SetWindowTextW(
      state.repair_output,
      L"Microsoft署名済みREAgentCの /info だけを使い、WinREの登録先と"
      L"Winre.wimを読み取り確認しています。\r\n"
      L"BCD、回復設定、ディスクは変更しません。");
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);

  const HWND window = state.window;
  const std::wstring offline_windows_directory =
      windows_root + L"Windows";
  std::thread(
      [window,
       offline_windows_directory,
       disk_number = disk_number.value()]() {
        auto payload = std::make_unique<WinReDiagnosticPayload>();
        auto service =
            ytec::bootrepair::make_windows_winre_diagnostic_service();
        const auto result =
            service->inspect(offline_windows_directory, disk_number);
        if (result) {
          const auto view =
              ytec::winpeapp::build_winre_diagnostic_view(result.value());
          payload->output = view.headline + L"\r\n\r\n" + view.details;
        } else {
          payload->output = format_error(result.error()) +
              L"\r\n\r\n診断からMBR→GPT再構築の可否を推測せず、停止します。";
        }
        post_payload(
            window,
            kWinReDiagnosticCompleteMessage,
            std::move(payload));
      })
      .detach();
}

void start_boot_inspect(AppState& state) {
  const auto request = current_boot_request(state);
  if (state.operation_busy || !request.has_value()) {
    return;
  }
  state.operation_busy = true;
  state.repair_step = 2;
  state.inspected_boot_request.reset();
  state.inspected_boot_selection.reset();
  state.boot_confirmation_token.clear();
  SetWindowTextW(
      state.repair_output,
      request->auto_mount_system_partition
          ? L"Windows領域、未割り当てESP／Active、ディスク形式、"
            L"AMD64ブートローダーを\r\n読み取り専用で照合しています。"
            L"ドライブ文字やBCDは変更していません。"
          : L"Windows領域、システム領域、ディスク形式、"
            L"AMD64ブートローダーを\r\n読み取り専用で照合しています。"
            L"BCDは変更していません。");
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);
  const HWND window = state.window;
  std::thread([window, request = request.value()]() {
    auto payload = std::make_unique<BootInspectPayload>();
    auto provider =
        ytec::diskmodel::make_windows_disk_inventory_provider();
    auto service =
        ytec::bootrepair::make_windows_standalone_boot_repair_service(
            *provider);
    auto result = service->inspect(request);
    if (result) {
      payload->selection = result.take_value();
    } else {
      payload->error = format_error(result.error());
    }
    post_payload(window, kBootInspectCompleteMessage, std::move(payload));
  }).detach();
}

void start_boot_execute(AppState& state) {
  if (state.operation_busy ||
      !state.inspected_boot_request.has_value() ||
      !state.inspected_boot_selection.has_value()) {
    return;
  }
  if (SendMessageW(
          state.repair_acknowledge,
          BM_GETCHECK,
          0,
          0) != BST_CHECKED ||
      control_text(state.repair_token) !=
          state.boot_confirmation_token) {
    return;
  }
  const auto request = state.inspected_boot_request.value();
  const auto selection = state.inspected_boot_selection.value();
  const std::wstring token = state.boot_confirmation_token;
  state.operation_busy = true;
  state.repair_step = 3;
  SetWindowTextW(
      state.repair_output,
      request.auto_mount_system_partition
          ? L"実行直前に対象を再識別しています。\r\n"
            L"一致した未割り当て領域だけを一時割り当てし、"
            L"Microsoft署名済みBCDBootで再構築後、必ず解除します。"
          : L"実行直前に対象を再識別しています。\r\n"
            L"一致した場合だけ、Microsoft署名済みBCDBootで"
            L"起動ファイルを再構築します。");
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);
  const HWND window = state.window;
  std::thread([window, request, selection, token]() {
    auto payload = std::make_unique<BootExecutePayload>();
    auto provider =
        ytec::diskmodel::make_windows_disk_inventory_provider();
    auto service =
        ytec::bootrepair::make_windows_standalone_boot_repair_service(
            *provider);
    auto result = service->execute(
        ytec::bootrepair::StandaloneBootRepairExecutionRequest{
            .target = request,
            .expected = selection,
            .confirmation =
                ytec::clonecore::TargetConfirmation{
                    .first_step_acknowledged = true,
                    .typed_token = token,
                },
        });
    if (result) {
      payload->success =
          result.value().bcdboot.microsoft_signature_verified &&
          result.value().bcdboot.exit_code == 0 &&
          result.value().boot_store_verified;
      payload->output =
          L"起動修復が完了しました。\r\n"
          L"・Microsoft署名: 確認済み\r\n"
          L"・BCDBoot: 正常終了\r\n"
          L"・BCDストア: 再読込み確認済み\r\n" +
          std::wstring(
              result.value().system_partition_temporarily_mounted
                  ? L"・一時割り当て: 解除確認済み\r\n"
                  : L"・一時割り当て: 不要\r\n") +
          L"\r\n"
          L"USB/ISOを取り外し、修復したディスクから再起動してください。";
    } else {
      payload->output = format_error(result.error());
    }
    post_payload(window, kBootExecuteCompleteMessage, std::move(payload));
  }).detach();
}

void paint_sidebar(const AppState& state, HDC dc, const RECT& client) {
  RECT sidebar{0, 0, 230, client.bottom};
  SetDCBrushColor(dc, kSidebar);
  FillRect(dc, &sidebar, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));

  RECT brand{20, 24, 210, 62};
  draw_text(
      dc,
      L"Y-TEC",
      brand,
      state.heading_font,
      RGB(255, 255, 255),
      DT_LEFT | DT_TOP | DT_SINGLELINE);
  RECT name{20, 59, 214, 91};
  draw_text(
      dc,
      L"Tsumugi Drive",
      name,
      state.body_font,
      RGB(215, 231, 235),
      DT_LEFT | DT_TOP | DT_SINGLELINE);
  draw_strands(dc, 22, 112, 170);
  RECT concept_bounds{
      22, kSidebarConceptTop, 210, kSidebarConceptBottom};
  draw_text(
      dc,
      L"3つの工程を、ひとつに",
      concept_bounds,
      state.small_font,
      RGB(161, 179, 191),
      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
  RECT mode{20, kSidebarModeTop, 208, kSidebarModeTop + 52};
  draw_text(
      dc,
      L"起動環境モード\r\nネットワーク通信なし",
      mode,
      state.small_font,
      RGB(161, 179, 191),
      DT_LEFT | DT_TOP);
  RECT safety{20, client.bottom - 96, 208, client.bottom - 24};
  draw_text(
      dc,
      L"不明な対象は実行しません。\r\nコピー元は変更しません。",
      safety,
      state.small_font,
      RGB(161, 179, 191),
      DT_LEFT | DT_TOP);
}

void paint_header(const AppState& state, HDC dc, const RECT& client) {
  RECT header{230, 0, client.right, 88};
  FillRect(dc, &header, state.card_brush);
  const wchar_t* title = L"予約ジョブを確認";
  const wchar_t* subtitle =
      L"Windowsで準備したクローン・復元ジョブを安全に引き継ぎます";
  if (state.page == Page::boot_repair) {
    title = L"起動修復だけを行う";
    subtitle =
        L"クローンをせず、選択したWindowsのBCD起動構成だけを再構築します";
  } else if (state.page == Page::disk_diagnostics) {
    title = L"接続ディスクを確認";
    subtitle =
        L"モデル・容量・形式・パーティションを読み取り専用で表示します";
  }
  RECT title_bounds{260, 18, client.right - 28, 49};
  draw_text(
      dc,
      title,
      title_bounds,
      state.heading_font,
      kInk,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
  RECT subtitle_bounds{260, 51, client.right - 28, 78};
  draw_text(
      dc,
      subtitle,
      subtitle_bounds,
      state.small_font,
      kMuted,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
}

void paint_status_banner(const AppState& state, HDC dc) {
  const RECT content = content_rect(state);
  const COLORREF color =
      state.dashboard.readiness == ytec::winpeapp::DashboardReadiness::ready
      ? kSafeGreen
      : state.dashboard.readiness ==
                ytec::winpeapp::DashboardReadiness::scanning
      ? kTsumugiBlue
      : state.dashboard.readiness ==
                ytec::winpeapp::DashboardReadiness::warning
      ? kWarning
      : kDanger;
  RECT banner{
      content.left + 16,
      102,
      content.right - 16,
      150};
  fill_rounded_rect(dc, banner, RGB(255, 255, 255), color);
  RECT headline{banner.left + 16, banner.top + 7, banner.right - 12, banner.top + 27};
  draw_text(
      dc,
      state.dashboard.headline,
      headline,
      state.body_font,
      color,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
  RECT guidance{banner.left + 16, banner.top + 27, banner.right - 12, banner.bottom - 5};
  draw_text(
      dc,
      state.dashboard.guidance,
      guidance,
      state.small_font,
      kMuted,
      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void paint_job_page(const AppState& state, HDC dc) {
  const RECT content = content_rect(state);
  const std::wstring target_label(
      job_target_label(state.job_execution_kind));
  const std::wstring operation_label(
      job_operation_label(state.job_execution_kind));
  draw_stepper(
      state,
      dc,
      {L"ジョブ選択", L"完全検証", L"対象再確認", L"実行"},
      state.job_step);
  RECT path_card{content.left + 8, 168, content.right - 8, 260};
  fill_rounded_rect(dc, path_card, kCard, kBorder);
  RECT label{
      path_card.left + 16,
      kJobPathLabelTop,
      path_card.right - 16,
      kJobPathLabelBottom};
  draw_text(
      dc,
      L"予約ジョブ（.json）",
      label,
      state.small_font,
      kMuted,
      DT_LEFT | DT_TOP | DT_SINGLELINE);

  RECT action_card{content.left + 8, 276, content.right - 8, 430};
  action_card.bottom = 464;
  fill_rounded_rect(dc, action_card, kCard, kBorder);
  if (state.job_progress_active) {
    const auto progress = ytec::winpeapp::build_operation_progress_view(
        state.job_progress, state.job_elapsed);
    RECT stage{
        action_card.left + 16,
        action_card.top + 11,
        action_card.right - 164,
        action_card.top + 35};
    draw_text(
        dc,
        progress.stage_label,
        stage,
        state.body_font,
        progress.cancellation_allowed ? kTsumugiBlue : kWarning,
        DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT percentage{
        action_card.right - 156,
        action_card.top + 11,
        action_card.right - 16,
        action_card.top + 35};
    draw_text(
        dc,
        L"検証済み " + progress.percentage_label,
        percentage,
        state.body_font,
        kSafeGreen,
        DT_RIGHT | DT_TOP | DT_SINGLELINE);

    RECT partition{
        action_card.left + 16,
        action_card.top + 34,
        action_card.right - 16,
        action_card.top + 54};
    draw_text(
        dc,
        L"現在: " + progress.partition_label,
        partition,
        state.small_font,
        kMuted,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT track{
        action_card.left + 16,
        action_card.top + 58,
        action_card.right - 16,
        action_card.top + 74};
    fill_rounded_rect(dc, track, RGB(226, 233, 237), RGB(226, 233, 237));
    RECT fill = track;
    fill.right =
        fill.left + static_cast<int>(
                        static_cast<double>(fill.right - fill.left) *
                        progress.fraction);
    if (fill.right > fill.left) {
      fill_rounded_rect(dc, fill, kTsumugiBlue, kTsumugiBlue);
    }

    const int inner_left = action_card.left + 16;
    const int inner_right = action_card.right - 16;
    const int inner_width = inner_right - inner_left;
    const int eta_width = (std::max)(174, inner_width * 24 / 100);
    const int gap = 8;
    const int transfer_width = inner_width - eta_width - gap;
    const int metric_width = (transfer_width - gap * 2) / 3;
    const int metric_top = action_card.top + 83;
    const int metric_bottom = action_card.top + 132;
    const RECT read_tile{
        inner_left,
        metric_top,
        inner_left + metric_width,
        metric_bottom};
    const RECT write_tile{
        read_tile.right + gap,
        metric_top,
        read_tile.right + gap + metric_width,
        metric_bottom};
    const RECT verify_tile{
        write_tile.right + gap,
        metric_top,
        inner_left + transfer_width,
        metric_bottom};
    const RECT eta_tile{
        inner_left + transfer_width + gap,
        metric_top,
        inner_right,
        metric_bottom};
    draw_metric_tile(
        state,
        dc,
        read_tile,
        L"読込",
        progress.read_label,
        kTsumugiBlue,
        RGB(242, 249, 250));
    draw_metric_tile(
        state,
        dc,
        write_tile,
        L"書込",
        progress.write_label,
        kTsumugiPurple,
        RGB(247, 244, 251));
    draw_metric_tile(
        state,
        dc,
        verify_tile,
        L"検証",
        progress.verified_label,
        kSafeGreen,
        RGB(243, 250, 247));
    draw_metric_tile(
        state,
        dc,
        eta_tile,
        L"残り時間",
        progress.remaining_label,
        kWarning,
        RGB(253, 248, 240));

    RECT cancellation{
        action_card.left + 16,
        action_card.top + 141,
        action_card.right - 188,
        action_card.bottom - 8};
    draw_text(
        dc,
        L"速度 " + progress.speed_label +
            L"  •  経過 " + progress.elapsed_label +
            (progress.cancellation_allowed
                 ? L"  •  安全な境界で取消可能"
                 : L"  •  整合性保護のため取消不可"),
        cancellation,
        state.small_font,
        progress.cancellation_allowed ? kMuted : kWarning,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
  } else if (state.job_execution_ready) {
    RECT title{
        action_card.left + 16,
        action_card.top + 11,
        action_card.right - 16,
        action_card.top + 35};
    draw_text(
        dc,
        L"最終確認 — 表示された" + target_label +
            L"だけが完全消去されます",
        title,
        state.body_font,
        kDanger,
        DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT token{
        action_card.left + 16,
        action_card.top + 39,
        action_card.right - 16,
        action_card.top + 62};
    draw_text(
        dc,
        L"確認語: " + state.job_confirmation_token,
        token,
        state.mono_font,
        kInk,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT guidance{
        action_card.left + 16,
        action_card.top + 94,
        action_card.right - 210,
        action_card.bottom - 8};
    draw_text(
        dc,
        L"チェック後、確認語を入力すると" + operation_label +
            L"ボタンが有効になります。",
        guidance,
        state.small_font,
        kMuted,
        DT_LEFT | DT_TOP | DT_SINGLELINE);
  } else {
    RECT title{
        action_card.left + 16,
        action_card.top + 13,
        action_card.right - 16,
        action_card.top + 38};
    draw_text(
        dc,
        state.operation_busy
            ? L"読み取り専用で安全確認しています"
            : L"安全確認に合格したクローン／復元ジョブだけを実行できます",
        title,
        state.body_font,
        state.operation_busy ? kTsumugiBlue : kMuted,
        DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT guidance{
        action_card.left + 16,
        action_card.top + 48,
        action_card.right - 16,
        action_card.bottom - 12};
    draw_text(
        dc,
        L"イメージ作成・MBR→GPTジョブの実行は、"
        L"この製品画面にはまだ接続していません。",
        guidance,
        state.small_font,
        kMuted,
        DT_LEFT | DT_TOP | DT_WORDBREAK);
  }

  RECT result_card{content.left + 8, 476, content.right - 8, content.bottom};
  fill_rounded_rect(dc, result_card, kCard, kBorder);
  RECT result_title{
      result_card.left + 16,
      result_card.top + 11,
      result_card.right - 16,
      result_card.top + 35};
  draw_text(
      dc,
      state.job_progress_active
          ? operation_label + L"の状態"
          : state.operation_busy
          ? L"安全確認を実行中"
          : L"確認・実行結果",
      result_title,
      state.body_font,
      state.operation_busy ? kTsumugiBlue : kInk,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
}

void paint_repair_page(const AppState& state, HDC dc) {
  const RECT content = content_rect(state);
  RECT client{};
  GetClientRect(state.window, &client);
  const auto repair_actions =
      ytec::winpeapp::build_winpe_repair_action_layout(client.right);
  draw_stepper(
      state,
      dc,
      {L"対象選択", L"読取確認", L"最終確認", L"修復完了"},
      state.repair_step);
  RECT target_card{content.left + 8, 168, content.right - 8, 302};
  fill_rounded_rect(dc, target_card, kCard, kBorder);
  const int width = target_card.right - target_card.left - 28;
  const int field_width = (width - 42) / 4;
  const std::array<std::wstring_view, 4> labels{
      L"対象ディスク",
      L"Windows領域",
      L"システム領域",
      L"起動方式"};
  for (int index = 0; index < 4; ++index) {
    RECT bounds{
        target_card.left + 14 + index * (field_width + 14),
        179,
        target_card.left + 14 + index * (field_width + 14) + field_width,
        199};
    draw_text(
        dc,
        labels[static_cast<std::size_t>(index)],
        bounds,
        state.small_font,
        kMuted,
        DT_LEFT | DT_TOP | DT_SINGLELINE);
  }
  RECT note{
      repair_actions.note.left,
      repair_actions.note.top,
      repair_actions.note.right,
      repair_actions.note.bottom};
  draw_text(
      dc,
      L"WinRE診断も起動確認も読み取り専用です。"
      L"未割当領域は実行中だけ一時割当し、不明な構成は停止します。",
      note,
      state.small_font,
      kMuted,
      DT_LEFT | DT_TOP | DT_WORDBREAK);

  RECT confirm_card{content.left + 8, 320, content.right - 8, content.bottom};
  fill_rounded_rect(dc, confirm_card, kCard, kBorder);
  RECT confirm_title{
      confirm_card.left + 16,
      kRepairConfirmTitleTop,
      confirm_card.right - 16,
      kRepairConfirmTitleBottom};
  draw_text(
      dc,
      state.inspected_boot_selection.has_value()
          ? L"手順 2/2 — 対象固有の確認語を入力"
          : L"まず読み取り専用確認を完了してください",
      confirm_title,
      state.body_font,
      state.inspected_boot_selection.has_value() ? kWarning : kMuted,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
  RECT token_label{
      confirm_card.left + 16,
      kRepairTokenLabelTop,
      confirm_card.right - 16,
      kRepairTokenLabelBottom};
  draw_text(
      dc,
      state.boot_confirmation_token.empty()
          ? L"確認語は安全確認後に表示されます"
          : L"表示された確認語をそのまま入力してください",
      token_label,
      state.small_font,
      kMuted,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
  RECT result_label{
      confirm_card.left + 16,
      kRepairResultLabelTop,
      confirm_card.right - 16,
      kRepairResultLabelBottom};
  draw_text(
      dc,
      L"診断・実行結果",
      result_label,
      state.small_font,
      kMuted,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
}

void paint_disk_page(const AppState& state, HDC dc) {
  paint_status_banner(state, dc);
  const RECT content = content_rect(state);
  RECT list_card{content.left + 8, 154, content.left + (content.right - content.left) * 40 / 100, content.bottom};
  RECT detail_card{list_card.right + 12, 154, content.right - 8, content.bottom};
  fill_rounded_rect(dc, list_card, kCard, kBorder);
  fill_rounded_rect(dc, detail_card, kCard, kBorder);
  RECT list_label{list_card.left + 16, 164, list_card.right - 16, 190};
  draw_text(
      dc,
      L"物理ディスク",
      list_label,
      state.body_font,
      kInk,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
  RECT detail_label{detail_card.left + 16, 164, detail_card.right - 16, 190};
  draw_text(
      dc,
      L"読み取り専用の詳細",
      detail_label,
      state.body_font,
      kInk,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
}

void paint_window(const AppState& state, HDC dc) {
  RECT client{};
  GetClientRect(state.window, &client);
  FillRect(dc, &client, state.canvas_brush);
  paint_sidebar(state, dc, client);
  paint_header(state, dc, client);
  if (state.page == Page::job) {
    paint_job_page(state, dc);
  } else if (state.page == Page::boot_repair) {
    paint_repair_page(state, dc);
  } else {
    paint_disk_page(state, dc);
  }
}

void draw_navigation_button(
    const AppState& state,
    const DRAWITEMSTRUCT& item) {
  Page page = Page::job;
  if (item.CtlID == kNavRepairId) {
    page = Page::boot_repair;
  } else if (item.CtlID == kNavDiskId) {
    page = Page::disk_diagnostics;
  }
  const bool selected = state.page == page;
  const bool pressed = (item.itemState & ODS_SELECTED) != 0;
  const COLORREF background =
      pressed ? RGB(64, 78, 96)
              : selected ? kSidebarSelected : kSidebar;
  const HBRUSH brush = CreateSolidBrush(background);
  FillRect(item.hDC, &item.rcItem, brush);
  DeleteObject(brush);

  RECT text_bounds = item.rcItem;
  text_bounds.left += 16;
  const wchar_t* text = L"予約ジョブ";
  if (page == Page::boot_repair) {
    text = L"起動修復のみ";
  } else if (page == Page::disk_diagnostics) {
    text = L"ディスク診断";
  }
  draw_text(
      item.hDC,
      text,
      text_bounds,
      state.body_font,
      selected ? RGB(255, 255, 255) : RGB(205, 217, 225),
      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  if ((item.itemState & ODS_FOCUS) != 0) {
    RECT focus = item.rcItem;
    InflateRect(&focus, -3, -3);
    DrawFocusRect(item.hDC, &focus);
  }
}

void change_page(AppState& state, const Page page) {
  if (state.operation_busy) {
    return;
  }
  state.page = page;
  show_page_controls(state);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);
  const HWND focus = page == Page::job
      ? state.job_path
      : page == Page::boot_repair
      ? state.repair_disk
      : state.disk_list;
  SetFocus(focus);
}

void initialize_controls(AppState& state) {
  state.nav_job = create_control(
      state, L"BUTTON", L"", WS_TABSTOP | BS_OWNERDRAW, kNavJobId);
  state.nav_repair = create_control(
      state, L"BUTTON", L"", WS_TABSTOP | BS_OWNERDRAW, kNavRepairId);
  state.nav_disk = create_control(
      state, L"BUTTON", L"", WS_TABSTOP | BS_OWNERDRAW, kNavDiskId);
  state.refresh = create_control(
      state,
      L"BUTTON",
      L"再読込み",
      WS_TABSTOP | BS_PUSHBUTTON,
      kRefreshId);
  state.job_path = create_control(
      state,
      L"EDIT",
      L"",
      WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
      kJobPathId);
  state.job_browse = create_control(
      state,
      L"BUTTON",
      L"参照…",
      WS_TABSTOP | BS_PUSHBUTTON,
      kJobBrowseId);
  state.job_check = create_control(
      state,
      L"BUTTON",
      L"安全確認",
      WS_TABSTOP | BS_DEFPUSHBUTTON,
      kJobCheckId);
  state.job_acknowledge = create_control(
      state,
      L"BUTTON",
      L"確認した対象ディスクの全パーティションとデータが消去されることを理解しました",
      WS_TABSTOP | BS_AUTOCHECKBOX,
      kJobAcknowledgeId);
  state.job_token = create_control(
      state,
      L"EDIT",
      L"",
      WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
      kJobTokenId);
  state.job_execute = create_control(
      state,
      L"BUTTON",
      L"処理を開始",
      WS_TABSTOP | BS_PUSHBUTTON,
      kJobExecuteId);
  state.job_cancel = create_control(
      state,
      L"BUTTON",
      L"安全に取消",
      WS_TABSTOP | BS_PUSHBUTTON,
      kJobCancelId);
  state.job_output = create_control(
      state,
      L"EDIT",
      L"Windowsで作成した予約ジョブを選択してください。\r\n"
      L"ジョブ、イメージ、ディスク識別をすべて再確認します。",
      WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
          ES_READONLY,
      kJobOutputId);
  state.disk_list = create_control(
      state,
      L"LISTBOX",
      L"",
      WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY |
          LBS_NOINTEGRALHEIGHT,
      kDiskListId);
  state.disk_details = create_control(
      state,
      L"EDIT",
      L"",
      WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
          ES_READONLY,
      kDiskDetailsId);
  state.repair_disk = create_control(
      state,
      L"COMBOBOX",
      L"",
      WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
      kRepairDiskId);
  state.windows_root = create_control(
      state,
      L"COMBOBOX",
      L"",
      WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL,
      kWindowsRootId);
  state.system_root = create_control(
      state,
      L"COMBOBOX",
      L"",
      WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
      kSystemRootId);
  state.firmware = create_control(
      state,
      L"COMBOBOX",
      L"",
      WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
      kFirmwareId);
  state.repair_inspect = create_control(
      state,
      L"BUTTON",
      L"読み取り専用で確認",
      WS_TABSTOP | BS_DEFPUSHBUTTON,
      kRepairInspectId);
  state.winre_diagnostic = create_control(
      state,
      L"BUTTON",
      L"WinREを診断",
      WS_TABSTOP | BS_PUSHBUTTON,
      kWinReDiagnosticId);
  state.repair_acknowledge = create_control(
      state,
      L"BUTTON",
      L"選択したWindowsの起動ファイルが変更されることを理解しました",
      WS_TABSTOP | BS_AUTOCHECKBOX,
      kRepairAcknowledgeId);
  state.repair_token = create_control(
      state,
      L"EDIT",
      L"",
      WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
      kRepairTokenId);
  state.repair_execute = create_control(
      state,
      L"BUTTON",
      L"起動修復を実行",
      WS_TABSTOP | BS_PUSHBUTTON,
      kRepairExecuteId);
  state.repair_output = create_control(
      state,
      L"EDIT",
      L"対象を選び、「読み取り専用で確認」を押してください。\r\n"
      L"この段階ではBCDやディスクを変更しません。",
      WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
          ES_READONLY,
      kRepairOutputId);

  state.title_font = CreateFontW(
      -30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH, state.private_fonts.bold_face());
  state.heading_font = CreateFontW(
      -24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH, state.private_fonts.bold_face());
  state.body_font = CreateFontW(
      -18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH, state.private_fonts.regular_face());
  state.small_font = CreateFontW(
      -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH, state.private_fonts.regular_face());
  state.mono_font = CreateFontW(
      -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
  state.canvas_brush = CreateSolidBrush(kCanvas);
  state.card_brush = CreateSolidBrush(kCard);

  for (const HWND control :
       {state.nav_job,
        state.nav_repair,
        state.nav_disk,
        state.refresh,
        state.job_path,
        state.job_browse,
        state.job_check,
        state.job_acknowledge,
        state.job_token,
        state.job_execute,
        state.job_cancel,
        state.disk_list,
        state.repair_disk,
        state.windows_root,
        state.system_root,
        state.firmware,
        state.winre_diagnostic,
        state.repair_inspect,
        state.repair_acknowledge,
        state.repair_token,
        state.repair_execute}) {
    set_control_font(control, state.body_font);
  }
  set_control_font(state.job_output, state.small_font);
  set_control_font(state.disk_details, state.small_font);
  set_control_font(state.repair_output, state.small_font);
  SendMessageW(
      state.firmware,
      CB_ADDSTRING,
      0,
      reinterpret_cast<LPARAM>(L"UEFI（GPT）"));
  SendMessageW(
      state.firmware,
      CB_ADDSTRING,
      0,
      reinterpret_cast<LPARAM>(L"BIOS（MBR）"));
  SendMessageW(state.firmware, CB_SETCURSEL, 0, 0);
  populate_root_controls(state);
  show_page_controls(state);
  layout_controls(state);
}

void destroy_state_resources(AppState& state) {
  for (const HFONT font :
       {state.title_font,
        state.heading_font,
        state.body_font,
        state.small_font,
        state.mono_font}) {
    if (font != nullptr) {
      DeleteObject(font);
    }
  }
  if (state.canvas_brush != nullptr) {
    DeleteObject(state.canvas_brush);
  }
  if (state.card_brush != nullptr) {
    DeleteObject(state.card_brush);
  }
}

LRESULT CALLBACK window_proc(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
  auto* state = reinterpret_cast<AppState*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));

  if (message == WM_CREATE) {
    auto created = std::make_unique<AppState>();
    created->window = window;
    created->dashboard =
        ytec::winpeapp::DashboardView{
            .readiness = ytec::winpeapp::DashboardReadiness::scanning,
            .headline = L"起動環境を準備しています",
            .guidance = L"読み取り専用のディスク確認を開始します。"};
    state = created.release();
    SetWindowLongPtrW(
        window,
        GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(state));
    static_cast<void>(state->private_fonts.load_line_seed_jp(
        reinterpret_cast<HMODULE>(
            GetWindowLongPtrW(window, GWLP_HINSTANCE))));
    initialize_controls(*state);
    start_inventory(*state);
    return 0;
  }
  if (state == nullptr) {
    return DefWindowProcW(window, message, wparam, lparam);
  }

  switch (message) {
    case WM_SIZE:
      layout_controls(*state);
      return 0;
    case WM_GETMINMAXINFO: {
      auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
      limits->ptMinTrackSize.x = 1040;
      limits->ptMinTrackSize.y = 650;
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      const HDC dc = BeginPaint(window, &paint);
      paint_window(*state, dc);
      EndPaint(window, &paint);
      return 0;
    }
    case WM_DRAWITEM: {
      const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
      if (item != nullptr &&
          (item->CtlID == kNavJobId ||
           item->CtlID == kNavRepairId ||
           item->CtlID == kNavDiskId)) {
        draw_navigation_button(*state, *item);
        return TRUE;
      }
      break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
      const HDC control_dc = reinterpret_cast<HDC>(wparam);
      SetBkColor(control_dc, kCard);
      SetTextColor(control_dc, kInk);
      return reinterpret_cast<LRESULT>(state->card_brush);
    }
    case WM_COMMAND: {
      const int id = LOWORD(wparam);
      const int notification = HIWORD(wparam);
      if (id == kNavJobId && notification == BN_CLICKED) {
        change_page(*state, Page::job);
      } else if (id == kNavRepairId && notification == BN_CLICKED) {
        change_page(*state, Page::boot_repair);
      } else if (id == kNavDiskId && notification == BN_CLICKED) {
        change_page(*state, Page::disk_diagnostics);
      } else if (id == kRefreshId && notification == BN_CLICKED) {
        start_inventory(*state);
      } else if (id == kJobBrowseId && notification == BN_CLICKED) {
        std::wstring path = control_text(state->job_path);
        const BrowseResult browse_result = browse_for_job(window, path);
        if (browse_result == BrowseResult::selected) {
          SetWindowTextW(state->job_path, path.c_str());
        } else if (browse_result == BrowseResult::unavailable &&
                   path.empty()) {
          MessageBoxW(
              window,
              L"参照画面を開けない起動環境では、予約ジョブのフルパスを"
              L"入力欄へ直接入力してください。",
              L"予約ジョブの選択",
              MB_OK | MB_ICONINFORMATION);
        }
      } else if (id == kJobCheckId && notification == BN_CLICKED) {
        start_job_check(*state);
      } else if (id == kJobExecuteId && notification == BN_CLICKED) {
        start_job_execute(*state);
      } else if (id == kJobCancelId && notification == BN_CLICKED) {
        request_job_cancellation(*state);
      } else if (id == kJobPathId && notification == EN_CHANGE) {
        state->job_step = 1;
        if (!state->operation_busy) {
          invalidate_job_review(
              *state,
              L"予約ジョブが変更されました。「安全確認を開始」で"
              L"もう一度確認してください。"
              L"\r\nこの画面だけではクローン／復元を開始しません。");
          InvalidateRect(window, nullptr, FALSE);
        }
      } else if (id == kDiskListId && notification == LBN_SELCHANGE) {
        update_disk_details(*state);
      } else if (id == kRepairInspectId &&
                 notification == BN_CLICKED) {
        start_boot_inspect(*state);
      } else if (id == kWinReDiagnosticId &&
                 notification == BN_CLICKED) {
        start_winre_diagnostic(*state);
      } else if (id == kRepairExecuteId &&
                 notification == BN_CLICKED) {
        start_boot_execute(*state);
      } else if (id == kFirmwareId &&
                 notification == CBN_SELCHANGE) {
        select_default_roots(*state);
        invalidate_boot_review(*state);
      } else if ((id == kRepairDiskId &&
                  notification == CBN_SELCHANGE) ||
                 ((id == kWindowsRootId || id == kSystemRootId) &&
                  (notification == CBN_SELCHANGE ||
                   notification == CBN_EDITCHANGE))) {
        invalidate_boot_review(*state);
      }
      update_action_state(*state);
      return 0;
    }
    case kInventoryCompleteMessage: {
      std::unique_ptr<InventoryPayload> payload(
          reinterpret_cast<InventoryPayload*>(lparam));
      state->inventory_busy = false;
      if (payload != nullptr && payload->inventory.has_value()) {
        state->inventory = std::move(payload->inventory);
        state->dashboard =
            ytec::winpeapp::build_dashboard_view(state->inventory.value());
        populate_disk_controls(*state);
      } else {
        state->inventory.reset();
        state->dashboard =
            ytec::winpeapp::DashboardView{
                .readiness = ytec::winpeapp::DashboardReadiness::blocked,
                .headline = L"ディスク情報を取得できません",
                .guidance =
                    payload == nullptr || payload->error.empty()
                    ? L"再読込みしてください。処理は開始できません。"
                    : payload->error};
        SetWindowTextW(
            state->disk_details,
            state->dashboard.guidance.c_str());
      }
      update_action_state(*state);
      InvalidateRect(window, nullptr, TRUE);
      start_job_discovery(*state);
      return 0;
    }
    case kJobDiscoveryCompleteMessage: {
      std::unique_ptr<JobDiscoveryPayload> payload(
          reinterpret_cast<JobDiscoveryPayload*>(lparam));
      state->job_discovery_busy = false;
      if (payload == nullptr) {
        SetWindowTextW(
            state->job_output,
            L"予約ジョブの自動検出結果を受け取れませんでした。\r\n"
            L"参照ボタンまたはフルパス入力で選択してください。");
      } else if (!payload->error.empty()) {
        SetWindowTextW(
            state->job_output,
            (L"予約ジョブを安全に自動選択できませんでした。\r\n" +
             payload->error +
             L"\r\n候補を整理して再読込みするか、内容を確認してから"
             L"参照ボタンで選択してください。")
                .c_str());
      } else if (payload->path.has_value()) {
        SetWindowTextW(state->job_path, payload->path->c_str());
        start_job_check(*state);
      } else {
        SetWindowTextW(
            state->job_output,
            L"既定名の予約ジョブは見つかりませんでした。\r\n"
            L"参照ボタンまたはフルパス入力で選択できます。\r\n"
            L"この確認ではディスクを変更していません。");
      }
      update_action_state(*state);
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    case kJobCheckCompleteMessage: {
      std::unique_ptr<JobCheckPayload> payload(
          reinterpret_cast<JobCheckPayload*>(lparam));
      state->operation_busy = false;
      if (payload != nullptr) {
        state->job_execution_ready = payload->execution_ready;
        state->job_execution_kind = payload->kind;
        state->job_execution_mode =
            payload->execution_ready
            ? payload->execution_mode
            : ytec::imageformat::JobExecutionMode::review_required;
        state->job_payload_hash =
            payload->execution_ready
            ? payload->job_payload_hash
            : std::nullopt;
        state->job_confirmation_token =
            payload->execution_ready
            ? payload->confirmation_token
            : L"";
        state->job_step =
            payload->execution_ready ? 3 : payload->success ? 2 : 1;
        const bool copy_job = is_copy_job(payload->kind);
        const std::wstring operation_label(
            job_operation_label(payload->kind));
        SetWindowTextW(
            state->job_acknowledge,
            copy_job
                ? L"確認したコピー先の全パーティションとデータが消去されることを理解しました"
                : L"確認した復元先の全パーティションとデータが消去されることを理解しました");
        SetWindowTextW(
            state->job_execute,
            (operation_label + L"を開始").c_str());
        std::wstring text = payload->output;
        if (payload->execution_ready) {
          text +=
               L"\r\n\r\n必須安全確認に合格しました。"
               L"ここまでは" +
              std::wstring(job_target_label(payload->kind)) +
              L"を変更していません。"
               L"\r\n確認語:\r\n" +
               payload->confirmation_token;
          if (payload->execution_mode ==
              ytec::imageformat::JobExecutionMode::auto_once) {
            text +=
                L"\r\n\r\nWindows側で一回限り自動実行が明示許可されています。"
                L"開始記録を新規保存・読戻し確認してから実行へ進みます。";
          }
        } else if (
            payload->success &&
            payload->kind == JobExecutionKind::restore) {
          text +=
              L"\r\n\r\nジョブは読み取れましたが、必須安全条件に"
               L"危険または不明があるため復元を開始できません。";
        } else if (
            payload->success &&
            is_copy_job(payload->kind)) {
          text +=
              L"\r\n\r\nコピー／移行条件は読み取れましたが、コピー先を"
              L"一意に再確認できないため開始できません。";
        } else if (payload->success) {
          text +=
              L"\r\n\r\n安全確認は完了しました。"
              L"このジョブ種別の実行サービスは製品画面にまだ"
              L"接続していないため、対象ディスクは変更されていません。";
        }
        SetWindowTextW(state->job_output, text.c_str());
      }
      show_page_controls(*state);
      update_action_state(*state);
      InvalidateRect(window, nullptr, FALSE);
      if (state->job_execution_ready &&
          state->job_execution_mode ==
              ytec::imageformat::JobExecutionMode::auto_once) {
        start_job_execute(*state, true);
      }
      return 0;
    }
    case kJobProgressMessage: {
      std::unique_ptr<JobProgressPayload> payload(
          reinterpret_cast<JobProgressPayload*>(lparam));
      if (payload != nullptr && state->job_progress_active) {
        state->job_progress = payload->progress;
        state->job_elapsed = payload->elapsed;
        update_action_state(*state);
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }
    case kJobExecuteCompleteMessage: {
      std::unique_ptr<JobExecutePayload> payload(
          reinterpret_cast<JobExecutePayload*>(lparam));
      const JobExecutionKind completed_kind =
          payload != nullptr
              ? payload->kind
              : state->job_execution_kind;
      const std::wstring operation_label =
          std::wstring(job_operation_label(completed_kind));
      const std::wstring target_label =
          std::wstring(job_target_label(completed_kind));
      state->operation_busy = false;
      state->job_progress_active = false;
      state->job_execution_ready = false;
      state->job_cancellation.reset();
      state->job_confirmation_token.clear();
      SendMessageW(
          state->job_acknowledge,
          BM_SETCHECK,
          BST_UNCHECKED,
          0);
      SetWindowTextW(state->job_token, L"");
      if (payload != nullptr) {
        state->job_step = payload->success ? 4 : 1;
        std::wstring text = payload->output;
        if (payload->success) {
          if (completed_kind == JobExecutionKind::mbr_to_gpt) {
            text +=
                L"\r\n\r\nMBRクローン、Microsoft MBR2GPT、UEFI起動情報、"
                L"BCD、最終構成の検証まで完了しました。"
                L"\r\nPCを完全に終了し、コピー元を外してください。"
                L"コピー先だけを接続してUEFI起動へ切り替え、最初の起動を確認してください。";
          } else {
            text += L"\r\n\r\n" + operation_label +
                L"処理、読戻し検証、パーティション情報の確定、" +
                target_label + L"のオンライン復帰まで完了しました。";
          }
        } else {
          text += L"\r\n\r\n" + operation_label +
              L"は完了扱いにしていません。書込み開始後に"
              L"停止した場合、誤利用防止のため" + target_label +
              L"がオフラインのまま保護されている可能性があります。"
              L"\r\n再起動・初期化・再接続をせず、診断結果を保存して"
              L"確認してください。";
        }
        if (!payload->result_log_path.empty()) {
          text +=
              L"\r\n\r\n実行結果ログを新規保存し、全バイトを読戻し確認しました。"
              L"\r\n保存先: " + payload->result_log_path;
        } else if (!payload->result_log_error.empty()) {
          text +=
              L"\r\n\r\n実行結果ログは保存できませんでした。処理結果とは"
              L"分けて確認してください。\r\n" +
              payload->result_log_error;
        }
        SetWindowTextW(state->job_output, text.c_str());
      } else {
        state->job_step = 1;
        SetWindowTextW(
            state->job_output,
            (operation_label + L"実行結果を受け取れませんでした。" +
             target_label +
             L"がオフラインの可能性があるため、操作を続けないでください。")
                .c_str());
      }
      state->job_execution_kind = JobExecutionKind::none;
      state->job_execution_mode =
          ytec::imageformat::JobExecutionMode::review_required;
      state->job_payload_hash.reset();
      show_page_controls(*state);
      update_action_state(*state);
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    case kBootInspectCompleteMessage: {
      std::unique_ptr<BootInspectPayload> payload(
          reinterpret_cast<BootInspectPayload*>(lparam));
      state->operation_busy = false;
      if (payload != nullptr && payload->selection.has_value()) {
        state->inspected_boot_request = current_boot_request(*state);
        state->inspected_boot_selection =
            std::move(payload->selection);
        state->boot_confirmation_token =
            ytec::bootrepair::make_boot_repair_confirmation_token(
                state->inspected_boot_selection->identity,
                state->inspected_boot_request->firmware);
        state->repair_step = 3;
        std::wstring output =
            L"読み取り専用確認に合格しました。\r\n"
            L"・Windows領域: 確認済み\r\n"
            L"・システム領域: 確認済み" +
            std::wstring(
                state->inspected_boot_request->auto_mount_system_partition
                    ? L"（未割り当てのまま）\r\n"
                    : L"\r\n") +
            L"・ディスク形式と起動方式: 一致\r\n"
            L"・AMD64ブートローダー: 確認済み\r\n\r\n"
            L"確認語:\r\n" +
            state->boot_confirmation_token;
        SetWindowTextW(state->repair_output, output.c_str());
      } else {
        state->repair_step = 1;
        state->inspected_boot_request.reset();
        state->inspected_boot_selection.reset();
        state->boot_confirmation_token.clear();
        SetWindowTextW(
            state->repair_output,
            payload == nullptr || payload->error.empty()
                ? L"対象を安全に確定できませんでした。"
                : payload->error.c_str());
      }
      update_action_state(*state);
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    case kWinReDiagnosticCompleteMessage: {
      std::unique_ptr<WinReDiagnosticPayload> payload(
          reinterpret_cast<WinReDiagnosticPayload*>(lparam));
      state->operation_busy = false;
      SetWindowTextW(
          state->repair_output,
          payload == nullptr || payload->output.empty()
              ? L"WinRE診断結果を受け取れませんでした。"
              : payload->output.c_str());
      update_action_state(*state);
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    case kBootExecuteCompleteMessage: {
      std::unique_ptr<BootExecutePayload> payload(
          reinterpret_cast<BootExecutePayload*>(lparam));
      state->operation_busy = false;
      if (payload != nullptr) {
        state->repair_step = payload->success ? 4 : 1;
        SetWindowTextW(state->repair_output, payload->output.c_str());
      } else {
        state->repair_step = 1;
      }
      state->inspected_boot_request.reset();
      state->inspected_boot_selection.reset();
      state->boot_confirmation_token.clear();
      SendMessageW(
          state->repair_acknowledge,
          BM_SETCHECK,
          BST_UNCHECKED,
          0);
      SetWindowTextW(state->repair_token, L"");
      update_action_state(*state);
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    case WM_CLOSE:
      if (state->operation_busy || state->inventory_busy ||
          state->job_discovery_busy) {
        if (state->job_progress_active &&
            state->job_progress.cancellation_allowed) {
          request_job_cancellation(*state);
        }
        MessageBoxW(
            window,
            state->job_progress_active
                ? (is_copy_job(state->job_execution_kind)
                       ? L"コピー／移行処理が安全に停止または完了するまで画面を閉じられません。"
                       : L"復元処理が安全に停止または完了するまで画面を閉じられません。")
                : L"実行中の確認処理が完了するまで画面を閉じられません。",
            L"処理中です",
            MB_OK | MB_ICONINFORMATION);
        return 0;
      }
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      destroy_state_resources(*state);
      SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      delete state;
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(
    _In_ HINSTANCE instance,
    _In_opt_ HINSTANCE,
    _In_ PWSTR,
    _In_ int show_command) {
  SetProcessDPIAware();
  SetDefaultDllDirectories(
      LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground =
      reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = kWindowClass;
  if (RegisterClassExW(&window_class) == 0) {
    return 1;
  }

  RECT desired{0, 0, 1280, 720};
  AdjustWindowRectEx(
      &desired,
      WS_OVERLAPPEDWINDOW,
      FALSE,
      0);
  const int width = desired.right - desired.left;
  const int height = desired.bottom - desired.top;
  const int x = (std::max)(0, (GetSystemMetrics(SM_CXSCREEN) - width) / 2);
  const int y = (std::max)(0, (GetSystemMetrics(SM_CYSCREEN) - height) / 2);

  const HWND window = CreateWindowExW(
      0,
      kWindowClass,
      kWindowTitle,
      WS_OVERLAPPEDWINDOW,
      x,
      y,
      width,
      height,
      nullptr,
      nullptr,
      instance,
      nullptr);
  if (window == nullptr) {
    return 1;
  }
  ShowWindow(window, show_command);
  UpdateWindow(window);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
}
