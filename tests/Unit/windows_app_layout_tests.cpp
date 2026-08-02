#include "ytec/windowsapp/layout.h"

#include <Windows.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void check(const bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

void verify_width(const int client_width) {
  const auto layout =
      ytec::windowsapp::calculate_clone_column_layout(client_width);
  check(
      layout.source_card.contains(layout.source_control),
      "source dropdown must remain inside its card");
  check(
      layout.target_card.contains(layout.target_control),
      "target dropdown must remain inside its card");
  check(
      layout.source_card.right <= layout.target_card.left,
      "clone cards must not overlap");
  check(
      layout.source_control.right < layout.target_control.left,
      "clone dropdowns must not overlap");
  check(
      layout.target_card.right <= client_width - 36,
      "target card must remain inside the content boundary");

  const auto media =
      ytec::windowsapp::calculate_rescue_media_control_layout(
          client_width);
  check(
      media.card.contains(media.kind_control) &&
          media.card.contains(media.profile_control),
      "rescue media dropdowns must remain inside their card");
  check(
      media.kind_control.right < media.profile_control.left,
      "rescue media dropdowns must not overlap");
  check(
      media.card.contains(media.output_edit) &&
          media.card.contains(media.browse_button),
      "rescue media output controls must remain inside their card");
  check(
      media.output_edit.right < media.browse_button.left,
      "rescue media output controls must not overlap");
  check(
      media.profile_control.right <= media.card.right - 18,
      "rescue profile dropdown must keep right padding");
  check(
      media.browse_button.right <= media.card.right - 18,
      "rescue browse button must keep right padding");
}

INT_PTR CALLBACK inert_dialog_proc(
    HWND,
    UINT,
    WPARAM,
    LPARAM) {
  return FALSE;
}

RECT child_rect_in_dialog(const HWND dialog, const int identifier) {
  const HWND child = GetDlgItem(dialog, identifier);
  if (child == nullptr) {
    check(false, "confirmation dialog control must exist");
    return {};
  }
  RECT area{};
  check(
      GetWindowRect(child, &area) != FALSE,
      "confirmation dialog control rectangle must be readable");
  MapWindowPoints(
      nullptr,
      dialog,
      reinterpret_cast<POINT*>(&area),
      2);
  return area;
}

bool overlaps(const RECT& left, const RECT& right) {
  return left.left < right.right && left.right > right.left &&
      left.top < right.bottom && left.bottom > right.top;
}

void verify_confirmation_dialog_resource() {
  constexpr int kDialogId = 101;
  constexpr std::array<int, 6> kControlIds{
      1001, 1002, 1003, 1004, IDOK, IDCANCEL};
  const HWND dialog = CreateDialogParamW(
      GetModuleHandleW(nullptr),
      MAKEINTRESOURCEW(kDialogId),
      nullptr,
      inert_dialog_proc,
      0);
  if (dialog == nullptr) {
    check(false, "confirmation dialog resource must instantiate");
    return;
  }
  RECT client{};
  check(
      GetClientRect(dialog, &client) != FALSE,
      "confirmation dialog client rectangle must be readable");

  std::array<RECT, kControlIds.size()> controls{};
  for (std::size_t index = 0; index < kControlIds.size(); ++index) {
    controls[index] = child_rect_in_dialog(dialog, kControlIds[index]);
    check(
        controls[index].left >= client.left &&
            controls[index].top >= client.top &&
            controls[index].right <= client.right &&
            controls[index].bottom <= client.bottom,
        "confirmation dialog controls must remain inside the client area");
  }
  for (std::size_t left = 0; left < controls.size(); ++left) {
    for (std::size_t right = left + 1; right < controls.size(); ++right) {
      check(
          !overlaps(controls[left], controls[right]),
          "confirmation dialog controls must not overlap");
    }
  }

  const HWND auto_once = GetDlgItem(dialog, 1004);
  if (auto_once == nullptr) {
    DestroyWindow(dialog);
    check(false, "auto-once checkbox must exist");
    return;
  }
  const int text_length = GetWindowTextLengthW(auto_once);
  std::vector<wchar_t> text(
      static_cast<std::size_t>(text_length + 1), L'\0');
  GetWindowTextW(auto_once, text.data(), static_cast<int>(text.size()));
  HDC dc = GetDC(auto_once);
  if (dc == nullptr) {
    DestroyWindow(dialog);
    check(false, "checkbox text device context must be available");
    return;
  }
  const auto font = reinterpret_cast<HFONT>(
      SendMessageW(auto_once, WM_GETFONT, 0, 0));
  const HGDIOBJ previous =
      font == nullptr ? nullptr : SelectObject(dc, font);
  RECT measured{};
  DrawTextW(
      dc,
      text.data(),
      text_length,
      &measured,
      DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
  if (previous != nullptr) {
    SelectObject(dc, previous);
  }
  ReleaseDC(auto_once, dc);
  check(
      measured.right - measured.left + 28 <=
          controls[3].right - controls[3].left,
      "auto-once checkbox text must fit without clipping");
  DestroyWindow(dialog);
}

}  // namespace

int main() {
  verify_width(964);
  verify_width(1024);
  verify_width(1266);
  verify_width(1280);
  verify_width(1600);
  verify_confirmation_dialog_resource();
  std::cout << "windows app layout tests: PASS\n";
  return 0;
}
