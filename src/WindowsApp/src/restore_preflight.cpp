#include "ytec/windowsapp/restore_preflight.h"

namespace ytec::windowsapp {
namespace {

bool identifies_same_device(
    const clonecore::StableDiskIdentity& left,
    const clonecore::StableDiskIdentity& right) {
  if (!left.serial_suffix.empty() &&
      !right.serial_suffix.empty() &&
      left.model == right.model &&
      left.serial_suffix == right.serial_suffix) {
    return true;
  }
  return !left.device_instance_id.empty() &&
         !right.device_instance_id.empty() &&
         left.device_instance_id == right.device_instance_id;
}

}  // namespace

clonecore::Result<RestoreImagePreflightReport>
inspect_restore_image_reader(
    std::wstring canonical_path,
    const std::uint64_t image_length,
    const imageformat::Sha256ReadCallback& reader,
    const RestoreImagePreflightOptions& options) {
  return imageformat::inspect_restore_image_reader_v1(
      std::move(canonical_path), image_length, reader, options);
}

clonecore::Result<RestoreImagePreflightReport>
inspect_restore_image_file(
    const std::wstring& requested_path,
    const RestoreImagePreflightOptions& options) {
  return imageformat::inspect_restore_image_file_v1(
      requested_path, options);
}

RestoreTargetSelectionView evaluate_restore_target_selection(
    const RestoreImagePreflightReport* const image,
    const diskmodel::InventoryReport* const inventory,
    const std::optional<std::size_t> target_index,
    const bool inventory_loading) {
  if (image == nullptr ||
      !image->complete_container_verified ||
      !image->metadata_verified ||
      !image->restore_layout_verified) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::image_unavailable,
        .message =
            L"先に復元イメージの読み取り専用完全検証を完了してください。"};
  }
  if (inventory_loading) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::inventory_loading,
        .message =
            L"復元先候補を読み取り専用で確認しています…"};
  }
  if (inventory == nullptr || inventory->disks.empty()) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::inventory_unavailable,
        .message = L"復元先候補のディスク情報を取得できません。"};
  }
  if (!inventory->issues.empty()) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::inventory_has_issues,
        .message =
            L"未解決のディスク列挙診断があるため候補を確定できません。"};
  }
  if (!target_index.has_value() ||
      target_index.value() >= inventory->disks.size()) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::target_not_selected,
        .message = L"読み取り専用で確認する復元先候補を選択してください。"};
  }

  const auto& target = inventory->disks[target_index.value()];
  const auto identity =
      diskmodel::make_stable_disk_identity(
          target, target.is_system_disk);
  if (!identity) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::unstable_identity,
        .message =
            L"候補の安定識別情報が不足しているため進めません。"};
  }
  if (identifies_same_device(
          image->manifest.source, identity.value())) {
    return RestoreTargetSelectionView{
        .issue =
            RestoreTargetSelectionIssue::target_is_original_source,
        .message =
            L"イメージに記録されたコピー元と同じディスクは復元先にできません。"};
  }
  if (target.is_system_disk) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::target_is_system,
        .message =
            L"現在動作中のWindowsディスクは復元先にできません。"};
  }
  if (!target.read_only.has_value() ||
      !target.removable.has_value()) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::target_state_unknown,
        .message =
            L"読取り専用またはリムーバブル状態が不明なため進めません。"};
  }
  if (target.read_only.value()) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::target_is_read_only,
        .message = L"候補ディスクが読み取り専用です。"};
  }
  if (target.removable.value()) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::target_is_removable,
        .message =
            L"リムーバブル媒体は初版の物理復元先にできません。"};
  }
  if (target.partition_style ==
      diskmodel::PartitionStyle::unknown) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::target_style_unknown,
        .message =
            L"候補ディスクのパーティション形式が不明なため進めません。"};
  }
  if (target.size_bytes < image->header.source_disk_size) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::target_too_small,
        .message =
            L"候補の容量がイメージ元ディスクより小さいため進めません。"};
  }
  if (target.logical_sector_size !=
      image->header.logical_sector_size) {
    return RestoreTargetSelectionView{
        .issue =
            RestoreTargetSelectionIssue::logical_sector_mismatch,
        .message =
            L"候補とイメージの論理セクターサイズが一致しません。"};
  }
  return RestoreTargetSelectionView{
      .issue =
          RestoreTargetSelectionIssue::ready_for_confirmation,
      .ready_for_confirmation = true,
      .restore_execution_enabled = false,
      .target_identity = identity.value(),
      .message =
          L"容量・セクター・基本状態は確認済みです。"
          L"実行前の詳細検査と二段階確認はまだ行っていません。"};
}

}  // namespace ytec::windowsapp
