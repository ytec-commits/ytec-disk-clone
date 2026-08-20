#include "ytec/bootrepair/clone_boot_finalization.h"

#include <Windows.h>

#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::clonecore::Error test_error(const std::wstring& operation) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_WRITE_FAULT,
      .operation = operation,
      .message = L"失敗注入",
  };
}

ytec::diskmodel::DiskInfo make_target() {
  using ytec::diskmodel::PartitionInfo;
  using ytec::diskmodel::PartitionStyle;
  ytec::diskmodel::DiskInfo disk{
      .disk_number = 3U,
      .device_path = L"\\\\.\\PhysicalDrive3",
      .device_instance_id = L"MOCK\\TARGET\\3",
      .model = L"Tsumugi Target",
      .size_bytes = 128ULL * 1024ULL * 1024ULL,
      .sector_count = 262144ULL,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .bus_type = L"SATA",
      .serial_suffix = "A1B2C3D4",
      .partition_style = PartitionStyle::gpt,
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = false,
  };
  disk.partitions = {
      PartitionInfo{
          .number = 1U,
          .offset_bytes = 1ULL * 1024ULL * 1024ULL,
          .size_bytes = 16ULL * 1024ULL * 1024ULL,
          .style = PartitionStyle::gpt,
          .type = L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}",
          .identifier = L"{00000000-0000-0000-0000-000000000001}",
          .name = L"EFI",
      },
      PartitionInfo{
          .number = 2U,
          .offset_bytes = 17ULL * 1024ULL * 1024ULL,
          .size_bytes = 96ULL * 1024ULL * 1024ULL,
          .style = PartitionStyle::gpt,
          .type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}",
          .identifier = L"{00000000-0000-0000-0000-000000000002}",
          .name = L"Windows",
      },
  };
  return disk;
}

ytec::clonecore::StableDiskIdentity identity_for(
    const ytec::diskmodel::DiskInfo& disk) {
  const auto identity = ytec::diskmodel::make_stable_disk_identity(
      disk, disk.is_system_disk);
  check(identity.has_value(), "Fixture identity should build");
  return identity.value();
}

ytec::bootrepair::BootVolumeObservation volume_for(
    const ytec::diskmodel::DiskInfo& disk,
    const ytec::diskmodel::PartitionInfo& partition,
    std::wstring volume_name,
    std::wstring file_system) {
  return ytec::bootrepair::BootVolumeObservation{
      .volume_name = std::move(volume_name),
      .location = ytec::bootrepair::BootRepairVolumeLocation{
          .disk_number = disk.disk_number,
          .starting_offset = partition.offset_bytes,
          .extent_length = partition.size_bytes,
          .file_system = std::move(file_system),
      },
  };
}

class Inventory final : public ytec::diskmodel::IDiskInventoryProvider {
 public:
  ytec::clonecore::Result<ytec::diskmodel::InventoryReport> enumerate()
      override {
    ++calls;
    const std::size_t index = reports.size() == 1U
        ? 0U
        : (std::min)(calls - 1U, reports.size() - 1U);
    return ytec::clonecore::Result<
        ytec::diskmodel::InventoryReport>::success(reports[index]);
  }

  std::vector<ytec::diskmodel::InventoryReport> reports;
  std::size_t calls{};
};

