#include "ytec/bootrepair/standalone_repair.h"

#include "ytec/bootrepair/offline_windows.h"
#include "ytec/bootrepair/system_volume_mount.h"
#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwctype>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace ytec::bootrepair {
namespace {

constexpr std::wstring_view kEfiPartitionType =
    L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}";
constexpr std::wstring_view kBasicDataPartitionType =
    L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}";

clonecore::Error repair_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

bool is_drive_root(const std::wstring& value) {
  return value.size() == 3 && std::iswalpha(value[0]) != 0 &&
         value[1] == L':' && (value[2] == L'\\' || value[2] == L'/');
}

std::wstring normalized_root(std::wstring value) {
  if (is_drive_root(value)) {
    value[0] = static_cast<wchar_t>(std::towupper(value[0]));
    value[2] = L'\\';
  }
  return value;
}

bool equals_case_insensitive(
    const std::wstring& left,
    const std::wstring_view right) {
  return left.size() == right.size() &&
         _wcsicmp(left.c_str(), right.data()) == 0;
}

clonecore::Result<BootRepairVolumeLocation> query_volume_location(
    const std::wstring& root) {
  if (!is_drive_root(root)) {
    return clonecore::Result<BootRepairVolumeLocation>::failure(repair_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"起動修復対象ボリューム",
        L"ドライブ文字のルートを指定してください"));
  }
  const std::wstring normalized = normalized_root(root);
  const std::wstring device = L"\\\\.\\" + normalized.substr(0, 2);
  clonecore::UniqueHandle volume(CreateFileW(
      device.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  if (!volume) {
    return clonecore::Result<BootRepairVolumeLocation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"起動修復対象ボリュームを読取り専用で開く",
            GetLastError()));
  }

  std::array<std::byte, 1024> buffer{};
  DWORD bytes_returned = 0;
  if (!DeviceIoControl(
          volume.get(),
          IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
          nullptr,
          0,
          buffer.data(),
          static_cast<DWORD>(buffer.size()),
          &bytes_returned,
          nullptr)) {
    return clonecore::Result<BootRepairVolumeLocation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"起動修復対象ボリュームのディスク対応取得",
            GetLastError()));
  }
  constexpr std::size_t kRequiredBytes =
      offsetof(VOLUME_DISK_EXTENTS, Extents) + sizeof(DISK_EXTENT);
  const auto* const extents =
      reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
  if (bytes_returned < kRequiredBytes ||
      extents->NumberOfDiskExtents != 1 ||
      extents->Extents[0].StartingOffset.QuadPart < 0 ||
      extents->Extents[0].ExtentLength.QuadPart <= 0) {
    return clonecore::Result<BootRepairVolumeLocation>::failure(repair_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"起動修復対象ボリュームのディスク対応検証",
        L"単一物理ディスク上の通常パーティションだけを使用できます"));
  }

  std::array<wchar_t, MAX_PATH> file_system{};
  if (!GetVolumeInformationW(
          normalized.c_str(),
          nullptr,
          0,
          nullptr,
          nullptr,
          nullptr,
          file_system.data(),
          static_cast<DWORD>(file_system.size()))) {
    return clonecore::Result<BootRepairVolumeLocation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"起動修復対象ファイルシステム取得",
            GetLastError()));
  }
  return clonecore::Result<BootRepairVolumeLocation>::success(
      BootRepairVolumeLocation{
          .disk_number = extents->Extents[0].DiskNumber,
          .starting_offset = static_cast<std::uint64_t>(
              extents->Extents[0].StartingOffset.QuadPart),
          .extent_length = static_cast<std::uint64_t>(
              extents->Extents[0].ExtentLength.QuadPart),
          .file_system = file_system.data(),
      });
}

clonecore::Status verify_regular_file(
    const std::wstring& path,
    std::wstring operation,
    std::wstring missing_message) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return clonecore::Status::failure(repair_error(
        clonecore::ErrorCode::invalid_data,
        attributes == INVALID_FILE_ATTRIBUTES ? GetLastError()
                                               : ERROR_REPARSE_TAG_INVALID,
        std::move(operation),
        std::move(missing_message)));
  }
  return clonecore::success_status();
}

