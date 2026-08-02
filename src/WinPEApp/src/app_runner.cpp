#include "ytec/winpeapp/app_runner.h"

#include "ytec/clitools/cli_runner.h"
#include "ytec/clonecore/disk_identity.h"
#include "ytec/diskmodel/inventory_formatter.h"
#include "ytec/winpeapp/clone_execution_readiness.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace ytec::winpeapp {
namespace {

constexpr clitools::InventoryCliPresentation kWinPePresentation{
    .title = "Y-TEC Tsumugi Drive WinPE ディスク診断（読み取り専用）",
    .executable_name = "ytec-winpe-app",
};

enum class OutputFormat : std::uint8_t {
  text,
  json,
};

struct ClonePreflightArguments final {
  std::uint32_t source_disk_number{};
  std::uint32_t target_disk_number{};
  OutputFormat format{OutputFormat::text};
};

struct CloneExecuteArguments final {
  std::uint32_t source_disk_number{};
  std::uint32_t target_disk_number{};
  OutputFormat format{OutputFormat::text};
  bool erasure_acknowledged{};
  std::wstring confirmation;
  std::wstring authorization;
};

struct ClonePreflightReport final {
  diskmodel::DiskInfo source;
  diskmodel::DiskInfo target;
  clonecore::StableDiskIdentity source_identity;
  clonecore::StableDiskIdentity target_identity;
  std::wstring confirmation_token;
};

struct BootRepairArguments final {
  bootrepair::BootRepairTargetRequest request;
  OutputFormat format{OutputFormat::text};
  bool execute{};
  bool change_acknowledged{};
  std::wstring confirmation;
};

struct WinReDiagnosticArguments final {
  std::uint32_t disk_number{};
  std::wstring windows_root;
  OutputFormat format{OutputFormat::text};
};

struct JobPreflightArguments final {
  std::wstring job_path;
  std::optional<std::wstring> restore_image_path_override;
  bool search_restore_image{};
  OutputFormat format{OutputFormat::text};
};

struct JobExecuteArguments final {
  JobPreflightArguments preflight;
  bool erasure_acknowledged{};
  std::wstring confirmation;
};

struct JobPreflightReport final {
  imageformat::VerifiedJobManifest verified_job;
  std::optional<imageformat::RestoreImageInspectionReport>
      verified_restore_image;
  std::optional<RestoreExecutionReadinessReport>
      restore_execution_readiness;
  bool restore_image_auto_located{};
  std::optional<diskmodel::DiskInfo> source;
  std::optional<diskmodel::DiskInfo> target;
};

void write_usage(
    std::ostream& stream,
    const bool execution_available,
    const bool boot_repair_available,
    const bool job_manifest_available,
    const bool restore_execution_available,
    const bool clone_job_execution_available,
    const bool winre_diagnostic_available,
    const bool mbr2gpt_job_execution_available = false) {
  stream
      << "Y-TEC Tsumugi Drive WinPE ディスク操作\n"
         "使い方:\n"
         "  ytec-winpe-app [--text | --json]\n"
         "  ytec-winpe-app --clone-preflight --source N --target N "
         "[--text | --json]\n"
         "  ytec-winpe-app --help\n";
  if (job_manifest_available) {
    stream
        << "  ytec-winpe-app --job-preflight --job-path X:\\path\\job.json "
           "[--image-path Y:\\path\\image.dcimg | --search-image] "
           "[--text | --json]\n";
  }
  if (restore_execution_available || clone_job_execution_available ||
      mbr2gpt_job_execution_available) {
    stream
        << "  ytec-winpe-app --job-execute --job-path X:\\path\\job.json "
           "[--image-path Y:\\path\\image.dcimg | --search-image]\n"
           "      --acknowledge-target-erasure --confirmation TEXT "
           "[--text | --json]\n";
  }
  if (execution_available) {
    stream
        << "  ytec-winpe-app --clone-execute --source N --target N\n"
           "      --acknowledge-target-erasure --confirmation TEXT\n"
           "      --authorization TEXT [--text | --json]\n";
  }
  if (boot_repair_available) {
    stream
        << "  ytec-winpe-app --boot-repair-preflight --disk N\n"
           "      --windows-root W:\\ (--system-root S:\\ | "
           "--auto-system-partition)\n"
           "      --firmware uefi|bios [--text | --json]\n"
           "  ytec-winpe-app --boot-repair-execute --disk N\n"
           "      --windows-root W:\\ (--system-root S:\\ | "
           "--auto-system-partition)\n"
           "      --firmware uefi|bios --acknowledge-boot-files-change\n"
           "      --confirmation TEXT [--text | --json]\n";
  }
  if (winre_diagnostic_available) {
    stream
        << "  ytec-winpe-app --winre-diagnostic --disk N\n"
           "      --windows-root W:\\ [--text | --json]\n";
  }
  stream
      <<
         "\n"
         "--clone-preflight はコピー元・コピー先の選択条件と確認語を\n"
         "読み取り専用で表示します。ファイルシステム検査や書き込みは行いません。\n";
  if (execution_available) {
    stream
        << "--clone-execute は明示的に注入されたVM専用実行サービスだけを使用します。\n"
           "対象再識別、二段階確認、VM制限のいずれかが不一致なら停止します。\n";
  } else if (restore_execution_available || clone_job_execution_available ||
             mbr2gpt_job_execution_available) {
    stream
        << "直接クローン実行は無効です。製品版の書込み操作は検証済みの\n"
           "--job-executeだけが対象で、対象固有の二段階確認が必要です。\n";
  } else {
    stream << (boot_repair_available
                   ? "この実行ファイルにはクローン・復元実行機能はありません。\n"
                   : "この実行ファイルには書き込み・クローン・復元機能はありません。\n");
  }
  if (boot_repair_available) {
    stream
        << "--boot-repair-preflight は対象を読み取り専用検査します。\n"
           "--auto-system-partition は未割当ESP/Activeを診断時には変更せず特定し、\n"
           "実行中だけ一時ドライブ文字を割り当て、検証後に解除します。\n"
           "--boot-repair-execute は再検査と二段階確認後、Microsoft署名済み\n"
           "BCDBootで選択したWindowsの起動ファイルだけを再構築します。\n";
  } else {
    stream << "この実行ファイルでは単独起動修復も無効です。\n";
  }
  if (winre_diagnostic_available) {
    stream
        << "--winre-diagnostic は対象ディスクを再識別し、Microsoft署名済み\n"
           "REAgentCの/infoとWinre.wimを読み取り専用で確認します。\n"
           "WinREの登録変更、ディスク書込み、起動修復は行いません。\n";
  }
  if (job_manifest_available) {
    stream
        << "--job-preflight はジョブの正規形とSHA-256を検証してから、\n"
           "安定識別情報で現在のディスク番号を読み取り専用で再解決します。\n"
           "復元ジョブではdcimg全体、全チャンク、メタデータ、復元領域も\n"
           "同じ読み取り専用コマンド内で再検証します。ドライブ文字が変わった場合は\n"
           "--image-pathで再選択し、ジョブ記録済みの長さと全体SHA-256へ照合します。\n"
           "--search-imageは同じ相対パスのローカルドライブ候補だけを有界列挙し、\n"
           "完全検証と同じ指紋の一致後だけ自動再解決します。\n"
           "さらにBitLocker、基本ディスク、Storage Spaces、対応ファイルシステム、\n"
           "電源を三値判定し、不明な必須項目は実行可能と扱いません。\n";
  }
  if (restore_execution_available || clone_job_execution_available ||
      mbr2gpt_job_execution_available) {
    stream
        << "--job-execute はクローンまたは復元の予約ジョブ専用です。同じ呼出し内で\n"
           "ジョブ、対象ディスク、安全条件、二段階確認を再検証し、復元時はdcimgも\n"
           "完全検証します。すべて合格した場合だけ対象サービスへ渡し、サービス側でも\n"
            "書込み前に再識別します。MBRからGPT移行はMBRコピー後にMicrosoft署名済み\n"
            "MBR2GPTとBCDBootを使い、変換後GPT/ESP/MSR/Windows/BCDまで再検証します。\n"
            "イメージ作成ジョブはまだ実行しません。\n";
  }
}

bool is_help_argument(const std::wstring_view argument) {
  return argument == L"--help" || argument == L"-h" || argument == L"/?";
}

std::optional<std::uint32_t> parse_disk_number(const std::wstring& text) {
  if (text.empty() || text.size() > 10) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const wchar_t character : text) {
    if (character < L'0' || character > L'9') {
      return std::nullopt;
    }
    value = value * 10U + static_cast<std::uint64_t>(character - L'0');
    if (value > (std::numeric_limits<std::uint32_t>::max)()) {
      return std::nullopt;
    }
  }
  return static_cast<std::uint32_t>(value);
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

std::optional<ClonePreflightArguments> parse_clone_preflight_arguments(
    const std::vector<std::wstring>& arguments) {
  if (arguments.empty() || arguments.front() != L"--clone-preflight") {
    return std::nullopt;
  }
  std::optional<std::uint32_t> source;
  std::optional<std::uint32_t> target;
  OutputFormat format = OutputFormat::text;
  bool format_selected = false;

  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::wstring& argument = arguments[index];
    if (argument == L"--source" || argument == L"--target") {
      if (index + 1 >= arguments.size()) {
        return std::nullopt;
      }
      const auto disk_number = parse_disk_number(arguments[++index]);
      if (!disk_number.has_value()) {
        return std::nullopt;
      }
      auto& destination =
          argument == L"--source" ? source : target;
      if (destination.has_value()) {
        return std::nullopt;
      }
      destination = disk_number;
      continue;
    }
    if (argument == L"--text" || argument == L"--json") {
      const OutputFormat requested =
          argument == L"--json" ? OutputFormat::json : OutputFormat::text;
      if (format_selected && requested != format) {
        return std::nullopt;
      }
      format = requested;
      format_selected = true;
      continue;
    }
    return std::nullopt;
  }

  if (!source.has_value() || !target.has_value() || source == target) {
    return std::nullopt;
  }
  return ClonePreflightArguments{
      .source_disk_number = source.value(),
      .target_disk_number = target.value(),
      .format = format,
  };
}

std::optional<CloneExecuteArguments> parse_clone_execute_arguments(
    const std::vector<std::wstring>& arguments) {
  if (arguments.empty() || arguments.front() != L"--clone-execute") {
    return std::nullopt;
  }
  std::optional<std::uint32_t> source;
  std::optional<std::uint32_t> target;
  std::optional<std::wstring> confirmation;
  std::optional<std::wstring> authorization;
  OutputFormat format = OutputFormat::text;
  bool format_selected = false;
  bool erasure_acknowledged = false;

  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::wstring& argument = arguments[index];
    if (argument == L"--source" || argument == L"--target") {
      if (index + 1 >= arguments.size()) {
        return std::nullopt;
      }
      const auto disk_number = parse_disk_number(arguments[++index]);
      if (!disk_number.has_value()) {
        return std::nullopt;
      }
      auto& destination = argument == L"--source" ? source : target;
      if (destination.has_value()) {
        return std::nullopt;
      }
      destination = disk_number;
      continue;
    }
    if (argument == L"--confirmation" || argument == L"--authorization") {
      if (index + 1 >= arguments.size()) {
        return std::nullopt;
      }
      auto& destination = argument == L"--confirmation"
                              ? confirmation
                              : authorization;
      if (destination.has_value() || arguments[index + 1].empty()) {
        return std::nullopt;
      }
      destination = arguments[++index];
      continue;
    }
    if (argument == L"--acknowledge-target-erasure") {
      if (erasure_acknowledged) {
        return std::nullopt;
      }
      erasure_acknowledged = true;
      continue;
    }
    if (argument == L"--text" || argument == L"--json") {
      const OutputFormat requested =
          argument == L"--json" ? OutputFormat::json : OutputFormat::text;
      if (format_selected && requested != format) {
        return std::nullopt;
      }
      format = requested;
      format_selected = true;
      continue;
    }
    return std::nullopt;
  }

  if (!source.has_value() || !target.has_value() || source == target ||
      !erasure_acknowledged || !confirmation.has_value() ||
      !authorization.has_value()) {
    return std::nullopt;
  }
  return CloneExecuteArguments{
      .source_disk_number = source.value(),
      .target_disk_number = target.value(),
      .format = format,
      .erasure_acknowledged = true,
      .confirmation = confirmation.value(),
      .authorization = authorization.value(),
  };
}

