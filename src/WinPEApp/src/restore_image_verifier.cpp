#include "ytec/winpeapp/app_runner.h"

#include <memory>

namespace ytec::winpeapp {
namespace {

class WindowsRestoreImageVerifier final : public IRestoreImageVerifier {
 public:
  clonecore::Result<imageformat::RestoreImageInspectionReport> verify(
      const std::wstring& path) override {
    return imageformat::inspect_restore_image_file_v1(path);
  }
};

}  // namespace

std::unique_ptr<IRestoreImageVerifier>
make_windows_restore_image_verifier() {
  return std::make_unique<WindowsRestoreImageVerifier>();
}

}  // namespace ytec::winpeapp
