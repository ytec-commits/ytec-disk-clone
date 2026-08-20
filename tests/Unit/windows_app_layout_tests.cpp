#include "ytec/windowsapp/layout.h"

#include <Windows.h>

#include <array>
#include <cstdlib>
#include <iostream>

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
      media.card.contains(media.mode_control) &&
          media.card.contains(media.file_system_control),
      "rescue USB mode and filesystem dropdowns must remain inside their card");
  check(
      media.mode_control.right < media.file_system_control.left,
      "rescue USB mode and filesystem dropdowns must not overlap");
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
      media.file_system_control.right <= media.card.right - 18,
      "rescue filesystem dropdown must keep right padding");
  check(
      media.browse_button.right <= media.card.right - 18,
      "rescue browse button must keep right padding");

  const auto actions =
      ytec::windowsapp::calculate_bottom_action_layout(client_width);
  check(
      actions.secondary_action.left >= 286,
      "secondary action must remain inside the content boundary");
  check(
      actions.secondary_action.right < actions.primary_action.left,
      "bottom actions must not overlap");
  check(
      actions.primary_action.right <= client_width - 36,
      "primary action must keep the right margin");
  check(
      actions.primary_action.width() >= 300,
      "primary action must be wide enough for long Japanese labels");

  const auto image_options = ytec::windowsapp::
      calculate_image_create_option_layout(client_width);
  check(
      image_options.verification_control.left >= 312 &&
          image_options.transfer_control.right <= client_width - 36,
      "image-create options must remain inside the content boundary");
  check(
      image_options.verification_control.right <
          image_options.transfer_control.left,
      "verification and transfer mode selectors must not overlap");
  check(
      image_options.verification_control.width() >= 250 &&
          image_options.transfer_control.width() >= 250,
      "image-create option labels need a non-truncating width");
}

void verify_rescue_media_compact_height(
    const int client_height,
    const bool usb_selected) {
  constexpr int kControlHeight = 34;
  constexpr int kCardBottomMargin = 92;
  constexpr int kBottomActionTopMargin = 72;
  const auto layout = ytec::windowsapp::
      calculate_rescue_media_vertical_layout(client_height, usb_selected);
  check(layout.compact, "short rescue-media layout must be compact");
  check(
      layout.kind_label_top < layout.kind_control_top &&
          layout.option_label_top < layout.option_control_top &&
          layout.destination_label_top <
              layout.destination_control_top,
      "every rescue-media label must precede its control");
  check(
      layout.destination_control_top + kControlHeight <=
          client_height - kCardBottomMargin,
      "the compact rescue destination must remain inside its card");
  check(
      layout.destination_control_top + kControlHeight <
          client_height - kBottomActionTopMargin,
      "the compact rescue destination must not overlap the primary action");
  if (usb_selected) {
    check(
        layout.kind_control_top < layout.option_label_top &&
            layout.option_control_top < layout.destination_label_top,
        "the three compact USB rows must remain separated");
  }
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
  constexpr std::array<int, 5> kControlIds{
      1001, 1002, 1003, IDOK, IDCANCEL};
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

  DestroyWindow(dialog);
}

}  // namespace

int main() {
  verify_width(964);
  verify_width(1024);
  verify_width(1266);
  verify_width(1280);
  verify_width(1600);
  verify_rescue_media_compact_height(516, true);
  verify_rescue_media_compact_height(516, false);
  verify_rescue_media_compact_height(600, true);
  check(
      !ytec::windowsapp::
           calculate_rescue_media_vertical_layout(720, true)
           .compact,
      "720-high rescue-media layout should keep the spacious rows");
  verify_confirmation_dialog_resource();
  std::cout << "windows app layout tests: PASS\n";
  return 0;
}