std::optional<JobPreflightArguments> parse_job_preflight_arguments(
    const std::vector<std::wstring>& arguments) {
  if (arguments.empty() || arguments.front() != L"--job-preflight") {
    return std::nullopt;
  }
  std::optional<std::wstring> job_path;
  std::optional<std::wstring> restore_image_path_override;
  bool search_restore_image = false;
  OutputFormat format = OutputFormat::text;
  bool format_selected = false;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::wstring& argument = arguments[index];
    if (argument == L"--job-path" || argument == L"--image-path") {
      auto& destination =
          argument == L"--job-path"
              ? job_path
              : restore_image_path_override;
      if (destination.has_value() || index + 1 >= arguments.size() ||
          arguments[index + 1].empty()) {
        return std::nullopt;
      }
      destination = arguments[++index];
      continue;
    }
    if (argument == L"--search-image") {
      if (search_restore_image) {
        return std::nullopt;
      }
      search_restore_image = true;
      continue;
    }
    if (argument == L"--text" || argument == L"--json") {
      const OutputFormat requested =
          argument == L"--json" ? OutputFormat::json : OutputFormat::text;
      if (format_selected && requested != format) {
        return std::nullopt;
      }
      format = requested;
      format_selected = true;
      continue;
    }
    return std::nullopt;
  }
  if (!job_path.has_value() ||
      (search_restore_image &&
       restore_image_path_override.has_value())) {
    return std::nullopt;
  }
  return JobPreflightArguments{
      .job_path = std::move(job_path.value()),
      .restore_image_path_override =
          std::move(restore_image_path_override),
      .search_restore_image = search_restore_image,
      .format = format,
  };
}

std::optional<JobExecuteArguments> parse_job_execute_arguments(
    const std::vector<std::wstring>& arguments) {
  if (arguments.empty() || arguments.front() != L"--job-execute") {
    return std::nullopt;
  }
  std::optional<std::wstring> job_path;
  std::optional<std::wstring> restore_image_path_override;
  std::optional<std::wstring> confirmation;
  bool search_restore_image = false;
  bool erasure_acknowledged = false;
  OutputFormat format = OutputFormat::text;
  bool format_selected = false;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::wstring& argument = arguments[index];
    if (argument == L"--job-path" || argument == L"--image-path" ||
        argument == L"--confirmation") {
      if (index + 1 >= arguments.size() ||
          arguments[index + 1].empty()) {
        return std::nullopt;
      }
      if (argument == L"--job-path") {
        if (job_path.has_value()) {
          return std::nullopt;
        }
        job_path = arguments[++index];
      } else if (argument == L"--image-path") {
        if (restore_image_path_override.has_value()) {
          return std::nullopt;
        }
        restore_image_path_override = arguments[++index];
      } else {
        if (confirmation.has_value()) {
          return std::nullopt;
        }
        confirmation = arguments[++index];
      }
      continue;
    }
    if (argument == L"--search-image") {
      if (search_restore_image) {
        return std::nullopt;
      }
      search_restore_image = true;
      continue;
    }
    if (argument == L"--acknowledge-target-erasure") {
      if (erasure_acknowledged) {
        return std::nullopt;
      }
      erasure_acknowledged = true;
      continue;
    }
    if (argument == L"--text" || argument == L"--json") {
      const OutputFormat requested =
          argument == L"--json" ? OutputFormat::json : OutputFormat::text;
      if (format_selected && requested != format) {
        return std::nullopt;
      }
      format = requested;
      format_selected = true;
      continue;
    }
    return std::nullopt;
  }
  if (!job_path.has_value() || !confirmation.has_value() ||
      !erasure_acknowledged ||
      (search_restore_image &&
       restore_image_path_override.has_value())) {
    return std::nullopt;
  }
  return JobExecuteArguments{
      .preflight =
          JobPreflightArguments{
              .job_path = std::move(job_path.value()),
              .restore_image_path_override =
                  std::move(restore_image_path_override),
              .search_restore_image = search_restore_image,
              .format = format,
          },
      .erasure_acknowledged = true,
      .confirmation = std::move(confirmation.value()),
  };
}

std::optional<BootRepairArguments> parse_boot_repair_arguments(
    const std::vector<std::wstring>& arguments,
    const bool execute) {
  const std::wstring_view expected_mode =
      execute ? L"--boot-repair-execute" : L"--boot-repair-preflight";
  if (arguments.empty() || arguments.front() != expected_mode) {
    return std::nullopt;
  }
  std::optional<std::uint32_t> disk;
  std::optional<std::wstring> windows_root;
  std::optional<std::wstring> system_root;
  std::optional<bootrepair::BcdBootFirmware> firmware;
  std::optional<std::wstring> confirmation;
  OutputFormat format = OutputFormat::text;
  bool format_selected = false;
  bool change_acknowledged = false;
  bool auto_system_partition = false;

  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::wstring& argument = arguments[index];
    if (argument == L"--disk") {
      if (disk.has_value() || index + 1 >= arguments.size()) {
        return std::nullopt;
      }
      disk = parse_disk_number(arguments[++index]);
      if (!disk.has_value()) {
        return std::nullopt;
      }
      continue;
    }
    if (argument == L"--windows-root" || argument == L"--system-root") {
      if (index + 1 >= arguments.size()) {
        return std::nullopt;
      }
      auto& destination =
          argument == L"--windows-root" ? windows_root : system_root;
      if (destination.has_value()) {
        return std::nullopt;
      }
      std::wstring root = normalized_root(arguments[++index]);
      if (!is_drive_root(root)) {
        return std::nullopt;
      }
      destination = std::move(root);
      continue;
    }
    if (argument == L"--firmware") {
      if (firmware.has_value() || index + 1 >= arguments.size()) {
        return std::nullopt;
      }
      const std::wstring& value = arguments[++index];
      if (value == L"uefi" || value == L"UEFI") {
        firmware = bootrepair::BcdBootFirmware::uefi;
      } else if (value == L"bios" || value == L"BIOS") {
        firmware = bootrepair::BcdBootFirmware::bios;
      } else {
        return std::nullopt;
      }
      continue;
    }
    if (argument == L"--auto-system-partition") {
      if (auto_system_partition) {
        return std::nullopt;
      }
      auto_system_partition = true;
      continue;
    }
    if (argument == L"--acknowledge-boot-files-change") {
      if (!execute || change_acknowledged) {
        return std::nullopt;
      }
      change_acknowledged = true;
      continue;
    }
    if (argument == L"--confirmation") {
      if (!execute || confirmation.has_value() ||
          index + 1 >= arguments.size() || arguments[index + 1].empty()) {
        return std::nullopt;
      }
      confirmation = arguments[++index];
      continue;
    }
    if (argument == L"--text" || argument == L"--json") {
      const OutputFormat requested =
          argument == L"--json" ? OutputFormat::json : OutputFormat::text;
      if (format_selected && requested != format) {
        return std::nullopt;
      }
      format = requested;
      format_selected = true;
      continue;
    }
    return std::nullopt;
  }

  if (!disk.has_value() || !windows_root.has_value() ||
      !firmware.has_value() ||
      (system_root.has_value() == auto_system_partition) ||
      (system_root.has_value() &&
       firmware.value() == bootrepair::BcdBootFirmware::uefi &&
       windows_root.value()[0] == system_root.value()[0]) ||
      (execute && (!change_acknowledged || !confirmation.has_value()))) {
    return std::nullopt;
  }
  return BootRepairArguments{
      .request =
          bootrepair::BootRepairTargetRequest{
              .disk_number = disk.value(),
              .windows_root = windows_root.value(),
              .system_root = system_root.value_or(L""),
              .firmware = firmware.value(),
              .auto_mount_system_partition = auto_system_partition,
          },
      .format = format,
      .execute = execute,
      .change_acknowledged = change_acknowledged,
      .confirmation = confirmation.value_or(L""),
  };
}

std::optional<WinReDiagnosticArguments>
parse_winre_diagnostic_arguments(
    const std::vector<std::wstring>& arguments) {
  if (arguments.empty() ||
      arguments.front() != L"--winre-diagnostic") {
    return std::nullopt;
  }
  std::optional<std::uint32_t> disk;
  std::optional<std::wstring> windows_root;
  OutputFormat format = OutputFormat::text;
  bool format_selected = false;

  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::wstring& argument = arguments[index];
    if (argument == L"--disk") {
      if (disk.has_value() || index + 1 >= arguments.size()) {
        return std::nullopt;
      }
      disk = parse_disk_number(arguments[++index]);
      if (!disk.has_value()) {
        return std::nullopt;
      }
      continue;
    }
    if (argument == L"--windows-root") {
      if (windows_root.has_value() ||
          index + 1 >= arguments.size()) {
        return std::nullopt;
      }
      std::wstring root = normalized_root(arguments[++index]);
      if (!is_drive_root(root)) {
        return std::nullopt;
      }
      windows_root = std::move(root);
      continue;
    }
    if (argument == L"--text" || argument == L"--json") {
      const OutputFormat requested =
          argument == L"--json" ? OutputFormat::json : OutputFormat::text;
      if (format_selected && requested != format) {
        return std::nullopt;
      }
      format = requested;
      format_selected = true;
      continue;
    }
    return std::nullopt;
  }

  if (!disk.has_value() || !windows_root.has_value()) {
    return std::nullopt;
  }
  return WinReDiagnosticArguments{
      .disk_number = disk.value(),
      .windows_root = std::move(windows_root.value()),
      .format = format,
  };
}

clonecore::Error preflight_error(
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

clonecore::Result<ClonePreflightReport> evaluate_clone_preflight(
    const ClonePreflightArguments& arguments,
    const diskmodel::InventoryReport& inventory) {
  if (!inventory.issues.empty()) {
    return clonecore::Result<ClonePreflightReport>::failure(preflight_error(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"クローンプリフライトの全ディスク列挙",
        L"未解決の列挙診断があるため対象を選択できません"));
  }

  const auto source = std::find_if(
      inventory.disks.begin(),
      inventory.disks.end(),
      [&](const auto& disk) {
        return disk.disk_number == arguments.source_disk_number;
      });
  const auto target = std::find_if(
      inventory.disks.begin(),
      inventory.disks.end(),
      [&](const auto& disk) {
        return disk.disk_number == arguments.target_disk_number;
      });
  if (source == inventory.disks.end() || target == inventory.disks.end()) {
    return clonecore::Result<ClonePreflightReport>::failure(preflight_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_NOT_FOUND,
        L"クローン対象ディスクの選択",
        L"指定したコピー元またはコピー先ディスクが見つかりません"));
  }
  if (source->is_system_disk || target->is_system_disk) {
    return clonecore::Result<ClonePreflightReport>::failure(preflight_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"WinPEオフライン実行の確認",
        L"実行中システムに所属するディスクはPhase 1の対象にできません"));
  }
  if (source->partition_style != diskmodel::PartitionStyle::gpt ||
      source->partitions.empty()) {
    return clonecore::Result<ClonePreflightReport>::failure(preflight_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"コピー元GPT構成の確認",
        L"Phase 1はパーティションを持つGPTコピー元だけを対象にします"));
  }
  if (target->partition_style != diskmodel::PartitionStyle::raw ||
      !target->partitions.empty()) {
    return clonecore::Result<ClonePreflightReport>::failure(preflight_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"コピー先RAW構成の確認",
        L"現段階の製品プリフライトは空のRAWコピー先だけを対象にします"));
  }
  if (!target->offline.has_value() || !target->read_only.has_value() ||
      !target->removable.has_value()) {
    return clonecore::Result<ClonePreflightReport>::failure(preflight_error(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"コピー先属性の確認",
        L"offline、read-only、removable属性をすべて取得できません"));
  }
  if (target->read_only.value() || target->removable.value()) {
    return clonecore::Result<ClonePreflightReport>::failure(preflight_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"コピー先属性の確認",
        L"現段階では読取り専用またはremovableのコピー先を対象にしません"));
  }
  if (source->logical_sector_size != 512 ||
      target->logical_sector_size != 512) {
    return clonecore::Result<ClonePreflightReport>::failure(preflight_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"4Kn有効化ゲート",
        L"実機検証前のPhase 1は512バイト論理セクターだけを対象にします"));
  }

  auto source_identity =
      diskmodel::make_stable_disk_identity(*source, source->is_system_disk);
  if (!source_identity) {
    return clonecore::Result<ClonePreflightReport>::failure(
        source_identity.error());
  }
  auto target_identity =
      diskmodel::make_stable_disk_identity(*target, target->is_system_disk);
  if (!target_identity) {
    return clonecore::Result<ClonePreflightReport>::failure(
        target_identity.error());
  }
  const clonecore::Status selection_status =
      clonecore::validate_clone_selection(
          source_identity.value(),
          source_identity.value(),
          target_identity.value(),
          target_identity.value());
  if (!selection_status) {
    return clonecore::Result<ClonePreflightReport>::failure(
        selection_status.error());
  }

  const std::wstring confirmation =
      clonecore::make_target_confirmation_token(target_identity.value());
  return clonecore::Result<ClonePreflightReport>::success(
      ClonePreflightReport{
          .source = *source,
          .target = *target,
          .source_identity = source_identity.take_value(),
          .target_identity = target_identity.take_value(),
          .confirmation_token = confirmation,
      });
}

