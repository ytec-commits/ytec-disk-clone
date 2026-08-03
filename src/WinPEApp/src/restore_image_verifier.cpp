#include "ytec/winpeapp/app_runner.h"

#include "ytec/migrationengine/restore_inspection.h"
#include "ytec/migrationengine/shrink_bundle.h"

#include <algorithm>
#include <filesystem>
#include <memory>

namespace ytec::winpeapp {
namespace {

class WindowsRestoreImageVerifier final : public IRestoreImageVerifier {
 public:
  clonecore::Result<imageformat::RestoreImageInspectionReport> verify(
      const std::wstring& path) override {
    if (_wcsicmp(
            std::filesystem::path(path).filename().c_str(),
            migrationengine::kShrinkBundleManifestFileName) == 0) {
      return migrationengine::inspect_shrink_bundle_for_restore(path);
    }
    return imageformat::inspect_restore_image_file_v1(path);
  }
};

}  // namespace

std::unique_ptr<IRestoreImageVerifier>
make_windows_restore_image_verifier() {
  return std::make_unique<WindowsRestoreImageVerifier>();
}

}  // namespace ytec::winpeapp