bool is_administrator() {
  SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
  PSID administrators = nullptr;
  if (!AllocateAndInitializeSid(
          &authority,
          2,
          SECURITY_BUILTIN_DOMAIN_RID,
          DOMAIN_ALIAS_RID_ADMINS,
          0,
          0,
          0,
          0,
          0,
          0,
          &administrators)) {
    return false;
  }
  BOOL member = FALSE;
  const BOOL checked = CheckTokenMembership(nullptr, administrators, &member);
  FreeSid(administrators);
  return checked != FALSE && member != FALSE;
}

bool same_partition(
    const diskmodel::PartitionInfo& expected,
    const diskmodel::PartitionInfo& observed) {
  return expected.number == observed.number &&
         expected.offset_bytes == observed.offset_bytes &&
         expected.size_bytes == observed.size_bytes &&
         expected.style == observed.style &&
         expected.type == observed.type &&
         expected.bootable == observed.bootable;
}

clonecore::Status verify_boot_store(
    const std::wstring& system_root,
    const BcdBootFirmware firmware) {
  const std::wstring root = normalized_root(system_root);
  const std::wstring store_path = firmware == BcdBootFirmware::uefi
      ? root + L"EFI\\Microsoft\\Boot\\BCD"
      : root + L"Boot\\BCD";
  return verify_regular_file(
      store_path,
      L"再構築後BCDストア検証",
      L"BCDBoot成功後に通常ファイルのBCDストアを確認できません");
}

std::wstring used_drive_letters() {
  const DWORD mask = GetLogicalDrives();
  std::wstring letters;
  if (mask == 0U) {
    return letters;
  }
  for (std::uint32_t index = 0U; index < 26U; ++index) {
    if ((mask & (1U << index)) != 0U) {
      letters.push_back(static_cast<wchar_t>(L'A' + index));
    }
  }
  return letters;
}

struct InspectedBootRepairTarget final {
  BootRepairTargetSelection selection;
  std::optional<TemporarySystemVolumeMountPlan> temporary_mount;
};