clonecore::Result<diskmodel::DiskInfo> resolve_job_disk(
    const clonecore::StableDiskIdentity& expected,
    const diskmodel::InventoryReport& inventory,
    const std::wstring_view role) {
  std::optional<diskmodel::DiskInfo> match;
  for (const auto& disk : inventory.disks) {
    auto identity =
        diskmodel::make_stable_disk_identity(disk, disk.is_system_disk);
    if (!identity) {
      return clonecore::Result<diskmodel::DiskInfo>::failure(
          identity.error());
    }
    const auto status =
        clonecore::validate_stable_identity(
            expected, identity.value(), role);
    if (!status) {
      continue;
    }
    if (match.has_value()) {
      return clonecore::Result<diskmodel::DiskInfo>::failure(
          preflight_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DUP_NAME,
              std::wstring(role) + L"のWinPE再解決",
              L"同じ安定識別情報に一致するディスクが複数あります"));
    }
    match = disk;
  }
  if (!match.has_value()) {
    return clonecore::Result<diskmodel::DiskInfo>::failure(
        preflight_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_NOT_CONNECTED,
            std::wstring(role) + L"のWinPE再解決",
            L"ジョブ作成時の安定識別情報に一致するディスクがありません"));
  }
  return clonecore::Result<diskmodel::DiskInfo>::success(
      std::move(match.value()));
}

clonecore::Status validate_job_target_observation(
    const diskmodel::DiskInfo& target) {
  if (target.is_system_disk) {
    return clonecore::Status::failure(preflight_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"WinPEジョブのコピー先保護",
        L"実行中システムのディスクはコピー先にできません"));
  }
  if (!target.offline.has_value() || !target.read_only.has_value() ||
      !target.removable.has_value()) {
    return clonecore::Status::failure(preflight_error(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"WinPEジョブのコピー先属性",
        L"offline、read-only、removable属性をすべて取得できません"));
  }
  if (target.read_only.value() || target.removable.value()) {
    return clonecore::Status::failure(preflight_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"WinPEジョブのコピー先属性",
        L"読み取り専用またはremovableのディスクはコピー先にできません"));
  }
  if (target.logical_sector_size != 512) {
    return clonecore::Status::failure(preflight_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"WinPEジョブの4Kn有効化ゲート",
        L"実機相当検証までは512バイト論理セクターだけを対象にします"));
  }
  return clonecore::success_status();
}

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

clonecore::Status validate_restore_image_identity(
    const imageformat::JobManifest& manifest,
    const imageformat::RestoreImageInspectionReport& image) {
  if (!manifest.restore_image_identity.has_value()) {
    return clonecore::Status::failure(preflight_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"WinPE復元イメージ識別情報",
        L"復元ジョブに検証済みイメージの識別情報がありません"));
  }
  if (manifest.restore_image_identity->length_bytes !=
          image.image_length ||
      manifest.restore_image_identity->global_hash != image.global_hash) {
    return clonecore::Status::failure(preflight_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_CRC,
        L"WinPE復元イメージ再選択",
        L"選択されたdcimgは復元ジョブ作成時に検証したイメージと一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_restore_image_target(
    const imageformat::RestoreImageInspectionReport& image,
    const diskmodel::DiskInfo& target) {
  if (!image.complete_container_verified ||
      !image.metadata_verified ||
      !image.restore_layout_verified) {
    return clonecore::Status::failure(preflight_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"WinPE復元イメージ完全検証",
        L"dcimgのコンテナ、メタデータ、または復元領域の検証が完了していません"));
  }
  if (target.size_bytes < image.header.source_disk_size) {
    return clonecore::Status::failure(preflight_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_DISK_FULL,
        L"WinPE復元先容量",
        L"復元先ディスクがイメージ元ディスクより小さいため進めません"));
  }
  if (target.logical_sector_size != image.header.logical_sector_size) {
    return clonecore::Status::failure(preflight_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"WinPE復元先論理セクター",
        L"復元先とイメージの論理セクターサイズが一致しません"));
  }
  auto target_identity = diskmodel::make_stable_disk_identity(
      target, target.is_system_disk);
  if (!target_identity) {
    return clonecore::Status::failure(target_identity.error());
  }
  if (identifies_same_device(
          image.manifest.source, target_identity.value())) {
    return clonecore::Status::failure(preflight_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DRIVE,
        L"WinPE復元元・復元先分離",
        L"イメージに記録された元ディスクと同じディスクは復元先にできません"));
  }
  return clonecore::success_status();
}

clonecore::Result<JobPreflightReport> evaluate_job_preflight(
    imageformat::VerifiedJobManifest verified_job,
    std::optional<imageformat::RestoreImageInspectionReport>
        verified_restore_image,
    const diskmodel::InventoryReport& inventory) {
  if (!inventory.issues.empty()) {
    return clonecore::Result<JobPreflightReport>::failure(
        preflight_error(
            clonecore::ErrorCode::query_failed,
            ERROR_INVALID_DATA,
            L"WinPEジョブの全ディスク列挙",
            L"未解決の列挙診断があるためジョブを再解決できません"));
  }

  std::optional<diskmodel::DiskInfo> source;
  std::optional<diskmodel::DiskInfo> target;
  if (verified_job.manifest.source.has_value()) {
    auto resolved = resolve_job_disk(
        *verified_job.manifest.source, inventory, L"コピー元");
    if (!resolved) {
      return clonecore::Result<JobPreflightReport>::failure(
          resolved.error());
    }
    source = resolved.take_value();
  }
  if (verified_job.manifest.target.has_value()) {
    auto resolved = resolve_job_disk(
        *verified_job.manifest.target, inventory, L"コピー先");
    if (!resolved) {
      return clonecore::Result<JobPreflightReport>::failure(
          resolved.error());
    }
    target = resolved.take_value();
    const auto target_status =
        validate_job_target_observation(*target);
    if (!target_status) {
      return clonecore::Result<JobPreflightReport>::failure(
          target_status.error());
    }
  }

  const auto type = verified_job.manifest.job_type;
  if (type == imageformat::JobType::restore_image) {
    if (!verified_restore_image.has_value() ||
        !target.has_value()) {
      return clonecore::Result<JobPreflightReport>::failure(
          preflight_error(
              clonecore::ErrorCode::internal_error,
              ERROR_INVALID_STATE,
              L"WinPE復元ジョブ前提",
              L"復元イメージまたは復元先の検証結果がありません"));
    }
    const auto restore_status = validate_restore_image_target(
        verified_restore_image.value(), target.value());
    if (!restore_status) {
      return clonecore::Result<JobPreflightReport>::failure(
          restore_status.error());
    }
  }
  if (type == imageformat::JobType::clone ||
      type == imageformat::JobType::mbr_to_gpt) {
    if (!source.has_value() || !target.has_value() ||
        !verified_job.manifest.source.has_value() ||
        !verified_job.manifest.target.has_value()) {
      return clonecore::Result<JobPreflightReport>::failure(
          preflight_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"WinPEクローンジョブ前提",
              L"コピー元とコピー先の安定識別情報が必要です"));
    }
    auto observed_source = diskmodel::make_stable_disk_identity(
        *source, source->is_system_disk);
    auto observed_target = diskmodel::make_stable_disk_identity(
        *target, target->is_system_disk);
    if (!observed_source) {
      return clonecore::Result<JobPreflightReport>::failure(
          observed_source.error());
    }
    if (!observed_target) {
      return clonecore::Result<JobPreflightReport>::failure(
          observed_target.error());
    }
    const auto selection = clonecore::validate_clone_selection(
        *verified_job.manifest.source,
        observed_source.value(),
        *verified_job.manifest.target,
        observed_target.value());
    if (!selection) {
      return clonecore::Result<JobPreflightReport>::failure(
          selection.error());
    }
    if (source->partitions.empty() ||
        (source->partition_style != diskmodel::PartitionStyle::gpt &&
         source->partition_style != diskmodel::PartitionStyle::mbr)) {
      return clonecore::Result<JobPreflightReport>::failure(
          preflight_error(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"WinPEジョブのコピー元構成",
              L"パーティションを持つGPTまたはMBRコピー元が必要です"));
    }
    if (type == imageformat::JobType::mbr_to_gpt) {
      if (source->partition_style != diskmodel::PartitionStyle::mbr) {
        return clonecore::Result<JobPreflightReport>::failure(
            preflight_error(
                clonecore::ErrorCode::unsupported_layout,
                ERROR_NOT_SUPPORTED,
                L"WinPE MBRからGPTジョブ",
                L"MBRからGPTジョブのコピー元がMBRではありません"));
      }
      if (verified_job.manifest.requested_conversion !=
          imageformat::RequestedConversion::mbr_to_gpt) {
        return clonecore::Result<JobPreflightReport>::failure(
            preflight_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DATA,
                L"WinPE MBRからGPTジョブの変換指定",
                L"MBRからGPTジョブには対応する変換指定が必要です"));
      }
      if (verified_job.manifest.execution_mode !=
          imageformat::JobExecutionMode::review_required) {
        return clonecore::Result<JobPreflightReport>::failure(
            preflight_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_ACCESS_DENIED,
                L"WinPE MBRからGPTジョブの実行方式",
                L"MBRからGPT移行はWinPE画面での手動確認だけを許可します"));
      }
    }
    if (type == imageformat::JobType::clone) {
      if (verified_job.manifest.requested_conversion !=
          imageformat::RequestedConversion::preserve) {
        return clonecore::Result<JobPreflightReport>::failure(
            preflight_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DATA,
                L"WinPEクローンジョブの変換指定",
                L"通常クローンではパーティション形式を維持する指定が必要です"));
      }
    }
    const auto readiness =
        validate_clone_execution_observation(*source, *target);
    if (!readiness) {
      return clonecore::Result<JobPreflightReport>::failure(
          readiness.error());
    }
  }

  return clonecore::Result<JobPreflightReport>::success(
      JobPreflightReport{
          .verified_job = std::move(verified_job),
          .verified_restore_image =
              std::move(verified_restore_image),
          .source = std::move(source),
          .target = std::move(target),
      });
}