class Volumes final
    : public ytec::bootrepair::ICloneBootFinalizationVolumeProvider {
 public:
  ytec::clonecore::Result<std::vector<
      ytec::bootrepair::BootVolumeObservation>>
  observe_volumes_read_only() override {
    ++observe_calls;
    if (!observation_batches.empty()) {
      const std::size_t index = (std::min)(
          observe_calls - 1U, observation_batches.size() - 1U);
      return ytec::clonecore::Result<std::vector<
          ytec::bootrepair::BootVolumeObservation>>::success(
          observation_batches[index]);
    }
    return ytec::clonecore::Result<std::vector<
        ytec::bootrepair::BootVolumeObservation>>::success(observations);
  }

  ytec::clonecore::Status wait_before_volume_retry() override {
    ++wait_calls;
    if (fail_wait) {
      return ytec::clonecore::Status::failure(
          test_error(L"モックボリューム到着待機"));
    }
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Result<std::wstring>
  unavailable_drive_letters() override {
    return ytec::clonecore::Result<std::wstring>::success(L"C");
  }

  ytec::clonecore::Result<bool> contains_supported_offline_windows(
      const std::wstring& volume_root) override {
    const auto found = supported.find(volume_root);
    return ytec::clonecore::Result<bool>::success(
        found != supported.end() && found->second);
  }

  std::vector<ytec::bootrepair::BootVolumeObservation> observations;
  std::vector<std::vector<ytec::bootrepair::BootVolumeObservation>>
      observation_batches;
  std::map<std::wstring, bool> supported;
  std::size_t observe_calls{};
  std::size_t wait_calls{};
  bool fail_wait{};
};

class Mounts final : public ytec::bootrepair::ISystemVolumeMountApi {
 public:
  ytec::clonecore::Status attach(
      const std::wstring& root,
      const std::wstring& volume_name) override {
    attached.emplace(root, volume_name);
    attach_order.push_back(root);
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Result<ytec::bootrepair::BootVolumeObservation> inspect(
      const std::wstring& root,
      const std::wstring& volume_name,
      const ytec::bootrepair::BootRepairVolumeLocation& location) override {
    if (!attached.contains(root) || attached[root] != volume_name) {
      return ytec::clonecore::Result<
          ytec::bootrepair::BootVolumeObservation>::failure(
          test_error(L"モックマウント検査"));
    }
    return ytec::clonecore::Result<
        ytec::bootrepair::BootVolumeObservation>::success({
        .volume_name = volume_name,
        .location = location,
        .mount_points = {root},
    });
  }

  ytec::clonecore::Status detach(
      const std::wstring& root,
      const std::wstring& volume_name) override {
    if (!attached.contains(root) || attached[root] != volume_name) {
      return ytec::clonecore::Status::failure(
          test_error(L"モックマウント解除"));
    }
    attached.erase(root);
    detach_order.push_back(root);
    return ytec::clonecore::success_status();
  }

  std::map<std::wstring, std::wstring> attached;
  std::vector<std::wstring> attach_order;
  std::vector<std::wstring> detach_order;
};

class BootRepair final
    : public ytec::bootrepair::IStandaloneBootRepairService {
 public:
  explicit BootRepair(ytec::bootrepair::BootRepairTargetSelection selection)
      : selection_(std::move(selection)) {}

  ytec::clonecore::Result<ytec::bootrepair::BootRepairTargetSelection>
  inspect(const ytec::bootrepair::BootRepairTargetRequest& request) override {
    ++inspect_calls;
    last_request = request;
    return ytec::clonecore::Result<
        ytec::bootrepair::BootRepairTargetSelection>::success(selection_);
  }

  ytec::clonecore::Result<ytec::bootrepair::StandaloneBootRepairReport>
  execute(
      const ytec::bootrepair::StandaloneBootRepairExecutionRequest& request)
      override {
    ++execute_calls;
    if (fail_execute) {
      return ytec::clonecore::Result<
          ytec::bootrepair::StandaloneBootRepairReport>::failure(
          test_error(L"モックBCDBoot"));
    }
    check(request.target.store_policy ==
              ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh,
          "Finalizer must require a fresh BCD transaction");
    check(request.confirmation.first_step_acknowledged &&
              !request.confirmation.typed_token.empty(),
          "Finalizer must bind the internal repair confirmation");
    return ytec::clonecore::Result<
        ytec::bootrepair::StandaloneBootRepairReport>::success({
        .repaired = selection_,
        .bcdboot = ytec::bootrepair::BcdBootReport{
            .exit_code = 0U,
            .microsoft_signature_verified = true,
            .prior_store_replaced = true,
            .fresh_store_verified = true,
        },
        .boot_store_verified = true,
        .system_partition_temporarily_mounted = false,
        .temporary_mount_released = false,
    });
  }

  ytec::bootrepair::BootRepairTargetSelection selection_;
  ytec::bootrepair::BootRepairTargetRequest last_request;
  std::size_t inspect_calls{};
  std::size_t execute_calls{};
  bool fail_execute{};
};

struct Fixture final {
  Fixture() : target(make_target()), expected(identity_for(target)) {
    inventory.reports.push_back({.disks = {target}});
    volumes.observations = {
        volume_for(
            target,
            target.partitions[0],
            L"\\\\?\\Volume{00000000-0000-0000-0000-000000000001}\\",
            L"FAT32"),
        volume_for(
            target,
            target.partitions[1],
            L"\\\\?\\Volume{00000000-0000-0000-0000-000000000002}\\",
            L"NTFS"),
    };
    volumes.supported[volumes.observations[1].volume_name] = true;
  }

  ytec::bootrepair::BootRepairTargetSelection selection() const {
    return {
        .disk = target,
        .identity = expected,
        .windows_partition = target.partitions[1],
        .system_partition = target.partitions[0],
    };
  }

  ytec::bootrepair::CloneBootFinalizationRequest request() const {
    return {
        .expected_target = expected,
        .expected_style = ytec::diskmodel::PartitionStyle::gpt,
        .expected_windows_partition_offset = target.partitions[1].offset_bytes,
    };
  }

  ytec::diskmodel::DiskInfo target;
  ytec::clonecore::StableDiskIdentity expected;
  Inventory inventory;
  Volumes volumes;
  Mounts mounts;
};

void test_success_reidentifies_and_releases() {
  Fixture fixture;
  BootRepair boot(fixture.selection());
  const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
      fixture.request(), fixture.inventory, fixture.volumes, fixture.mounts,
      boot);
  check(result.has_value(), "GPT finalization should succeed");
  check(result.value().temporary_mounts_released &&
            result.value().final_target_reidentified &&
            result.value().partition_layout_unchanged,
        "Success must prove cleanup and final identity/layout");
  check(fixture.mounts.attach_order.size() == 2U &&
            fixture.mounts.detach_order.size() == 2U &&
            fixture.mounts.attached.empty(),
        "Both exact volumes must be temporarily mounted and released");
  check(boot.inspect_calls == 1U && boot.execute_calls == 1U,
        "Fresh repair must be inspected and executed once");
}

void test_repair_failure_still_releases_every_mount() {
  Fixture fixture;
  BootRepair boot(fixture.selection());
  boot.fail_execute = true;
  const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
      fixture.request(), fixture.inventory, fixture.volumes, fixture.mounts,
      boot);
  check(!result.has_value(), "Injected repair failure must propagate");
  check(fixture.mounts.detach_order.size() == 2U &&
            fixture.mounts.attached.empty(),
        "Repair failure must release both temporary roots");
}

