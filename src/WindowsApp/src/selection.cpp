#include "ytec/windowsapp/selection.h"

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
  if (target.partition_style != diskmodel::PartitionStyle::raw ||
      !target.partitions.empty()) {
    return CloneSelectionView{
        .issue = CloneSelectionIssue::target_not_empty,
        .message =
            L"誤消去防止のため、空のRAWディスクだけをコピー先にできます。"};
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
      .message =
          L"選択内容は安全確認へ進めます。まだ書き込みは始まりません。"};
}

}  // namespace ytec::windowsapp