clonecore::Result<JobPreflightReport> perform_job_preflight(
    const JobPreflightArguments& arguments,
    diskmodel::IDiskInventoryProvider& provider,
    IJobManifestLoader& job_manifest_loader,
    IRestoreImageVerifier* const restore_image_verifier,
    IRestoreExecutionSafetyProbe* const restore_safety_probe,
    IRestoreImageCandidateProvider* const restore_image_candidates) {
  auto bytes = job_manifest_loader.load(arguments.job_path);
  if (!bytes) {
    return clonecore::Result<JobPreflightReport>::failure(bytes.error());
  }
  auto verified =
      imageformat::parse_and_verify_hashed_job_manifest(bytes.value());
  if (!verified) {
    return clonecore::Result<JobPreflightReport>::failure(
        verified.error());
  }
  if ((arguments.restore_image_path_override.has_value() ||
       arguments.search_restore_image) &&
      verified.value().manifest.job_type !=
          imageformat::JobType::restore_image) {
    return clonecore::Result<JobPreflightReport>::failure(
        preflight_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"WinPE復元イメージ再選択",
            L"--image-pathと--search-imageは復元ジョブだけで指定できます"));
  }

  std::optional<imageformat::RestoreImageInspectionReport>
      verified_restore_image;
  if (verified.value().manifest.job_type ==
      imageformat::JobType::restore_image) {
    if (restore_image_verifier == nullptr) {
      return clonecore::Result<JobPreflightReport>::failure(
          preflight_error(
              clonecore::ErrorCode::unsupported_platform,
              ERROR_NOT_SUPPORTED,
              L"WinPE復元イメージ検証サービス",
              L"このビルドでは復元イメージの完全検証サービスが無効です"));
    }
    if (arguments.search_restore_image) {
      if (restore_image_candidates == nullptr) {
        return clonecore::Result<JobPreflightReport>::failure(
            preflight_error(
                clonecore::ErrorCode::unsupported_platform,
                ERROR_NOT_SUPPORTED,
                L"WinPE復元イメージ候補列挙サービス",
                L"このビルドでは復元イメージの自動再解決が無効です"));
      }
      auto candidates = restore_image_candidates->candidates_for(
          verified.value().manifest.image_path);
      if (!candidates) {
        return clonecore::Result<JobPreflightReport>::failure(
            candidates.error());
      }
      constexpr std::size_t kMaximumRestoreImageCandidates = 64;
      if (candidates.value().size() >
          kMaximumRestoreImageCandidates) {
        return clonecore::Result<JobPreflightReport>::failure(
            preflight_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_BUFFER_OVERFLOW,
                L"WinPE復元イメージ候補数",
                L"復元イメージ候補が安全上限を超えています"));
      }
      for (const auto& candidate : candidates.value()) {
        auto image = restore_image_verifier->verify(candidate);
        if (!image) {
          continue;
        }
        const auto identity = validate_restore_image_identity(
            verified.value().manifest, image.value());
        if (identity) {
          verified_restore_image = image.take_value();
          break;
        }
      }
      if (!verified_restore_image.has_value()) {
        return clonecore::Result<JobPreflightReport>::failure(
            preflight_error(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_NOT_FOUND,
                L"WinPE復元イメージ自動再解決",
                L"ジョブに記録された長さと全体SHA-256に一致するdcimgが見つかりません"));
      }
    } else {
      const std::wstring& image_path =
          arguments.restore_image_path_override.has_value()
              ? *arguments.restore_image_path_override
              : verified.value().manifest.image_path;
      auto image = restore_image_verifier->verify(image_path);
      if (!image) {
        return clonecore::Result<JobPreflightReport>::failure(
            image.error());
      }
      const auto image_identity = validate_restore_image_identity(
          verified.value().manifest, image.value());
      if (!image_identity) {
        return clonecore::Result<JobPreflightReport>::failure(
            image_identity.error());
      }
      verified_restore_image = image.take_value();
    }
  }

  auto inventory = provider.enumerate();
  if (!inventory) {
    return clonecore::Result<JobPreflightReport>::failure(
        inventory.error());
  }
  auto preflight = evaluate_job_preflight(
      verified.take_value(),
      std::move(verified_restore_image),
      inventory.value());
  if (!preflight) {
    return preflight;
  }
  JobPreflightReport report = preflight.take_value();
  report.restore_image_auto_located =
      arguments.search_restore_image;
  if (report.verified_job.manifest.job_type ==
      imageformat::JobType::restore_image) {
    RestoreExecutionSafetyObservation observation;
    if (restore_safety_probe != nullptr) {
      auto inspected = restore_safety_probe->inspect(
          *report.target, *report.verified_restore_image);
      if (!inspected) {
        return clonecore::Result<JobPreflightReport>::failure(
            inspected.error());
      }
      observation = inspected.take_value();
    }
    report.restore_execution_readiness =
        evaluate_restore_execution_readiness(observation);
  }
  return clonecore::Result<JobPreflightReport>::success(
      std::move(report));
}

std::string disk_summary_json(const diskmodel::DiskInfo& disk) {
  std::ostringstream stream;
  stream << "{\"diskNumber\":" << disk.disk_number << ",\"model\":\""
         << diskmodel::json_escape(diskmodel::to_utf8(disk.model))
         << "\",\"sizeBytes\":" << disk.size_bytes
         << ",\"logicalSectorSize\":" << disk.logical_sector_size
         << ",\"busType\":\""
         << diskmodel::json_escape(diskmodel::to_utf8(disk.bus_type))
         << "\",\"serialSuffix\":\""
         << diskmodel::json_escape(disk.serial_suffix)
         << "\",\"partitionStyle\":\""
         << diskmodel::json_escape(
                diskmodel::to_utf8(
                    diskmodel::partition_style_name(disk.partition_style)))
         << "\",\"partitionCount\":" << disk.partitions.size() << '}';
  return stream.str();
}

std::string job_hash_hex(const imageformat::Sha256Digest& digest) {
  constexpr char kHexDigits[] = "0123456789ABCDEF";
  std::string value;
  value.reserve(digest.size() * 2);
  for (const std::byte byte : digest) {
    const auto numeric = std::to_integer<unsigned int>(byte);
    value.push_back(kHexDigits[(numeric >> 4U) & 0x0FU]);
    value.push_back(kHexDigits[numeric & 0x0FU]);
  }
  return value;
}

std::string_view restore_safety_state_label_ja(
    const RestoreSafetyState state) {
  switch (state) {
    case RestoreSafetyState::passed:
      return "合格";
    case RestoreSafetyState::blocked:
      return "危険";
    case RestoreSafetyState::unknown:
      return "不明";
  }
  return "不明";
}

std::string format_job_preflight_json(
    const JobPreflightReport& report) {
  const auto& manifest = report.verified_job.manifest;
  std::ostringstream stream;
  stream
      << "{\"schemaVersion\":7,\"mode\":\"job-preflight\","
         "\"readOnly\":true,\"jobHashVerified\":true,\"jobType\":\""
      << imageformat::job_type_name(manifest.job_type)
      << "\",\"executionMode\":\""
      << imageformat::job_execution_mode_name(manifest.execution_mode)
      << "\",\"jobHashSha256\":\""
      << job_hash_hex(report.verified_job.payload_hash)
      << "\",\"source\":";
  if (report.source.has_value()) {
    stream << disk_summary_json(*report.source);
  } else {
    stream << "null";
  }
  stream << ",\"target\":";
  if (report.target.has_value()) {
    stream << disk_summary_json(*report.target);
  } else {
    stream << "null";
  }
  stream
      << ",\"imagePathConfigured\":"
      << (manifest.image_path.empty() ? "false" : "true")
      << ",\"imageVerified\":"
      << (report.verified_restore_image.has_value()
              ? "true"
              : "false")
      << ",\"imageAutoLocated\":"
      << (report.restore_image_auto_located ? "true" : "false")
      << ",\"imageLengthBytes\":"
      << (report.verified_restore_image.has_value()
              ? report.verified_restore_image->image_length
              : 0)
      << ",\"imageSourceDiskBytes\":"
      << (report.verified_restore_image.has_value()
              ? report.verified_restore_image->header.source_disk_size
              : 0)
      << ",\"targetPartitionsToErase\":"
      << (report.target.has_value()
              ? report.target->partitions.size()
              : 0)
      << ",\"stableIdentityResolved\":true,"
         "\"restoreExecutionReadiness\":";
  if (report.restore_execution_readiness.has_value()) {
    const auto& readiness = *report.restore_execution_readiness;
    stream << "{\"requiredChecksPassed\":"
           << (readiness.required_checks_passed ? "true" : "false")
           << ",\"allChecksPassed\":"
           << (readiness.all_checks_passed ? "true" : "false")
           << ",\"checks\":[";
    for (std::size_t index = 0; index < readiness.checks.size(); ++index) {
      if (index != 0) {
        stream << ',';
      }
      const auto& finding = readiness.checks[index];
      stream << "{\"id\":\""
             << restore_safety_check_id(finding.check)
             << "\",\"required\":"
             << (finding.required ? "true" : "false")
             << ",\"state\":\""
             << restore_safety_state_name(finding.state) << "\"}";
    }
    stream << "]}";
  } else {
    stream << "null";
  }
  stream <<
         ",\"executionEnabled\":false}\n";
  return stream.str();
}

std::string format_job_preflight_text(
    const JobPreflightReport& report) {
  const auto& manifest = report.verified_job.manifest;
  std::ostringstream stream;
  stream
      << "Y-TEC Tsumugi Drive WinPEジョブ プリフライト"
         "（読み取り専用）\n"
      << "ジョブ種別: "
      << imageformat::job_type_name(manifest.job_type) << '\n'
      << "実行方式: "
      << imageformat::job_execution_mode_name(manifest.execution_mode)
      << '\n'
      << "ジョブSHA-256: "
      << job_hash_hex(report.verified_job.payload_hash) << '\n'
      << "正規形・ハッシュ検証: 成功\n"
         "安定識別によるディスク再解決: 成功\n";
  if (report.source.has_value()) {
    stream << "コピー元: Disk " << report.source->disk_number << " / "
           << diskmodel::to_utf8(report.source->model) << " / "
           << report.source->size_bytes << " bytes / serial "
           << report.source->serial_suffix << '\n';
  }
  if (report.target.has_value()) {
    stream << "コピー先: Disk " << report.target->disk_number << " / "
           << diskmodel::to_utf8(report.target->model) << " / "
           << report.target->size_bytes << " bytes / serial "
           << report.target->serial_suffix << '\n'
           << "実行時に消去対象となる区画数: "
           << report.target->partitions.size() << '\n';
  }
  stream
      << "イメージパス設定: "
      << (manifest.image_path.empty() ? "なし" : "あり（値は表示しません）")
      << '\n';
  if (report.verified_restore_image.has_value()) {
    stream
        << "dcimg完全検証: 成功（全体・全チャンク・メタデータ・復元領域）\n"
        << "イメージ容量: "
        << report.verified_restore_image->image_length << " bytes\n"
        << "イメージ元ディスク容量: "
        << report.verified_restore_image->header.source_disk_size
        << " bytes\n";
    stream << "イメージ自動再解決: "
           << (report.restore_image_auto_located ? "使用" : "未使用")
           << '\n';
  }
  if (report.restore_execution_readiness.has_value()) {
    stream << "復元実行直前の安全検査:\n";
    for (const auto& finding :
         report.restore_execution_readiness->checks) {
      stream << "  - "
             << restore_safety_check_label_ja(finding.check) << ": "
             << restore_safety_state_label_ja(finding.state)
             << (finding.required ? "（必須）" : "（警告）") << '\n';
    }
    stream << "必須検査合格: "
           << (report.restore_execution_readiness->required_checks_passed
                   ? "はい"
                   : "いいえ")
           << "\n全検査合格: "
           << (report.restore_execution_readiness->all_checks_passed
                   ? "はい"
                   : "いいえ")
           << '\n';
  } else {
    stream
        << "実行直前の詳細安全検査: このジョブ種別では未接続\n";
  }
  stream << "ジョブ実行: 無効（このコマンドは書き込みません）\n";
  return stream.str();
}

std::string format_preflight_json(const ClonePreflightReport& report) {
  std::ostringstream stream;
  stream << "{\"schemaVersion\":1,\"mode\":\"clone-preflight\","
            "\"readOnly\":true,\"scope\":\"disk-selection-only\","
            "\"eligible\":true,\"source\":"
         << disk_summary_json(report.source) << ",\"target\":"
         << disk_summary_json(report.target)
         << ",\"targetPartitionsToErase\":"
         << report.target.partitions.size() << ",\"confirmationToken\":\""
         << diskmodel::json_escape(
                diskmodel::to_utf8(report.confirmation_token))
         << "\",\"executionEnabled\":false}\n";
  return stream.str();
}

