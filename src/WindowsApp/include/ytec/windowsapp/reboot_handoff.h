#pragma once

#include "ytec/clonecore/result.h"

#include <cstdint>
#include <memory>
#include <string>

namespace ytec::windowsapp {

enum class RebootHandoffReadiness : std::uint8_t {
  ready,
  elevation_required,
  unsupported_windows,
};

struct RebootHandoffPlan final {
  RebootHandoffReadiness readiness{
      RebootHandoffReadiness::unsupported_windows};
  std::wstring guidance;
};

// Advanced startup is available on Windows 8 and later. It intentionally
// stops at the Microsoft boot-options UI: choosing a user-created WinPE USB
// remains a visible user action because firmware boot entries are not portable.
[[nodiscard]] RebootHandoffPlan build_reboot_handoff_plan(
    bool process_elevated,
    std::uint32_t windows_major,
    std::uint32_t windows_minor);

class IRebootHandoffService {
 public:
  virtual ~IRebootHandoffService() = default;

  [[nodiscard]] virtual clonecore::Status
  restart_to_advanced_boot_options() = 0;
};

// Refuses to call the service unless the previously displayed plan is ready.
[[nodiscard]] clonecore::Status request_reboot_handoff(
    const RebootHandoffPlan& plan,
    IRebootHandoffService& service);

// Uses only the local Windows InitiateShutdownW API. It enables
// SE_SHUTDOWN_NAME for the current process, never forces other sessions or
// applications, and requests the Microsoft advanced boot-options screen.
[[nodiscard]] std::unique_ptr<IRebootHandoffService>
make_windows_reboot_handoff_service();

}  // namespace ytec::windowsapp