void test_final_layout_change_is_rejected_after_cleanup() {
  Fixture fixture;
  auto changed = fixture.target;
  changed.partitions[1].name = L"Changed";
  fixture.inventory.reports.push_back({.disks = {changed}});
  BootRepair boot(fixture.selection());
  const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
      fixture.request(), fixture.inventory, fixture.volumes, fixture.mounts,
      boot);
  check(!result.has_value() &&
            result.error().code ==
                ytec::clonecore::ErrorCode::identity_mismatch,
        "A final layout change must fail closed");
  check(fixture.mounts.detach_order.size() == 2U &&
            fixture.mounts.attached.empty(),
        "Final reidentification failure occurs only after mount cleanup");
}

void test_ambiguous_windows_is_rejected_without_mounting() {
  Fixture fixture;
  auto extra = fixture.target.partitions[1];
  extra.number = 3U;
  extra.offset_bytes = 114ULL * 1024ULL * 1024ULL;
  extra.size_bytes = 8ULL * 1024ULL * 1024ULL;
  extra.identifier = L"{00000000-0000-0000-0000-000000000003}";
  fixture.target.partitions.push_back(extra);
  fixture.inventory.reports[0].disks[0] = fixture.target;
  fixture.volumes.observations.push_back(volume_for(
      fixture.target,
      fixture.target.partitions[2],
      L"\\\\?\\Volume{00000000-0000-0000-0000-000000000003}\\",
      L"NTFS"));
  fixture.volumes.supported[
      fixture.volumes.observations.back().volume_name] = true;
  auto request = fixture.request();
  request.expected_windows_partition_offset.reset();
  BootRepair boot({
      .disk = fixture.target,
      .identity = fixture.expected,
      .windows_partition = fixture.target.partitions[1],
      .system_partition = fixture.target.partitions[0],
  });
  const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
      request, fixture.inventory, fixture.volumes, fixture.mounts, boot);
  check(!result.has_value() && fixture.mounts.attach_order.empty() &&
            fixture.volumes.wait_calls == 0U,
        "Multiple supported Windows installations require an explicit choice");
}

