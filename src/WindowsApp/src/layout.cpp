#include "ytec/windowsapp/layout.h"

#include <algorithm>

namespace ytec::windowsapp {

CloneColumnLayout calculate_clone_column_layout(
    const int client_width) noexcept {
  constexpr int kContentLeft = 286;
  constexpr int kContentRightMargin = 36;
  constexpr int kColumnGap = 18;
  constexpr int kCardPadding = 18;

  const int content_right = (std::max)(kContentLeft, client_width - kContentRightMargin);
  const int available = content_right - kContentLeft;
  const int column_width = (std::max)(0, (available - kColumnGap) / 2);
  const int source_left = kContentLeft;
  const int target_left = source_left + column_width + kColumnGap;

  const HorizontalBounds source_card{
      source_left, source_left + column_width};
  const HorizontalBounds target_card{
      target_left, target_left + column_width};
  const HorizontalBounds source_control{
      source_card.left + kCardPadding,
      (std::max)(source_card.left + kCardPadding,
                 source_card.right - kCardPadding)};
  const HorizontalBounds target_control{
      target_card.left + kCardPadding,
      (std::max)(target_card.left + kCardPadding,
                 target_card.right - kCardPadding)};
  return CloneColumnLayout{
      .source_card = source_card,
      .target_card = target_card,
      .source_control = source_control,
      .target_control = target_control};
}

RescueMediaControlLayout calculate_rescue_media_control_layout(
    const int client_width) noexcept {
  constexpr int kCardLeft = 286;
  constexpr int kCardRightMargin = 36;
  constexpr int kInnerLeft = 312;
  constexpr int kInnerRightPadding = 18;
  constexpr int kControlGap = 18;
  constexpr int kBrowseButtonWidth = 156;

  const int card_right = (std::max)(kCardLeft, client_width - kCardRightMargin);
  const int inner_right = (std::max)(kInnerLeft, card_right - kInnerRightPadding);
  const int inner_width = inner_right - kInnerLeft;
  const int column_width = (std::max)(0, (inner_width - kControlGap) / 2);
  const int browse_left = (std::max)(
      kInnerLeft,
      inner_right - kBrowseButtonWidth);
  const int edit_right = (std::max)(
      kInnerLeft,
      browse_left - kControlGap);

  return RescueMediaControlLayout{
      .card = HorizontalBounds{kCardLeft, card_right},
      .kind_control = HorizontalBounds{
          kInnerLeft, kInnerLeft + column_width},
      .profile_control = HorizontalBounds{
          kInnerLeft + column_width + kControlGap,
          kInnerLeft + column_width * 2 + kControlGap},
      .output_edit = HorizontalBounds{kInnerLeft, edit_right},
      .browse_button = HorizontalBounds{browse_left, inner_right}};
}

}  // namespace ytec::windowsapp
