#include "ytec/winpeapp/app_runner.h"

#include <Windows.h>

#include <memory>

namespace ytec::winpeapp {
namespace {

class WindowsRestoreExecutionSafetyProbe final
    : public IRestoreExecutionSafetyProbe {
 public:
  clonecore::Result<RestoreExecutionSafetyObservation> inspect(
      const diskmodel::DiskInfo& target,
      const imageformat::RestoreImageInspectionReport& image) override {
    SYSTEM_POWER_STATUS power{};
    RestoreSafetyState power_state = RestoreSafetyState::unknown;
    if (GetSystemPowerStatus(&power) != FALSE) {
      if (power.ACLineStatus == 1) {
        power_state = RestoreSafetyState::passed;
      } else if (power.ACLineStatus == 0) {
        power_state = RestoreSafetyState::blocked;
      }
    }
    return clonecore::Result<
        RestoreExecutionSafetyObservation>::success(
        derive_restore_execution_safety_observation(
            target, image, power_state));
  }
};

}  // namespace

std::unique_ptr<IRestoreExecutionSafetyProbe>
make_windows_restore_execution_safety_probe() {
  return std::make_unique<WindowsRestoreExecutionSafetyProbe>();
}

}  // namespace ytec::winpeapp