std::string format_preflight_text(const ClonePreflightReport& report) {
  std::ostringstream stream;
  stream
      << "Y-TEC WinPE GPT→GPTクローン プリフライト（読み取り専用）\n"
         "判定: ディスク選択条件を通過\n"
         "範囲: 対象選択だけ。GPT整合性、ファイルシステム、BitLocker、\n"
         "      使用クラスタは実行前の次段階で再検証します。\n\n"
         "コピー元\n"
      << "  Disk: " << report.source.disk_number << "\n"
      << "  モデル: " << diskmodel::to_utf8(report.source.model) << "\n"
      << "  容量: " << report.source.size_bytes << " bytes\n"
      << "  BusType: " << diskmodel::to_utf8(report.source.bus_type) << "\n"
      << "  シリアル末尾: " << report.source.serial_suffix << "\n"
      << "  パーティション数: " << report.source.partitions.size() << "\n\n"
         "コピー先（将来の実行時に消去される対象）\n"
      << "  Disk: " << report.target.disk_number << "\n"
      << "  モデル: " << diskmodel::to_utf8(report.target.model) << "\n"
      << "  容量: " << report.target.size_bytes << " bytes\n"
      << "  BusType: " << diskmodel::to_utf8(report.target.bus_type) << "\n"
      << "  シリアル末尾: " << report.target.serial_suffix << "\n"
      << "  削除対象パーティション数: " << report.target.partitions.size()
      << "\n\n二段階確認の入力語:\n  "
      << diskmodel::to_utf8(report.confirmation_token)
      << "\n\nクローン実行: 無効（このコマンドは書き込みません）\n";
  return stream.str();
}

std::string format_execution_json(const CloneExecutionReport& report) {
  const std::string_view partition_style =
      report.partition_style == ClonePartitionStyle::gpt ? "GPT" : "MBR";
  std::ostringstream stream;
  stream << "{\"schemaVersion\":3,\"mode\":\"clone-execution\"," 
            "\"result\":\"PASS\",\"partitionStyle\":\""
         << partition_style << "\",\"copiedDataBytes\":"
         << report.copied_data_bytes << ",\"copiedPartitions\":"
         << report.copied_partition_count << ",\"recreatedPartitions\":"
         << report.recreated_partition_count << ",\"readBackVerified\":"
         << (report.read_back_verified ? "true" : "false")
         << ",\"partitionTableCommitted\":"
         << (report.partition_table_committed ? "true" : "false")
         << ",\"targetReturnedOnline\":"
         << (report.target_returned_online ? "true" : "false")
         << ",\"bcdbootSignatureVerified\":"
         << (report.boot_repair.bcdboot.microsoft_signature_verified
                 ? "true"
                 : "false")
         << ",\"bootStoreVerified\":"
         << (report.boot_repair.boot_store_verified ? "true" : "false")
         << ",\"temporaryMountsReleased\":"
         << (report.temporary_mounts_released ? "true" : "false")
         << ",\"bootFinalizationVerified\":"
         << (report.boot_finalization_verified ? "true" : "false")
         << "}\n";
  return stream.str();
}

std::string format_execution_text(const CloneExecutionReport& report) {
  const std::string_view partition_style =
      report.partition_style == ClonePartitionStyle::gpt ? "GPT" : "MBR";
  std::ostringstream stream;
  stream << "Y-TEC WinPE オフラインクローン実行\n"
            "パーティション形式: "
         << partition_style << '\n'
         << "結果: PASS\n"
         << "コピー済みデータ: "
         << report.copied_data_bytes << " bytes\n"
         << "コピー区画数: " << report.copied_partition_count << '\n'
         << "再作成区画数: " << report.recreated_partition_count << '\n'
         << "読戻し検証: "
         << (report.read_back_verified ? "成功" : "失敗") << '\n'
      << "パーティションテーブル確定: "
         << (report.partition_table_committed ? "成功" : "失敗") << '\n'
         << "コピー先オンライン復帰: "
         << (report.target_returned_online ? "成功" : "失敗") << '\n'
         << "Microsoft BCDBootとBCDストア検証: "
         << (report.boot_finalization_verified ? "成功" : "失敗") << '\n'
         << "一時ドライブ文字の解除: "
         << (report.temporary_mounts_released ? "成功" : "失敗") << '\n';
  return stream.str();
}

clonecore::Status validate_clone_job_execution_report(
    const CloneExecutionReport& report,
    diskmodel::PartitionStyle expected_style);

clonecore::Status validate_mbr2gpt_job_execution_report(
    const Mbr2GptJobExecutionReport& report) {
  const auto clone_status = validate_clone_job_execution_report(
      report.clone, diskmodel::PartitionStyle::mbr);
  if (!clone_status) {
    return clone_status;
  }
  if (!report.conversion.microsoft_signature_verified ||
      !report.conversion.target_reidentified_before_conversion ||
      report.conversion.validation.exit_code != 0U ||
      report.conversion.conversion.exit_code != 0U ||
      !report.boot_repair.bcdboot.microsoft_signature_verified ||
      report.boot_repair.bcdboot.exit_code != 0U ||
      !report.boot_repair.boot_store_verified ||
      (report.boot_repair.system_partition_temporarily_mounted &&
       !report.boot_repair.temporary_mount_released) ||
      !report.source_reidentified_unchanged ||
      !report.target_reidentified_as_gpt ||
      !report.efi_system_partition_verified ||
      !report.microsoft_reserved_partition_verified ||
      !report.offline_windows_verified ||
      !report.temporary_windows_mount_released ||
      !report.final_layout_verified) {
    return clonecore::Status::failure(preflight_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"WinPE MBRからGPT移行結果",
        L"MBRコピー、Microsoft変換、GPT必須区画、Windows、BCD、または一時割当解除を完全確認できません"));
  }
  return clonecore::success_status();
}

std::string format_mbr2gpt_execution_json(
    const Mbr2GptJobExecutionReport& report) {
  std::ostringstream stream;
  stream
      << "{\"schemaVersion\":1,\"mode\":\"mbr-to-gpt-execution\"," 
         "\"result\":\"PASS\",\"sourceUnchanged\":"
      << (report.source_reidentified_unchanged ? "true" : "false")
      << ",\"mbrCloneReadBackVerified\":"
      << (report.clone.read_back_verified ? "true" : "false")
      << ",\"mbr2gptSignatureVerified\":"
      << (report.conversion.microsoft_signature_verified ? "true" : "false")
      << ",\"mbr2gptValidateExitCode\":"
      << report.conversion.validation.exit_code
      << ",\"mbr2gptConvertExitCode\":"
      << report.conversion.conversion.exit_code
      << ",\"targetIsGpt\":"
      << (report.target_reidentified_as_gpt ? "true" : "false")
      << ",\"efiSystemPartitionVerified\":"
      << (report.efi_system_partition_verified ? "true" : "false")
      << ",\"microsoftReservedPartitionVerified\":"
      << (report.microsoft_reserved_partition_verified ? "true" : "false")
      << ",\"offlineWindowsVerified\":"
      << (report.offline_windows_verified ? "true" : "false")
      << ",\"bcdbootSignatureVerified\":"
      << (report.boot_repair.bcdboot.microsoft_signature_verified
              ? "true"
              : "false")
      << ",\"bcdStoreVerified\":"
      << (report.boot_repair.boot_store_verified ? "true" : "false")
      << ",\"temporaryMountsReleased\":"
      << (report.temporary_windows_mount_released &&
                  (!report.boot_repair.system_partition_temporarily_mounted ||
                   report.boot_repair.temporary_mount_released)
              ? "true"
              : "false")
      << ",\"finalLayoutVerified\":"
      << (report.final_layout_verified ? "true" : "false")
      << ",\"nextAction\":\"disconnect-source-and-boot-target-in-uefi\"}\n";
  return stream.str();
}

std::string format_mbr2gpt_execution_text(
    const Mbr2GptJobExecutionReport& report) {
  std::ostringstream stream;
  stream
      << "Y-TEC Tsumugi Drive MBRからGPT / UEFI移行\n"
         "結果: PASS\n"
         "コピー元の再識別・MBR維持: 成功\n"
         "コピー先へのMBRクローン・読戻し: 成功\n"
         "Microsoft MBR2GPT署名検証: 成功\n"
         "MBR2GPT /validate: 成功 (exit "
      << report.conversion.validation.exit_code
      << ")\nMBR2GPT /convert: 成功 (exit "
      << report.conversion.conversion.exit_code
      << ")\nGPT / ESP / MSR / Windows再確認: 成功\n"
         "Microsoft BCDBootとBCDストア検証: 成功\n"
         "一時ドライブ文字の解除: 成功\n\n"
         "次の操作: PCを完全に終了し、コピー元ディスクを外してください。\n"
         "コピー先だけを接続し、ファームウェアをUEFI起動へ切り替えて起動してください。\n"
         "最初の単独起動が完了するまでコピー元を同時接続しないでください。\n";
  return stream.str();
}