class WindowsStandaloneBootRepairService final
    : public IStandaloneBootRepairService {
 public:
  explicit WindowsStandaloneBootRepairService(
      diskmodel::IDiskInventoryProvider& inventory)
      : inventory_(inventory), mount_api_(make_windows_system_volume_mount_api()) {}

  clonecore::Result<BootRepairTargetSelection> inspect(
      const BootRepairTargetRequest& request) override {
    auto inspected = inspect_target(request);
    if (!inspected) {
      return clonecore::Result<BootRepairTargetSelection>::failure(
          inspected.error());
    }
    return clonecore::Result<BootRepairTargetSelection>::success(
        std::move(inspected.value().selection));
  }

  clonecore::Result<StandaloneBootRepairReport> execute(
      const StandaloneBootRepairExecutionRequest& request) override {
    if (!is_administrator()) {
      return clonecore::Result<StandaloneBootRepairReport>::failure(
          repair_error(
              clonecore::ErrorCode::access_denied,
              ERROR_ELEVATION_REQUIRED,
              L"単独起動修復の管理者権限確認",
              L"起動ファイルを変更するにはWinPEまたは管理者権限が必要です"));
    }
    auto inspected = inspect_target(request.target);
    if (!inspected) {
      return clonecore::Result<StandaloneBootRepairReport>::failure(
          inspected.error());
    }
    auto& observed = inspected.value();
    const clonecore::Status selection_status =
        validate_boot_repair_selection(
            request.expected,
            observed.selection,
            request.target.firmware,
            request.confirmation);
    if (!selection_status) {
      return clonecore::Result<StandaloneBootRepairReport>::failure(
          selection_status.error());
    }

    std::optional<TemporarySystemVolumeMount> temporary_mount;
    std::wstring system_root = normalized_root(request.target.system_root);
    if (observed.temporary_mount.has_value()) {
      if (mount_api_ == nullptr) {
        return clonecore::Result<StandaloneBootRepairReport>::failure(
            repair_error(
                clonecore::ErrorCode::internal_error,
                ERROR_INVALID_STATE,
                L"一時システム領域のAPI初期化",
                L"一時割り当てAPIを初期化できませんでした"));
      }
      auto mounted = TemporarySystemVolumeMount::acquire(
          observed.temporary_mount.value(), *mount_api_);
      if (!mounted) {
        return clonecore::Result<StandaloneBootRepairReport>::failure(
            mounted.error());
      }
      temporary_mount.emplace(mounted.take_value());
      system_root = temporary_mount->root();
    }

    auto bcdboot = execute_bcdboot_with_windows_apis(BcdBootRequest{
        .target_windows_directory =
            normalized_root(request.target.windows_root) + L"Windows",
        .target_system_partition_root = system_root,
        .firmware = request.target.firmware,
        .store_policy = request.target.store_policy,
    });
    clonecore::Status store_status = clonecore::success_status();
    if (bcdboot) {
      store_status = verify_boot_store(system_root, request.target.firmware);
    }
    if (temporary_mount.has_value()) {
      const clonecore::Status release_status = temporary_mount->release();
      if (!release_status) {
        return clonecore::Result<StandaloneBootRepairReport>::failure(
            release_status.error());
      }
    }
    if (!bcdboot) {
      return clonecore::Result<StandaloneBootRepairReport>::failure(
          bcdboot.error());
    }
    if (!store_status) {
      return clonecore::Result<StandaloneBootRepairReport>::failure(
          store_status.error());
    }
    return clonecore::Result<StandaloneBootRepairReport>::success(
        StandaloneBootRepairReport{
            .repaired = std::move(observed.selection),
            .bcdboot = bcdboot.take_value(),
            .boot_store_verified = true,
            .system_partition_temporarily_mounted =
                observed.temporary_mount.has_value(),
            .temporary_mount_released =
                observed.temporary_mount.has_value(),
        });
  }

 private:
  clonecore::Result<InspectedBootRepairTarget> inspect_target(
      const BootRepairTargetRequest& request) {
    auto inventory = inventory_.enumerate();
    if (!inventory) {
      return clonecore::Result<InspectedBootRepairTarget>::failure(
          inventory.error());
    }
    const auto windows_volume = query_volume_location(request.windows_root);
    if (!windows_volume) {
      return clonecore::Result<InspectedBootRepairTarget>::failure(
          windows_volume.error());
    }
    BootRepairVolumeLocation system_volume;
    std::optional<TemporarySystemVolumeMountPlan> temporary_plan;
    if (request.auto_mount_system_partition) {
      if (!request.system_root.empty()) {
        return clonecore::Result<InspectedBootRepairTarget>::failure(
            repair_error(
                clonecore::ErrorCode::invalid_argument,
                ERROR_INVALID_PARAMETER,
                L"未割当システム領域の指定",
                L"自動検出時はシステム領域のドライブ文字を同時指定できません"));
      }
      const auto disk = std::find_if(
          inventory.value().disks.begin(),
          inventory.value().disks.end(),
          [&](const auto& candidate) {
            return candidate.disk_number == request.disk_number;
          });
      if (disk == inventory.value().disks.end()) {
        return clonecore::Result<InspectedBootRepairTarget>::failure(
            repair_error(
                clonecore::ErrorCode::invalid_argument,
                ERROR_NOT_FOUND,
                L"未割当システム領域の対象ディスク選択",
                L"指定された物理ディスクが見つかりません"));
      }
      auto volumes = enumerate_windows_boot_volumes_read_only();
      if (!volumes) {
        return clonecore::Result<InspectedBootRepairTarget>::failure(
            volumes.error());
      }
      auto plan = plan_temporary_system_volume_mount(
          *disk,
          request.firmware,
          volumes.value(),
          used_drive_letters() + request.windows_root);
      if (!plan) {
        return clonecore::Result<InspectedBootRepairTarget>::failure(
            plan.error());
      }
      system_volume = plan.value().expected_location;
      temporary_plan = plan.take_value();
    } else {
      const auto queried_system_volume =
          query_volume_location(request.system_root);
      if (!queried_system_volume) {
        return clonecore::Result<InspectedBootRepairTarget>::failure(
            queried_system_volume.error());
      }
      system_volume = queried_system_volume.value();
    }
    auto selection = evaluate_boot_repair_target(
        request,
        inventory.value(),
        windows_volume.value(),
        system_volume);
    if (!selection) {
      return clonecore::Result<InspectedBootRepairTarget>::failure(
          selection.error());
    }

    const std::wstring windows_root = normalized_root(request.windows_root);
    const clonecore::Status architecture =
        verify_offline_windows_amd64(windows_root);
    if (!architecture) {
      return clonecore::Result<InspectedBootRepairTarget>::failure(
          architecture.error());
    }
    const std::wstring loader =
        windows_root + L"Windows\\System32\\" +
        (request.firmware == BcdBootFirmware::uefi
             ? L"winload.efi"
             : L"winload.exe");
    const clonecore::Status loader_status = verify_regular_file(
        loader,
        L"起動修復対象Windowsブートローダー確認",
        L"対象Windowsに通常ファイルのブートローダーを確認できません");
    if (!loader_status) {
      return clonecore::Result<InspectedBootRepairTarget>::failure(
          loader_status.error());
    }
    return clonecore::Result<InspectedBootRepairTarget>::success(
        InspectedBootRepairTarget{
            .selection = selection.take_value(),
            .temporary_mount = std::move(temporary_plan),
        });
  }

  diskmodel::IDiskInventoryProvider& inventory_;
  std::unique_ptr<ISystemVolumeMountApi> mount_api_;
};

}  // namespace

