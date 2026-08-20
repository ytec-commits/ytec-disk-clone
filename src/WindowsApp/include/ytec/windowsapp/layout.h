#pragma once

namespace ytec::windowsapp {

struct HorizontalBounds final {
  int left{};
  int right{};

  [[nodiscard]] int width() const noexcept {
    return right > left ? right - left : 0;
  }

  [[nodiscard]] bool contains(const HorizontalBounds& other) const noexcept {
    return other.left >= left && other.right <= right;
  }
};

struct CloneColumnLayout final {
  HorizontalBounds source_card;
  HorizontalBounds target_card;
  HorizontalBounds source_control;
  HorizontalBounds target_control;
};

struct RescueMediaControlLayout final {
  HorizontalBounds card;
  HorizontalBounds kind_control;
  HorizontalBounds profile_control;
  HorizontalBounds mode_control;
  HorizontalBounds file_system_control;
  HorizontalBounds output_edit;
  HorizontalBounds browse_button;
};

struct RescueMediaVerticalLayout final {
  bool compact{};
  int kind_label_top{};
  int kind_control_top{};
  int option_label_top{};
  int option_control_top{};
  int destination_label_top{};
  int destination_control_top{};
};

struct BottomActionLayout final {
  HorizontalBounds secondary_action;
  HorizontalBounds primary_action;
};

struct ImageCreateOptionLayout final {
  HorizontalBounds verification_control;
  HorizontalBounds transfer_control;
};

[[nodiscard]] CloneColumnLayout calculate_clone_column_layout(
    int client_width) noexcept;

[[nodiscard]] RescueMediaControlLayout
calculate_rescue_media_control_layout(int client_width) noexcept;

[[nodiscard]] RescueMediaVerticalLayout
calculate_rescue_media_vertical_layout(
    int client_height,
    bool usb_selected) noexcept;

[[nodiscard]] BottomActionLayout calculate_bottom_action_layout(
    int client_width) noexcept;

[[nodiscard]] ImageCreateOptionLayout
calculate_image_create_option_layout(int client_width) noexcept;

}  // namespace ytec::windowsapp