clonecore::Status validate_restore_confirmation(
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::StableDiskIdentity& observed_target,
    const clonecore::TargetConfirmation& confirmation) {
  const auto identity_status = clonecore::validate_stable_identity(
      expected_target, observed_target, L"復元先");
  if (!identity_status) {
    return identity_status;
  }
  if (!confirmation.first_step_acknowledged ||
      confirmation.typed_token !=
          clonecore::make_target_confirmation_token(observed_target)) {
    return clonecore::Status::failure(preflight_error(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"WinPE復元の二段階確認",
        L"復元先消去への同意と対象固有の確認語が一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_restore_execution_report(
    const RestoreExecutionReport& report) {
  if (!report.complete_image_verified_before_write ||
      !report.backup_manifest_verified_before_write ||
      !report.read_back_verified ||
      !report.partition_table_committed ||
      !report.target_returned_online ||
      !report.boot_repair.bcdboot.microsoft_signature_verified ||
      report.boot_repair.bcdboot.exit_code != 0U ||
      !report.boot_repair.boot_store_verified ||
      (report.boot_repair.system_partition_temporarily_mounted &&
       !report.boot_repair.temporary_mount_released) ||
      !report.temporary_mounts_released ||
      !report.boot_finalization_verified) {
    return clonecore::Status::failure(preflight_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"WinPE復元実行結果",
        L"書込み前検証、読戻し検証、確定、オンライン復帰、起動再構築、または一時割当解除が完了していません"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_clone_job_execution_report(
    const CloneExecutionReport& report,
    const diskmodel::PartitionStyle expected_style) {
  const bool style_matches =
      (expected_style == diskmodel::PartitionStyle::gpt &&
       report.partition_style == ClonePartitionStyle::gpt) ||
      (expected_style == diskmodel::PartitionStyle::mbr &&
       report.partition_style == ClonePartitionStyle::mbr);
  if (!style_matches || report.copied_data_bytes == 0 ||
      report.copied_partition_count == 0 ||
      !report.read_back_verified ||
      !report.partition_table_committed ||
      !report.target_returned_online ||
      !report.boot_repair.bcdboot.microsoft_signature_verified ||
      report.boot_repair.bcdboot.exit_code != 0U ||
      !report.boot_repair.boot_store_verified ||
      (report.boot_repair.system_partition_temporarily_mounted &&
       !report.boot_repair.temporary_mount_released) ||
      !report.temporary_mounts_released ||
      !report.boot_finalization_verified) {
    return clonecore::Status::failure(preflight_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"WinPEクローン実行結果",
        L"形式維持、データコピー、読戻し検証、パーティション確定、オンライン復帰、起動再構築、または一時割当解除が完了していません"));
  }
  return clonecore::success_status();
}

std::string format_restore_execution_json(
    const RestoreExecutionReport& report) {
  std::ostringstream stream;
  stream
      << "{\"schemaVersion\":8,\"mode\":\"restore-execution\"," 
         "\"result\":\"PASS\",\"restoredDataBytes\":"
      << report.restored_data_bytes
      << ",\"committedPartitionTableBytes\":"
      << report.committed_partition_table_bytes
      << ",\"restoredChunks\":" << report.restored_chunk_count
      << ",\"completeImageVerifiedBeforeWrite\":"
      << (report.complete_image_verified_before_write ? "true" : "false")
      << ",\"backupManifestVerifiedBeforeWrite\":"
      << (report.backup_manifest_verified_before_write ? "true" : "false")
      << ",\"readBackVerified\":"
      << (report.read_back_verified ? "true" : "false")
      << ",\"partitionTableCommitted\":"
      << (report.partition_table_committed ? "true" : "false")
      << ",\"targetReturnedOnline\":"
      << (report.target_returned_online ? "true" : "false")
      << ",\"bcdbootSignatureVerified\":"
      << (report.boot_repair.bcdboot.microsoft_signature_verified
              ? "true"
              : "false")
      << ",\"bootStoreVerified\":"
      << (report.boot_repair.boot_store_verified ? "true" : "false")
      << ",\"temporaryMountsReleased\":"
      << (report.temporary_mounts_released ? "true" : "false")
      << ",\"bootFinalizationVerified\":"
      << (report.boot_finalization_verified ? "true" : "false")
      << "}\n";
  return stream.str();
}

std::string format_restore_execution_text(
    const RestoreExecutionReport& report) {
  std::ostringstream stream;
  stream
      << "Y-TEC Tsumugi Drive WinPE イメージ復元\n"
         "結果: PASS\n"
         "復元済みデータ: "
      << report.restored_data_bytes << " bytes\n"
      << "復元チャンク数: " << report.restored_chunk_count << '\n'
      << "確定済みパーティションテーブル: "
      << report.committed_partition_table_bytes << " bytes\n"
      << "書込み前dcimg完全検証: "
      << (report.complete_image_verified_before_write ? "成功" : "失敗")
      << '\n'
      << "書込み前バックアップ情報検証: "
      << (report.backup_manifest_verified_before_write ? "成功" : "失敗")
      << '\n'
      << "書込み後読戻し検証: "
      << (report.read_back_verified ? "成功" : "失敗") << '\n'
      << "パーティションテーブル確定: "
      << (report.partition_table_committed ? "成功" : "失敗") << '\n'
      << "復元先オンライン復帰: "
      << (report.target_returned_online ? "成功" : "失敗") << '\n'
      << "Microsoft BCDBootとBCDストア検証: "
      << (report.boot_finalization_verified ? "成功" : "失敗") << '\n'
      << "一時ドライブ文字の解除: "
      << (report.temporary_mounts_released ? "成功" : "失敗") << '\n';
  return stream.str();
}

std::string format_boot_repair_preflight_json(
    const bootrepair::BootRepairTargetRequest& request,
    const bootrepair::BootRepairTargetSelection& selection) {
  const std::string_view firmware =
      request.firmware == bootrepair::BcdBootFirmware::uefi ? "UEFI" : "BIOS";
  const std::wstring confirmation =
      bootrepair::make_boot_repair_confirmation_token(
          selection.identity, request.firmware);
  std::ostringstream stream;
  stream
      << "{\"schemaVersion\":3,\"mode\":\"boot-repair-preflight\","
         "\"readOnly\":true,\"eligible\":true,\"firmware\":\""
      << firmware << "\",\"disk\":" << disk_summary_json(selection.disk)
      << ",\"windowsRoot\":\""
      << diskmodel::json_escape(diskmodel::to_utf8(request.windows_root))
      << "\",\"systemRoot\":\""
      << diskmodel::json_escape(diskmodel::to_utf8(request.system_root))
      << "\",\"systemRootMode\":\""
      << (request.auto_mount_system_partition ? "auto-unassigned" : "assigned")
      << "\",\"windowsPartition\":"
      << selection.windows_partition.number << ",\"systemPartition\":"
      << selection.system_partition.number << ",\"confirmationToken\":\""
      << diskmodel::json_escape(diskmodel::to_utf8(confirmation))
      << "\",\"executionEnabled\":true}\n";
  return stream.str();
}

std::string format_boot_repair_preflight_text(
    const bootrepair::BootRepairTargetRequest& request,
    const bootrepair::BootRepairTargetSelection& selection) {
  const std::string_view firmware =
      request.firmware == bootrepair::BcdBootFirmware::uefi ? "UEFI" : "BIOS";
  const std::wstring confirmation =
      bootrepair::make_boot_repair_confirmation_token(
          selection.identity, request.firmware);
  std::ostringstream stream;
  stream
      << "Y-TEC Tsumugi Drive 起動修復プリフライト（読み取り専用）\n"
         "判定: 起動修復の対象条件を通過\n"
      << "ファームウェア: " << firmware << '\n'
      << "対象ディスク: " << selection.disk.disk_number << " / "
      << diskmodel::to_utf8(selection.disk.model) << " / "
      << selection.disk.size_bytes << " bytes\n"
      << "Windows領域: Partition "
      << selection.windows_partition.number << " / "
      << diskmodel::to_utf8(request.windows_root) << '\n'
      << "システム領域: Partition "
      << selection.system_partition.number << " / "
      << (request.auto_mount_system_partition
              ? "未割り当て領域を自動検出（診断中は割り当てなし）"
              : diskmodel::to_utf8(request.system_root))
      << "\n\n"
      << "変更内容: クローンや初期化は行わず、選択したシステム領域の\n"
         "          Windows起動ファイルとBCDだけを再構築します。\n"
      << "二段階確認の入力語:\n  "
      << diskmodel::to_utf8(confirmation)
      << "\n\n起動修復実行: まだ実行していません\n";
  return stream.str();
}

std::string format_boot_repair_execution_json(
    const bootrepair::BootRepairTargetRequest& request,
    const bootrepair::StandaloneBootRepairReport& report) {
  const std::string_view firmware =
      request.firmware == bootrepair::BcdBootFirmware::uefi ? "UEFI" : "BIOS";
  std::ostringstream stream;
  stream
      << "{\"schemaVersion\":4,\"mode\":\"boot-repair-execution\","
         "\"result\":\"PASS\",\"firmware\":\""
      << firmware << "\",\"diskNumber\":" << report.repaired.disk.disk_number
      << ",\"windowsPartition\":"
      << report.repaired.windows_partition.number
      << ",\"systemPartition\":" << report.repaired.system_partition.number
      << ",\"microsoftSignatureVerified\":"
      << (report.bcdboot.microsoft_signature_verified ? "true" : "false")
      << ",\"bootStoreVerified\":"
      << (report.boot_store_verified ? "true" : "false")
      << ",\"systemPartitionTemporarilyMounted\":"
      << (report.system_partition_temporarily_mounted ? "true" : "false")
      << ",\"temporaryMountReleased\":"
      << (report.temporary_mount_released ? "true" : "false")
      << ",\"exitCode\":" << report.bcdboot.exit_code << "}\n";
  return stream.str();
}

std::string format_boot_repair_execution_text(
    const bootrepair::BootRepairTargetRequest& request,
    const bootrepair::StandaloneBootRepairReport& report) {
  const std::string_view firmware =
      request.firmware == bootrepair::BcdBootFirmware::uefi ? "UEFI" : "BIOS";
  std::ostringstream stream;
  stream
      << "Y-TEC Tsumugi Drive 単独起動修復\n"
      << "結果: PASS\n"
      << "ファームウェア: " << firmware << '\n'
      << "対象ディスク: " << report.repaired.disk.disk_number << '\n'
      << "Windows領域: Partition "
      << report.repaired.windows_partition.number << '\n'
      << "システム領域: Partition "
      << report.repaired.system_partition.number << '\n'
      << "Microsoft署名検証: "
      << (report.bcdboot.microsoft_signature_verified ? "成功" : "失敗")
      << '\n'
      << "BCDストア再確認: "
      << (report.boot_store_verified ? "成功" : "失敗") << '\n'
      << "システム領域の一時割り当て: "
      << (report.system_partition_temporarily_mounted ? "実施" : "未実施")
      << '\n'
      << "一時割り当ての解除: "
      << (report.system_partition_temporarily_mounted
              ? (report.temporary_mount_released ? "成功" : "失敗")
              : "対象外")
      << '\n'
      << "BCDBoot終了コード: " << report.bcdboot.exit_code << '\n';
  return stream.str();
}

std::string_view winre_source_state_name(
    const bootrepair::WinReSourceState state) {
  switch (state) {
    case bootrepair::WinReSourceState::registered_partition:
      return "registered-partition";
    case bootrepair::WinReSourceState::image_available_in_windows:
      return "windows-fallback";
    case bootrepair::WinReSourceState::missing:
      return "missing";
    case bootrepair::WinReSourceState::unknown:
      return "unknown";
  }
  return "unknown";
}

std::string_view winre_source_state_label_ja(
    const bootrepair::WinReSourceState state) {
  switch (state) {
    case bootrepair::WinReSourceState::registered_partition:
      return "登録済み回復パーティション";
    case bootrepair::WinReSourceState::image_available_in_windows:
      return "Windows内のフォールバックイメージ";
    case bootrepair::WinReSourceState::missing:
      return "WinREイメージなし";
    case bootrepair::WinReSourceState::unknown:
      return "不明";
  }
  return "不明";
}

bool winre_diagnostic_complete(
    const bootrepair::WinReDiagnosticReport& report) {
  return report.microsoft_signature_verified &&
         report.read_only_command &&
         report.exit_code == 0U &&
         report.source_state !=
             bootrepair::WinReSourceState::unknown;
}

std::string format_winre_diagnostic_json(
    const WinReDiagnosticArguments& arguments,
    const diskmodel::DiskInfo& disk,
    const bootrepair::WinReDiagnosticReport& report) {
  std::ostringstream stream;
  stream
      << "{\"schemaVersion\":1,\"mode\":\"winre-diagnostic\","
         "\"readOnly\":"
      << (report.read_only_command ? "true" : "false")
      << ",\"diagnosticComplete\":"
      << (winre_diagnostic_complete(report) ? "true" : "false")
      << ",\"sourceState\":\""
      << winre_source_state_name(report.source_state)
      << "\",\"disk\":" << disk_summary_json(disk)
      << ",\"windowsRoot\":\""
      << diskmodel::json_escape(
             diskmodel::to_utf8(arguments.windows_root))
      << "\",\"reagentcExitCode\":" << report.exit_code
      << ",\"microsoftSignatureVerified\":"
      << (report.microsoft_signature_verified ? "true" : "false")
      << ",\"registeredLocationReported\":"
      << (report.registered_location_reported ? "true" : "false")
      << ",\"registeredLocationMatchesExpectedDisk\":"
      << (report.registered_location_matches_expected_disk
              ? "true"
              : "false")
      << ",\"registeredPartition\":";
  if (report.registered_partition_number == 0U) {
    stream << "null";
  } else {
    stream << report.registered_partition_number;
  }
  stream
      << ",\"registeredImagePresent\":"
      << (report.registered_image_present ? "true" : "false")
      << ",\"fallbackImagePresent\":"
      << (report.fallback_image_present ? "true" : "false")
      << ",\"winreImageSizeBytes\":"
      << report.winre_image_size_bytes
      << ",\"executionEnabled\":false}\n";
  return stream.str();
}

std::string format_winre_diagnostic_text(
    const WinReDiagnosticArguments& arguments,
    const diskmodel::DiskInfo& disk,
    const bootrepair::WinReDiagnosticReport& report) {
  std::ostringstream stream;
  stream
      << "Y-TEC Tsumugi Drive WinRE診断（読み取り専用）\n"
      << "診断完了: "
      << (winre_diagnostic_complete(report) ? "はい" : "いいえ")
      << '\n'
      << "対象ディスク: " << disk.disk_number << " / "
      << diskmodel::to_utf8(disk.model) << " / "
      << disk.size_bytes << " bytes\n"
      << "Windows領域: "
      << diskmodel::to_utf8(arguments.windows_root) << '\n'
      << "WinRE状態: "
      << winre_source_state_label_ja(report.source_state) << '\n'
      << "REAgentC終了コード: " << report.exit_code << '\n'
      << "Microsoft署名検証: "
      << (report.microsoft_signature_verified ? "成功" : "失敗")
      << '\n'
      << "登録先ディスク照合: "
      << (report.registered_location_matches_expected_disk
              ? "成功"
              : (report.registered_location_reported
                     ? "失敗"
                     : "登録なし"))
      << '\n'
      << "登録パーティション: ";
  if (report.registered_partition_number == 0U) {
    stream << "なし\n";
  } else {
    stream << report.registered_partition_number << '\n';
  }
  stream
      << "Winre.wim容量: " << report.winre_image_size_bytes
      << " bytes\n"
      << "変更処理: 実行していません\n";
  return stream.str();
}

void write_failure(
    std::ostream& stream,
    const clonecore::Error& error) {
  stream << "処理を完了できませんでした: "
         << diskmodel::to_utf8(error.operation) << " (Windows error "
         << error.native_code << ") "
         << diskmodel::to_utf8(error.message) << '\n';
}

}  // namespace

int run_winpe_app(
    const std::vector<std::wstring>& arguments,
    diskmodel::IDiskInventoryProvider& provider,
    std::ostream& output,
    std::ostream& error_output,
    ICloneExecutionService* const execution_service,
    bootrepair::IStandaloneBootRepairService* const boot_repair_service,
    IJobManifestLoader* const job_manifest_loader,
    IRestoreImageVerifier* const restore_image_verifier,
    IRestoreExecutionSafetyProbe* const restore_safety_probe,
    IRestoreImageCandidateProvider* const restore_image_candidates,
    const clonecore::DiskOperationCallbacks* const operation_callbacks,
    IRestoreExecutionService* const restore_execution_service,
    ICloneExecutionService* const clone_job_execution_service,
    bootrepair::IWinReDiagnosticService* const
        winre_diagnostic_service,
    IMbr2GptJobExecutionService* const
        mbr2gpt_job_execution_service) {
  if (arguments.size() == 1 && is_help_argument(arguments.front())) {
    write_usage(
        output,
        execution_service != nullptr,
        boot_repair_service != nullptr,
        job_manifest_loader != nullptr,
        restore_execution_service != nullptr,
        clone_job_execution_service != nullptr,
        winre_diagnostic_service != nullptr,
        mbr2gpt_job_execution_service != nullptr);
    return static_cast<int>(clitools::CliExitCode::success);
  }
  const bool is_preflight =
      !arguments.empty() && arguments.front() == L"--clone-preflight";
  const bool is_execute =
      !arguments.empty() && arguments.front() == L"--clone-execute";
  const bool is_boot_preflight =
      !arguments.empty() &&
      arguments.front() == L"--boot-repair-preflight";
  const bool is_boot_execute =
      !arguments.empty() &&
      arguments.front() == L"--boot-repair-execute";
  const bool is_job_preflight =
      !arguments.empty() && arguments.front() == L"--job-preflight";
  const bool is_job_execute =
      !arguments.empty() && arguments.front() == L"--job-execute";
  const bool is_winre_diagnostic =
      !arguments.empty() &&
      arguments.front() == L"--winre-diagnostic";
  if (!is_preflight && !is_execute &&
      !is_boot_preflight && !is_boot_execute && !is_job_preflight &&
      !is_job_execute && !is_winre_diagnostic) {
    return clitools::run_inventory_cli(
        arguments, provider, output, error_output, kWinPePresentation);
  }

  if (is_execute && execution_service == nullptr) {
    error_output
        << "このビルドではクローン実行サービスが無効です。\n";
    write_usage(
        error_output,
        false,
        boot_repair_service != nullptr,
        job_manifest_loader != nullptr,
        restore_execution_service != nullptr,
        clone_job_execution_service != nullptr,
        winre_diagnostic_service != nullptr);
    return static_cast<int>(clitools::CliExitCode::invalid_arguments);
  }
  if ((is_boot_preflight || is_boot_execute) &&
      boot_repair_service == nullptr) {
    error_output << "このビルドでは単独起動修復サービスが無効です。\n";
    write_usage(
        error_output,
        execution_service != nullptr,
        false,
        job_manifest_loader != nullptr,
        restore_execution_service != nullptr,
        clone_job_execution_service != nullptr,
        winre_diagnostic_service != nullptr);
    return static_cast<int>(clitools::CliExitCode::invalid_arguments);
  }
  if ((is_job_preflight || is_job_execute) &&
      job_manifest_loader == nullptr) {
    error_output
        << "このビルドではWinPEジョブ読取りサービスが無効です。\n";
    write_usage(
        error_output,
        execution_service != nullptr,
        boot_repair_service != nullptr,
        false,
        false,
        false,
        winre_diagnostic_service != nullptr);
    return static_cast<int>(clitools::CliExitCode::invalid_arguments);
  }
  if (is_job_execute && restore_execution_service == nullptr &&
      clone_job_execution_service == nullptr &&
      mbr2gpt_job_execution_service == nullptr) {
    error_output
        << "このビルドではクローン／復元／MBRからGPTジョブ実行サービスが無効です。\n";
    write_usage(
        error_output,
        execution_service != nullptr,
        boot_repair_service != nullptr,
        true,
        false,
        false,
        winre_diagnostic_service != nullptr);
    return static_cast<int>(clitools::CliExitCode::invalid_arguments);
  }
  if (is_winre_diagnostic &&
      winre_diagnostic_service == nullptr) {
    error_output
        << "このビルドではWinRE読み取り専用診断サービスが無効です。\n";
    write_usage(
        error_output,
        execution_service != nullptr,
        boot_repair_service != nullptr,
        job_manifest_loader != nullptr,
        restore_execution_service != nullptr,
        clone_job_execution_service != nullptr,
        false);
    return static_cast<int>(clitools::CliExitCode::invalid_arguments);
  }

  if (is_winre_diagnostic) {
    const auto parsed = parse_winre_diagnostic_arguments(arguments);
    if (!parsed.has_value()) {
      error_output << "WinRE読み取り専用診断の引数が不正です。\n";
      write_usage(
          error_output,
          execution_service != nullptr,
          boot_repair_service != nullptr,
          job_manifest_loader != nullptr,
          restore_execution_service != nullptr,
          clone_job_execution_service != nullptr,
          true);
      return static_cast<int>(
          clitools::CliExitCode::invalid_arguments);
    }
    auto inventory = provider.enumerate();
    if (!inventory) {
      write_failure(error_output, inventory.error());
      return static_cast<int>(clitools::CliExitCode::failure);
    }
    if (!inventory.value().issues.empty()) {
      write_failure(
          error_output,
          preflight_error(
              clonecore::ErrorCode::query_failed,
              ERROR_NOT_READY,
              L"WinRE診断のディスク列挙",
              L"未解決のディスク列挙診断があるため対象を確定できません"));
      return static_cast<int>(clitools::CliExitCode::failure);
    }
    const auto selected = std::find_if(
        inventory.value().disks.begin(),
        inventory.value().disks.end(),
        [&](const diskmodel::DiskInfo& disk) {
          return disk.disk_number == parsed->disk_number;
        });
    if (selected == inventory.value().disks.end()) {
      write_failure(
          error_output,
          preflight_error(
              clonecore::ErrorCode::query_failed,
              ERROR_NOT_FOUND,
              L"WinRE診断の対象ディスク",
              L"指定した物理ディスクを再列挙結果から一意に確認できません"));
      return static_cast<int>(clitools::CliExitCode::failure);
    }
    const auto identity = diskmodel::make_stable_disk_identity(
        *selected, selected->is_system_disk);
    if (!identity) {
      write_failure(error_output, identity.error());
      return static_cast<int>(clitools::CliExitCode::failure);
    }
    auto report = winre_diagnostic_service->inspect(
        parsed->windows_root + L"Windows",
        parsed->disk_number);
    if (!report) {
      write_failure(error_output, report.error());
      return static_cast<int>(clitools::CliExitCode::failure);
    }
    output << (parsed->format == OutputFormat::json
                   ? format_winre_diagnostic_json(
                         parsed.value(), *selected, report.value())
                   : format_winre_diagnostic_text(
                         parsed.value(), *selected, report.value()));
    if (!winre_diagnostic_complete(report.value())) {
      error_output
          << "WinRE診断結果が不明のため、再構築計画へ進めません。\n";
      return static_cast<int>(clitools::CliExitCode::failure);
    }
    return static_cast<int>(clitools::CliExitCode::success);
  }

  if (is_job_preflight || is_job_execute) {
    const auto parsed_preflight =
        is_job_preflight
            ? parse_job_preflight_arguments(arguments)
            : std::optional<JobPreflightArguments>{};
    const auto parsed_execute =
        is_job_execute
            ? parse_job_execute_arguments(arguments)
            : std::optional<JobExecuteArguments>{};
    if ((is_job_preflight && !parsed_preflight.has_value()) ||
        (is_job_execute && !parsed_execute.has_value())) {
      error_output
          << (is_job_execute
                  ? "WinPE予約ジョブ実行の引数が不正です。\n"
                  : "WinPEジョブプリフライトの引数が不正です。\n");
      write_usage(
          error_output,
          execution_service != nullptr,
          boot_repair_service != nullptr,
          true,
          restore_execution_service != nullptr,
          clone_job_execution_service != nullptr,
          winre_diagnostic_service != nullptr);
      return static_cast<int>(clitools::CliExitCode::invalid_arguments);
    }
    const JobPreflightArguments& preflight_arguments =
        is_job_execute
            ? parsed_execute->preflight
            : parsed_preflight.value();
    auto preflight = perform_job_preflight(
        preflight_arguments,
        provider,
        *job_manifest_loader,
        restore_image_verifier,
        restore_safety_probe,
        restore_image_candidates);
    if (!preflight) {
      write_failure(error_output, preflight.error());
      return static_cast<int>(clitools::CliExitCode::failure);
    }
    JobPreflightReport report = preflight.take_value();
    if (is_job_preflight) {
      output << (parsed_preflight->format == OutputFormat::json
                     ? format_job_preflight_json(report)
                     : format_job_preflight_text(report));
      return static_cast<int>(clitools::CliExitCode::success);
    }

    const clonecore::TargetConfirmation confirmation{
        .first_step_acknowledged =
            parsed_execute->erasure_acknowledged,
        .typed_token = parsed_execute->confirmation,
    };
    const auto job_type = report.verified_job.manifest.job_type;
    if (job_type == imageformat::JobType::clone) {
      if (clone_job_execution_service == nullptr) {
        write_failure(
            error_output,
            preflight_error(
                clonecore::ErrorCode::unsupported_platform,
                ERROR_NOT_SUPPORTED,
                L"WinPEクローンジョブ実行サービス",
                L"このビルドでは製品版クローンジョブ実行サービスが無効です"));
        return static_cast<int>(clitools::CliExitCode::failure);
      }
      if (!report.verified_job.manifest.source.has_value() ||
          !report.verified_job.manifest.target.has_value() ||
          !report.source.has_value() || !report.target.has_value()) {
        write_failure(
            error_output,
            preflight_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DATA,
                L"WinPEクローンジョブ実行",
                L"完全なコピー元・コピー先識別情報がありません"));
        return static_cast<int>(clitools::CliExitCode::failure);
      }
      auto observed_source = diskmodel::make_stable_disk_identity(
          *report.source, report.source->is_system_disk);
      auto observed_target = diskmodel::make_stable_disk_identity(
          *report.target, report.target->is_system_disk);
      if (!observed_source) {
        write_failure(error_output, observed_source.error());
        return static_cast<int>(clitools::CliExitCode::failure);
      }
      if (!observed_target) {
        write_failure(error_output, observed_target.error());
        return static_cast<int>(clitools::CliExitCode::failure);
      }
      const auto confirmation_status =
          clonecore::validate_clone_identities(
              *report.verified_job.manifest.source,
              observed_source.value(),
              *report.verified_job.manifest.target,
              observed_target.value(),
              confirmation);
      if (!confirmation_status) {
        write_failure(error_output, confirmation_status.error());
        return static_cast<int>(clitools::CliExitCode::failure);
      }
      auto execution = clone_job_execution_service->execute(
          CloneExecutionRequest{
              .expected_source = observed_source.take_value(),
              .expected_target = observed_target.take_value(),
              .confirmation = confirmation,
              .authorization = {},
              .callbacks =
                  operation_callbacks == nullptr
                      ? clonecore::DiskOperationCallbacks{}
                      : *operation_callbacks,
          });
      if (!execution) {
        write_failure(error_output, execution.error());
        return static_cast<int>(clitools::CliExitCode::failure);
      }
      const auto execution_status =
          validate_clone_job_execution_report(
              execution.value(), report.source->partition_style);
      if (!execution_status) {
        write_failure(error_output, execution_status.error());
        return static_cast<int>(clitools::CliExitCode::failure);
      }
      output << (parsed_execute->preflight.format == OutputFormat::json
                     ? format_execution_json(execution.value())
                     : format_execution_text(execution.value()));
      return static_cast<int>(clitools::CliExitCode::success);
    }

    if (job_type == imageformat::JobType::mbr_to_gpt) {
      if (mbr2gpt_job_execution_service == nullptr) {
        write_failure(
            error_output,
            preflight_error(
                clonecore::ErrorCode::unsupported_platform,
                ERROR_NOT_SUPPORTED,
                L"WinPE MBRからGPTジョブ実行サービス",
                L"このビルドでは製品版MBRからGPT移行サービスが無効です"));
        return static_cast<int>(clitools::CliExitCode::failure);
      }
      if (!report.verified_job.manifest.source.has_value() ||
          !report.verified_job.manifest.target.has_value() ||
          !report.source.has_value() || !report.target.has_value()) {
        write_failure(
            error_output,
            preflight_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DATA,
                L"WinPE MBRからGPTジョブ実行",
                L"完全なコピー元・コピー先識別情報がありません"));
        return static_cast<int>(clitools::CliExitCode::failure);
      }
      auto observed_source = diskmodel::make_stable_disk_identity(
          *report.source, report.source->is_system_disk);
      auto observed_target = diskmodel::make_stable_disk_identity(
          *report.target, report.target->is_system_disk);
      if (!observed_source) {
        write_failure(error_output, observed_source.error());
        return static_cast<int>(clitools::CliExitCode::failure);
      }
      if (!observed_target) {
        write_failure(error_output, observed_target.error());
        return static_cast<int>(clitools::CliExitCode::failure);
      }
      const auto confirmation_status =
          clonecore::validate_clone_identities(
              *report.verified_job.manifest.source,
              observed_source.value(),
              *report.verified_job.manifest.target,
              observed_target.value(),
              confirmation);
      if (!confirmation_status) {
        write_failure(error_output, confirmation_status.error());
        return static_cast<int>(clitools::CliExitCode::failure);
      }
      auto execution = mbr2gpt_job_execution_service->execute(
          Mbr2GptJobExecutionRequest{
              .expected_source =
                  *report.verified_job.manifest.source,
              .expected_target =
                  *report.verified_job.manifest.target,
              .confirmation = confirmation,
              .callbacks =
                  operation_callbacks == nullptr
                      ? clonecore::DiskOperationCallbacks{}
                      : *operation_callbacks,
          });
      if (!execution) {
        write_failure(error_output, execution.error());
        return static_cast<int>(clitools::CliExitCode::failure);
      }
      const auto execution_status =
          validate_mbr2gpt_job_execution_report(execution.value());
      if (!execution_status) {
        write_failure(error_output, execution_status.error());
        return static_cast<int>(clitools::CliExitCode::failure);
      }
      output << (parsed_execute->preflight.format == OutputFormat::json
                     ? format_mbr2gpt_execution_json(execution.value())
                     : format_mbr2gpt_execution_text(execution.value()));
      return static_cast<int>(clitools::CliExitCode::success);
    }

    if (job_type == imageformat::JobType::restore_image) {
      if (restore_execution_service == nullptr) {
        write_failure(
            error_output,
            preflight_error(
                clonecore::ErrorCode::unsupported_platform,
                ERROR_NOT_SUPPORTED,
                L"WinPE復元実行サービス",
                L"このビルドでは復元実行サービスが無効です"));
        return static_cast<int>(clitools::CliExitCode::failure);
      }
      if (!report.verified_job.manifest.target.has_value() ||
          !report.verified_job.manifest.restore_image_identity.has_value() ||
          !report.verified_restore_image.has_value() ||
          !report.restore_execution_readiness.has_value() ||
          !report.target.has_value()) {
        write_failure(
            error_output,
            preflight_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DATA,
                L"WinPE復元ジョブ実行",
                L"完全な復元ジョブ、dcimg、または安全検査結果がありません"));
        return static_cast<int>(clitools::CliExitCode::failure);
      }
      if (!report.restore_execution_readiness->required_checks_passed) {
        write_failure(
            error_output,
            preflight_error(
                clonecore::ErrorCode::verification_failed,
                ERROR_NOT_READY,
                L"WinPE復元実行直前の安全検査",
                L"必須安全検査に危険または不明があるため復元を開始できません"));
        return static_cast<int>(clitools::CliExitCode::failure);
      }
      auto observed_target = diskmodel::make_stable_disk_identity(
          *report.target, report.target->is_system_disk);
      if (!observed_target) {
        write_failure(error_output, observed_target.error());
        return static_cast<int>(clitools::CliExitCode::failure);
      }
      const auto confirmation_status = validate_restore_confirmation(
          *report.verified_job.manifest.target,
          observed_target.value(),
          confirmation);
      if (!confirmation_status) {
        write_failure(error_output, confirmation_status.error());
        return static_cast<int>(clitools::CliExitCode::failure);
      }
      auto execution = restore_execution_service->execute(
          RestoreExecutionRequest{
              .expected_target = observed_target.take_value(),
              .expected_image =
                  *report.verified_job.manifest.restore_image_identity,
              .verified_image_path =
                  report.verified_restore_image->canonical_path,
              .confirmation = confirmation,
              .callbacks =
                  operation_callbacks == nullptr
                      ? clonecore::DiskOperationCallbacks{}
                      : *operation_callbacks,
          });
      if (!execution) {
        write_failure(error_output, execution.error());
        return static_cast<int>(clitools::CliExitCode::failure);
      }
      const auto execution_status =
          validate_restore_execution_report(execution.value());
      if (!execution_status) {
        write_failure(error_output, execution_status.error());
        return static_cast<int>(clitools::CliExitCode::failure);
      }
      output << (parsed_execute->preflight.format == OutputFormat::json
                     ? format_restore_execution_json(execution.value())
                     : format_restore_execution_text(execution.value()));
      return static_cast<int>(clitools::CliExitCode::success);
    }

    write_failure(
        error_output,
        preflight_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"WinPE予約ジョブ実行",
            L"イメージ作成ジョブの実行サービスはまだ接続していません"));
    return static_cast<int>(clitools::CliExitCode::failure);
  }

  if (is_boot_preflight || is_boot_execute) {
    const auto parsed =
        parse_boot_repair_arguments(arguments, is_boot_execute);
    if (!parsed.has_value()) {
      error_output << "単独起動修復の引数が不正です。\n";
      write_usage(
          error_output,
          execution_service != nullptr,
          boot_repair_service != nullptr,
          job_manifest_loader != nullptr,
          restore_execution_service != nullptr,
          clone_job_execution_service != nullptr,
          winre_diagnostic_service != nullptr);
      return static_cast<int>(clitools::CliExitCode::invalid_arguments);
    }
    auto selection = boot_repair_service->inspect(parsed->request);
    if (!selection) {
      write_failure(error_output, selection.error());
      return static_cast<int>(clitools::CliExitCode::failure);
    }
    if (is_boot_preflight) {
      output << (parsed->format == OutputFormat::json
                     ? format_boot_repair_preflight_json(
                           parsed->request, selection.value())
                     : format_boot_repair_preflight_text(
                           parsed->request, selection.value()));
      return static_cast<int>(clitools::CliExitCode::success);
    }

    const clonecore::TargetConfirmation confirmation{
        .first_step_acknowledged = parsed->change_acknowledged,
        .typed_token = parsed->confirmation,
    };
    const clonecore::Status confirmation_status =
        bootrepair::validate_boot_repair_selection(
            selection.value(),
            selection.value(),
            parsed->request.firmware,
            confirmation);
    if (!confirmation_status) {
      write_failure(error_output, confirmation_status.error());
      return static_cast<int>(clitools::CliExitCode::failure);
    }
    auto execution = boot_repair_service->execute(
        bootrepair::StandaloneBootRepairExecutionRequest{
            .target = parsed->request,
            .expected = selection.take_value(),
            .confirmation = confirmation,
        });
    if (!execution) {
      write_failure(error_output, execution.error());
      return static_cast<int>(clitools::CliExitCode::failure);
    }
    output << (parsed->format == OutputFormat::json
                   ? format_boot_repair_execution_json(
                         parsed->request, execution.value())
                   : format_boot_repair_execution_text(
                         parsed->request, execution.value()));
    return static_cast<int>(clitools::CliExitCode::success);
  }

  const auto parsed_preflight = parse_clone_preflight_arguments(arguments);
  const auto parsed_execute = parse_clone_execute_arguments(arguments);
  if ((is_preflight && !parsed_preflight.has_value()) ||
      (is_execute && !parsed_execute.has_value())) {
    error_output << (is_execute
                         ? "クローン実行の引数が不正です。\n"
                         : "クローンプリフライトの引数が不正です。\n");
    write_usage(
        error_output,
        execution_service != nullptr,
        boot_repair_service != nullptr,
        job_manifest_loader != nullptr,
        restore_execution_service != nullptr,
        clone_job_execution_service != nullptr,
        winre_diagnostic_service != nullptr);
    return static_cast<int>(clitools::CliExitCode::invalid_arguments);
  }
  auto inventory = provider.enumerate();
  if (!inventory) {
    write_failure(error_output, inventory.error());
    return static_cast<int>(clitools::CliExitCode::failure);
  }
  const ClonePreflightArguments selection_arguments = is_execute
      ? ClonePreflightArguments{
            .source_disk_number = parsed_execute->source_disk_number,
            .target_disk_number = parsed_execute->target_disk_number,
            .format = parsed_execute->format,
        }
      : parsed_preflight.value();
  const auto preflight = evaluate_clone_preflight(
      selection_arguments, inventory.value());
  if (!preflight) {
    write_failure(error_output, preflight.error());
    return static_cast<int>(clitools::CliExitCode::failure);
  }
  if (!is_execute) {
    output << (parsed_preflight->format == OutputFormat::json
                   ? format_preflight_json(preflight.value())
                   : format_preflight_text(preflight.value()));
    return static_cast<int>(clitools::CliExitCode::success);
  }

  const clonecore::TargetConfirmation confirmation{
      .first_step_acknowledged = parsed_execute->erasure_acknowledged,
      .typed_token = parsed_execute->confirmation,
  };
  const auto confirmation_status = clonecore::validate_clone_identities(
      preflight.value().source_identity,
      preflight.value().source_identity,
      preflight.value().target_identity,
      preflight.value().target_identity,
      confirmation);
  if (!confirmation_status) {
    write_failure(error_output, confirmation_status.error());
    return static_cast<int>(clitools::CliExitCode::failure);
  }

  const auto execution = execution_service->execute(CloneExecutionRequest{
      .expected_source = preflight.value().source_identity,
      .expected_target = preflight.value().target_identity,
      .confirmation = confirmation,
      .authorization = parsed_execute->authorization,
      .callbacks =
          operation_callbacks == nullptr
          ? clonecore::DiskOperationCallbacks{}
          : *operation_callbacks,
  });
  if (!execution) {
    write_failure(error_output, execution.error());
    return static_cast<int>(clitools::CliExitCode::failure);
  }
  output << (parsed_execute->format == OutputFormat::json
                 ? format_execution_json(execution.value())
                 : format_execution_text(execution.value()));
  return static_cast<int>(clitools::CliExitCode::success);
}

}  // namespace ytec::winpeapp