clonecore::Result<BootRepairTargetSelection> evaluate_boot_repair_target(
    const BootRepairTargetRequest& request,
    const diskmodel::InventoryReport& inventory,
    const BootRepairVolumeLocation& windows_volume,
    const BootRepairVolumeLocation& system_volume) {
  const std::wstring windows_root = normalized_root(request.windows_root);
  const std::wstring system_root = normalized_root(request.system_root);
  const bool valid_system_target = request.auto_mount_system_partition
      ? system_root.empty()
      : is_drive_root(system_root);
  if (!is_drive_root(windows_root) || !valid_system_target ||
      (request.firmware != BcdBootFirmware::uefi &&
       request.firmware != BcdBootFirmware::bios) ||
      (request.firmware == BcdBootFirmware::uefi &&
       !request.auto_mount_system_partition &&
       windows_root[0] == system_root[0])) {
    return clonecore::Result<BootRepairTargetSelection>::failure(repair_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"単独起動修復の対象指定",
        L"UEFI/BIOSとWindows/システム領域のドライブ文字指定が不正です"));
  }
  if (!inventory.issues.empty()) {
    return clonecore::Result<BootRepairTargetSelection>::failure(repair_error(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"単独起動修復の全ディスク列挙",
        L"未解決の列挙診断があるため対象を確定できません"));
  }
  const auto disk = std::find_if(
      inventory.disks.begin(),
      inventory.disks.end(),
      [&](const auto& candidate) {
        return candidate.disk_number == request.disk_number;
      });
  if (disk == inventory.disks.end()) {
    return clonecore::Result<BootRepairTargetSelection>::failure(repair_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_NOT_FOUND,
        L"単独起動修復対象ディスクの選択",
        L"指定された物理ディスクが見つかりません"));
  }
  const diskmodel::PartitionStyle expected_style =
      request.firmware == BcdBootFirmware::uefi
      ? diskmodel::PartitionStyle::gpt
      : diskmodel::PartitionStyle::mbr;
  if (disk->partition_style != expected_style || disk->partitions.empty() ||
      disk->is_system_disk || !disk->read_only.has_value() ||
      !disk->removable.has_value() || disk->read_only.value() ||
      disk->removable.value()) {
    return clonecore::Result<BootRepairTargetSelection>::failure(repair_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"単独起動修復対象ディスクの安全属性",
        L"オフラインの固定ディスクで、指定ファームウェアに対応するGPT/MBR構成だけを修復できます"));
  }
  if (windows_volume.disk_number != request.disk_number ||
      system_volume.disk_number != request.disk_number ||
      !equals_case_insensitive(windows_volume.file_system, L"NTFS") ||
      (request.firmware == BcdBootFirmware::uefi
           ? !equals_case_insensitive(system_volume.file_system, L"FAT32")
           : !equals_case_insensitive(system_volume.file_system, L"NTFS"))) {
    return clonecore::Result<BootRepairTargetSelection>::failure(repair_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DRIVE,
        L"単独起動修復対象ボリュームの物理対応",
        L"Windows/システム領域が指定ディスクと想定ファイルシステムに一致しません"));
  }

  const auto matches_volume =
      [](const diskmodel::PartitionInfo& partition,
         const BootRepairVolumeLocation& volume) {
        return partition.offset_bytes == volume.starting_offset &&
               partition.size_bytes == volume.extent_length;
      };
  const auto windows_partition = std::find_if(
      disk->partitions.begin(),
      disk->partitions.end(),
      [&](const auto& partition) {
        if (!matches_volume(partition, windows_volume) ||
            partition.style != expected_style) {
          return false;
        }
        return request.firmware == BcdBootFirmware::bios ||
               equals_case_insensitive(partition.type, kBasicDataPartitionType);
      });
  const auto system_partition = std::find_if(
      disk->partitions.begin(),
      disk->partitions.end(),
      [&](const auto& partition) {
        if (!matches_volume(partition, system_volume) ||
            partition.style != expected_style) {
          return false;
        }
        return request.firmware == BcdBootFirmware::uefi
            ? equals_case_insensitive(partition.type, kEfiPartitionType)
            : partition.bootable;
      });
  const auto active_count = std::count_if(
      disk->partitions.begin(),
      disk->partitions.end(),
      [](const auto& partition) { return partition.bootable; });
  if (windows_partition == disk->partitions.end() ||
      system_partition == disk->partitions.end() ||
      (request.firmware == BcdBootFirmware::bios && active_count != 1)) {
    return clonecore::Result<BootRepairTargetSelection>::failure(repair_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"単独起動修復対象パーティションの種別検証",
        L"Windows領域とUEFI ESPまたは一意なBIOS Active領域を対応付けられません"));
  }

  auto identity =
      diskmodel::make_stable_disk_identity(*disk, disk->is_system_disk);
  if (!identity) {
    return clonecore::Result<BootRepairTargetSelection>::failure(
        identity.error());
  }
  return clonecore::Result<BootRepairTargetSelection>::success(
      BootRepairTargetSelection{
          .disk = *disk,
          .identity = identity.take_value(),
          .windows_partition = *windows_partition,
          .system_partition = *system_partition,
      });
}

