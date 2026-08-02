#pragma once

#include "ytec/clonecore/result.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::bootrepair {

enum class BcdBootFirmware : std::uint8_t {
  uefi,
  bios,
};

enum class BcdBootStorePolicy : std::uint8_t {
  preserve_existing,
  rebuild_fresh,
};

struct BcdBootRequest final {
  std::wstring target_windows_directory;
  std::wstring target_system_partition_root;
  BcdBootFirmware firmware{BcdBootFirmware::uefi};
  BcdBootStorePolicy store_policy{
      BcdBootStorePolicy::preserve_existing};
};

struct ProcessResult final {
  std::uint32_t exit_code{};
  std::string standard_output;
  std::string standard_error;
};

using ProcessOutputCallback =
    std::function<void(std::string_view)>;

struct BcdBootReport final {
  std::wstring executable_path;
  std::uint32_t exit_code{};
  std::string standard_output;
  std::string standard_error;
  bool microsoft_signature_verified{};
  bool prior_store_replaced{};
  bool fresh_store_verified{};
};

class IExecutableTrustVerifier {
 public:
  virtual ~IExecutableTrustVerifier() = default;
  [[nodiscard]] virtual clonecore::Status verify_microsoft_signed(
      const std::wstring& executable_path) = 0;
};

class IProcessRunner {
 public:
  virtual ~IProcessRunner() = default;
  [[nodiscard]] virtual clonecore::Result<ProcessResult> run(
      const std::wstring& executable_path,
      const std::vector<std::wstring>& arguments,
      const std::wstring& working_directory) = 0;

  // Implementations may override this to deliver stdout while the process is
  // running. The default keeps existing mock/process implementations usable
  // and delivers the captured output once the process has completed.
  [[nodiscard]] virtual clonecore::Result<ProcessResult> run_streamed(
      const std::wstring& executable_path,
      const std::vector<std::wstring>& arguments,
      const std::wstring& working_directory,
      const ProcessOutputCallback& standard_output_callback);
};

class IBcdStoreFileSystem {
 public:
  virtual ~IBcdStoreFileSystem() = default;

  [[nodiscard]] virtual clonecore::Result<bool>
  is_regular_non_reparse_file(const std::wstring& path) = 0;
  [[nodiscard]] virtual clonecore::Status move_file_no_replace(
      const std::wstring& source,
      const std::wstring& destination) = 0;
  [[nodiscard]] virtual clonecore::Status remove_file(
      const std::wstring& path) = 0;
};

[[nodiscard]] clonecore::Result<std::vector<std::wstring>>
build_bcdboot_arguments(const BcdBootRequest& request);

[[nodiscard]] std::wstring quote_windows_argument(std::wstring_view argument);

[[nodiscard]] clonecore::Result<BcdBootReport> execute_bcdboot(
    const BcdBootRequest& request,
    const std::wstring& trusted_system_directory,
    IExecutableTrustVerifier& trust_verifier,
    IProcessRunner& process_runner);

[[nodiscard]] clonecore::Result<std::wstring> bcd_store_path(
    const BcdBootRequest& request);

[[nodiscard]] clonecore::Result<BcdBootReport>
execute_bcdboot_with_store_transaction(
    const BcdBootRequest& request,
    const std::wstring& trusted_system_directory,
    IExecutableTrustVerifier& trust_verifier,
    IProcessRunner& process_runner,
    IBcdStoreFileSystem& file_system);

[[nodiscard]] clonecore::Result<BcdBootReport>
execute_bcdboot_with_windows_apis(const BcdBootRequest& request);

[[nodiscard]] std::unique_ptr<IExecutableTrustVerifier>
make_windows_authenticode_verifier();

[[nodiscard]] std::unique_ptr<IProcessRunner> make_windows_process_runner(
    std::uint32_t timeout_milliseconds = 10U * 60U * 1000U);

}  // namespace ytec::bootrepair
