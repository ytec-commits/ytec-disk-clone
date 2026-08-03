#include "ytec/windowsapp/selection.h"

#include "ytec/diskmodel/clone_target_layout.h"

namespace ytec::windowsapp {

CloneSelectionView evaluate_clone_selection(
    const diskmodel::InventoryReport* const inventory,
    const std::optional<std::size_t> source_index,
    const std::optional<std::size_t> target_index,
    const bool inventory_loading,
    const bool require_target_same_or_larger) {
  if (inventory_loading) {
    return CloneSelectionView{
        .issue = CloneSelectionIssue::loading,
        .message = L"ディスクを読み取り専用で確認しています…"};
  }
  if (inventory == nullptr || inventory->disks.empty()) {
    return CloneSelectionView{
        .issue = CloneSelectionIssue::inventory_unavailable,
        .message = L"利用できるディスク情報がありません。"};
  }
  if (!source_index.has_value() || !target_index.has_value() ||
      source_index.value() >= inventory->disks.size() ||
      target_index.value() >= inventory->disks.size()) {
    return CloneSelectionView{
        .issue = CloneSelectionIssue::disk_not_selected,
        .message = L"コピー元とコピー先を選択してください。"};
  }
  if (source_index.value() == target_index.value()) {
    return CloneSelectionView{
        .issue = CloneSelectionIssue::same_disk,
        .message = L"同じディスクをコピー元とコピー先には指定できません。"};
  }

  const auto& source = inventory->disks[source_index.value()];
  const auto& target = inventory->disks[target_index.value()];
  if (target.is_system_disk) {
    return CloneSelectionView{
        .issue = CloneSelectionIssue::target_is_system,
        .message =
            L"現在動作中のWindowsディスクはコピー先にできません。"};
  }
  if (!target.read_only.has_value()) {
    return CloneSelectionView{
        .issue = CloneSelectionIssue::target_state_unknown,
        .message =
            L"コピー先の読み取り専用状態を確認できないため進めません。"};
  }
  if (target.read_only.value()) {
    return CloneSelectionView{
        .issue = CloneSelectionIssue::target_is_read_only,
        .message = L"コピー先ディスクが読み取り専用です。"};
  }
  const auto target_layout =
      diskmodel::classify_clone_target_layout(target);
  if (target_layout == diskmodel::CloneTargetLayoutKind::unsupported) {
    return CloneSelectionView{
        .issue = CloneSelectionIssue::target_layout_unsupported,
        .message =
            L"不明・動的・Storage Spacesの構成はコピー先にできません。"};
  }
  if (require_target_same_or_larger &&
      target.size_bytes < source.size_bytes) {
    return CloneSelectionView{
        .issue = CloneSelectionIssue::target_too_small,
        .message =
            L"コピー先の容量がコピー元より小さいため進めません。"};
  }
  return CloneSelectionView{
      .issue = CloneSelectionIssue::ready,
      .ready = true,
      .target_requires_initialization =
          target_layout ==
          diskmodel::CloneTargetLayoutKind::supported_initialized,
      .message = target_layout ==
              diskmodel::CloneTargetLayoutKind::supported_initialized
          ? L"フォーマット済みコピー先です。確認後、WinPEで全領域を初期化して実行します。"
          : L"選択内容は安全確認へ進めます。実コピーはWinPEで行います。"};
}

}  // namespace ytec::windowsapp