void test_delayed_volume_arrival_reidentifies_before_success() {
  Fixture fixture;
  fixture.volumes.observation_batches = {
      {},
      fixture.volumes.observations,
  };
  BootRepair boot(fixture.selection());
  const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
      fixture.request(), fixture.inventory, fixture.volumes, fixture.mounts,
      boot);
  check(result.has_value(), "A delayed newly-online volume should settle");
  check(fixture.volumes.observe_calls == 2U &&
            fixture.volumes.wait_calls == 1U && fixture.inventory.calls == 3U,
        "Every retry must wait and reidentify before observing again");
}

void test_volume_arrival_timeout_is_bounded_without_mounting() {
  Fixture fixture;
  fixture.volumes.observations.clear();
  BootRepair boot(fixture.selection());
  const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
      fixture.request(), fixture.inventory, fixture.volumes, fixture.mounts,
      boot);
  check(!result.has_value() && result.error().native_code == ERROR_TIMEOUT,
        "A missing volume must end with an explicit bounded timeout");
  check(fixture.volumes.wait_calls > 0U &&
            fixture.volumes.wait_calls < 256U &&
            fixture.volumes.observe_calls == fixture.volumes.wait_calls + 1U &&
            fixture.mounts.attach_order.empty() && boot.inspect_calls == 0U,
        "Timeout retries must stay bounded and mutation-free");
}

void test_layout_change_during_volume_wait_is_rejected() {
  Fixture fixture;
  fixture.volumes.observation_batches = {
      {},
      fixture.volumes.observations,
  };
  auto changed = fixture.target;
  changed.partitions[1].size_bytes -= 512U;
  fixture.inventory.reports.push_back({.disks = {changed}});
  BootRepair boot(fixture.selection());
  const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
      fixture.request(), fixture.inventory, fixture.volumes, fixture.mounts,
      boot);
  check(!result.has_value() &&
            result.error().code ==
                ytec::clonecore::ErrorCode::identity_mismatch &&
            fixture.volumes.wait_calls == 1U &&
            fixture.mounts.attach_order.empty(),
        "A layout change while settling must fail before mounting");
}

void test_volume_wait_failure_propagates_without_reenumeration() {
  Fixture fixture;
  fixture.volumes.observations.clear();
  fixture.volumes.fail_wait = true;
  BootRepair boot(fixture.selection());
  const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
      fixture.request(), fixture.inventory, fixture.volumes, fixture.mounts,
      boot);
  check(!result.has_value() &&
            result.error().operation == L"モックボリューム到着待機" &&
            fixture.inventory.calls == 1U &&
            fixture.mounts.attach_order.empty(),
        "A wait failure must propagate before a second inventory query");
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, void (*)()>> tests{
      {"success_reidentifies_and_releases",
       test_success_reidentifies_and_releases},
      {"repair_failure_still_releases_every_mount",
       test_repair_failure_still_releases_every_mount},
      {"final_layout_change_is_rejected_after_cleanup",
       test_final_layout_change_is_rejected_after_cleanup},
      {"ambiguous_windows_is_rejected_without_mounting",
       test_ambiguous_windows_is_rejected_without_mounting},
      {"delayed_volume_arrival_reidentifies_before_success",
       test_delayed_volume_arrival_reidentifies_before_success},
      {"volume_arrival_timeout_is_bounded_without_mounting",
       test_volume_arrival_timeout_is_bounded_without_mounting},
      {"layout_change_during_volume_wait_is_rejected",
       test_layout_change_during_volume_wait_is_rejected},
      {"volume_wait_failure_propagates_without_reenumeration",
       test_volume_wait_failure_propagates_without_reenumeration},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << exception.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}