std::wstring make_boot_repair_confirmation_token(
    const clonecore::StableDiskIdentity& identity,
    const BcdBootFirmware firmware) {
  std::wostringstream stream;
  stream << L"REPAIR BOOT "
         << (firmware == BcdBootFirmware::uefi ? L"UEFI " : L"BIOS ")
         << identity.model << L" ";
  for (const char character : identity.serial_suffix) {
    stream << static_cast<wchar_t>(static_cast<unsigned char>(character));
  }
  stream << L" " << identity.size_bytes;
  return stream.str();
}

clonecore::Status validate_boot_repair_selection(
    const BootRepairTargetSelection& expected,
    const BootRepairTargetSelection& observed,
    const BcdBootFirmware firmware,
    const clonecore::TargetConfirmation& confirmation) {
  const clonecore::Status identity_status = clonecore::validate_stable_identity(
      expected.identity, observed.identity, L"起動修復対象");
  if (!identity_status) {
    return identity_status;
  }
  if (!same_partition(
          expected.windows_partition, observed.windows_partition) ||
      !same_partition(expected.system_partition, observed.system_partition)) {
    return clonecore::Status::failure(repair_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"起動修復対象パーティションの再識別",
        L"確認時と実行直前のWindowsまたはシステム領域が一致しません"));
  }
  if (!confirmation.first_step_acknowledged ||
      confirmation.typed_token !=
          make_boot_repair_confirmation_token(observed.identity, firmware)) {
    return clonecore::Status::failure(repair_error(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"起動ファイル変更の二段階確認",
        L"確認操作または対象固有の入力確認文字列が一致しません"));
  }
  return clonecore::success_status();
}

std::unique_ptr<IStandaloneBootRepairService>
make_windows_standalone_boot_repair_service(
    diskmodel::IDiskInventoryProvider& inventory) {
  return std::make_unique<WindowsStandaloneBootRepairService>(inventory);
}

}  // namespace ytec::bootrepair
