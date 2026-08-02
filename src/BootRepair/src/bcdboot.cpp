#include "ytec/bootrepair/bcdboot.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>
#include <Softpub.h>
#include <Wincrypt.h>
#include <Wintrust.h>
#include <Mscat.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ytec::bootrepair {
namespace {

using clonecore::Error;
using clonecore::ErrorCode;
using clonecore::Result;
using clonecore::Status;
using clonecore::UniqueHandle;

constexpr std::size_t kMaximumCapturedBytes = 4U * 1024U * 1024U;

Error boot_error(
    const ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

bool is_drive_absolute_path(const std::wstring& path) {
  return path.size() >= 3 && std::iswalpha(path[0]) != 0 && path[1] == L':' &&
         (path[2] == L'\\' || path[2] == L'/');
}

bool has_forbidden_path_text(const std::wstring& path) {
  if (path.find(L'"') != std::wstring::npos ||
      path.find(L'\r') != std::wstring::npos ||
      path.find(L'\n') != std::wstring::npos ||
      path.find(L'*') != std::wstring::npos ||
      path.find(L'?') != std::wstring::npos) {
    return true;
  }
  std::wstring normalized = path;
  std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
  return normalized.find(L"\\..\\") != std::wstring::npos ||
         normalized.ends_with(L"\\..") || normalized.starts_with(L"..\\");
}

std::wstring normalize_slashes(std::wstring value) {
  std::replace(value.begin(), value.end(), L'/', L'\\');
  while (value.size() > 3 && value.back() == L'\\') {
    value.pop_back();
  }
  return value;
}

bool ends_with_case_insensitive(
    const std::wstring& value,
    const std::wstring_view suffix) {
  if (value.size() < suffix.size()) {
    return false;
  }
  const std::wstring_view ending(
      value.data() + value.size() - suffix.size(), suffix.size());
  return _wcsnicmp(ending.data(), suffix.data(), suffix.size()) == 0;
}

Status validate_system_directory(const std::wstring& directory) {
  if (!is_drive_absolute_path(directory) || has_forbidden_path_text(directory) ||
      !ends_with_case_insensitive(
          normalize_slashes(directory), L"\\Windows\\System32")) {
    return Status::failure(boot_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"BCDBootシステムディレクトリ",
        L"現在のWindowsまたはWinPEのSystem32絶対パスが必要です"));
  }
  return clonecore::success_status();
}

Status validate_bcdboot_paths(const BcdBootRequest& request) {
  const std::wstring windows_directory =
      normalize_slashes(request.target_windows_directory);
  const std::wstring system_root =
      normalize_slashes(request.target_system_partition_root);
  if (!is_drive_absolute_path(windows_directory) ||
      has_forbidden_path_text(windows_directory) ||
      !ends_with_case_insensitive(windows_directory, L"\\Windows")) {
    return Status::failure(boot_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"BCDBoot対象Windowsディレクトリ",
        L"コピー先のドライブ絶対パスで終端がWindowsのディレクトリだけを指定できます"));
  }
  if (system_root.size() != 3 || std::iswalpha(system_root[0]) == 0 ||
      system_root[1] != L':' || system_root[2] != L'\\') {
    return Status::failure(boot_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"BCDBoot対象システムパーティション",
        L"コピー先システムパーティションのドライブ文字ルートが必要です"));
  }
  if (request.firmware != BcdBootFirmware::uefi &&
      request.firmware != BcdBootFirmware::bios) {
    return Status::failure(boot_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"BCDBootファームウェア方式",
        L"UEFIまたはBIOSだけを指定できます"));
  }
  if (request.store_policy !=
          BcdBootStorePolicy::preserve_existing &&
      request.store_policy !=
          BcdBootStorePolicy::rebuild_fresh) {
    return Status::failure(boot_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"BCDBootストア方針",
        L"既存保持または新規再構築だけを指定できます"));
  }
  if (request.firmware == BcdBootFirmware::uefi &&
      std::towupper(windows_directory[0]) == std::towupper(system_root[0])) {
    return Status::failure(boot_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_DRIVE,
        L"BCDBoot対象分離",
        L"WindowsパーティションとEFIシステムパーティションは別ドライブ文字で指定してください"));
  }
  return clonecore::success_status();
}

Result<std::string> read_pipe_to_string(
    const HANDLE pipe,
    const ProcessOutputCallback& callback = {}) {
  std::string output;
  std::array<char, 4096> buffer{};
  for (;;) {
    DWORD bytes_read = 0;
    const BOOL success = ReadFile(
        pipe,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &bytes_read,
        nullptr);
    if (!success) {
      const DWORD native_code = GetLastError();
      if (native_code == ERROR_BROKEN_PIPE) {
        break;
      }
      return Result<std::string>::failure(clonecore::make_win32_error(
          ErrorCode::io_failed, L"外部ツール出力の読取り", native_code));
    }
    if (bytes_read == 0) {
      break;
    }
    if (output.size() > kMaximumCapturedBytes - bytes_read) {
      return Result<std::string>::failure(boot_error(
          ErrorCode::invalid_data,
          ERROR_BUFFER_OVERFLOW,
          L"外部ツール出力上限",
          L"Microsoft標準ツールの出力が安全な取得上限を超えました"));
    }
    output.append(buffer.data(), bytes_read);
    if (callback) {
      try {
        callback(std::string_view(buffer.data(), bytes_read));
      } catch (...) {
        // Process capture must remain reliable even if a UI observer fails.
      }
    }
  }
  return Result<std::string>::success(std::move(output));
}

template <typename Function>
Function load_wintrust_function(
    const HMODULE module,
    const char* const function_name) noexcept {
#pragma warning(push)
#pragma warning(disable : 4191)
  const auto function =
      reinterpret_cast<Function>(GetProcAddress(module, function_name));
#pragma warning(pop)
  return function;
}

class WintrustFunctions final {
 public:
  WintrustFunctions() noexcept
      : module_(LoadLibraryExW(
            L"wintrust.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
    if (module_ == nullptr) {
      error_ = GetLastError();
      return;
    }
    acquire_context_ = load_wintrust_function<
        decltype(&CryptCATAdminAcquireContext2)>(
        module_, "CryptCATAdminAcquireContext2");
    calculate_hash_ = load_wintrust_function<
        decltype(&CryptCATAdminCalcHashFromFileHandle2)>(
        module_, "CryptCATAdminCalcHashFromFileHandle2");
    enumerate_catalog_ = load_wintrust_function<
        decltype(&CryptCATAdminEnumCatalogFromHash)>(
        module_, "CryptCATAdminEnumCatalogFromHash");
    catalog_information_ = load_wintrust_function<
        decltype(&CryptCATCatalogInfoFromContext)>(
        module_, "CryptCATCatalogInfoFromContext");
    release_catalog_ = load_wintrust_function<
        decltype(&CryptCATAdminReleaseCatalogContext)>(
        module_, "CryptCATAdminReleaseCatalogContext");
    release_context_ = load_wintrust_function<
        decltype(&CryptCATAdminReleaseContext)>(
        module_, "CryptCATAdminReleaseContext");
    provider_data_ = load_wintrust_function<
        decltype(&WTHelperProvDataFromStateData)>(
        module_, "WTHelperProvDataFromStateData");
    provider_signer_ = load_wintrust_function<
        decltype(&WTHelperGetProvSignerFromChain)>(
        module_, "WTHelperGetProvSignerFromChain");
    if (!available()) {
      error_ = ERROR_PROC_NOT_FOUND;
    }
  }

  ~WintrustFunctions() {
    if (module_ != nullptr) {
      FreeLibrary(module_);
    }
  }

  WintrustFunctions(const WintrustFunctions&) = delete;
  WintrustFunctions& operator=(const WintrustFunctions&) = delete;

  [[nodiscard]] bool available() const noexcept {
    return module_ != nullptr && acquire_context_ != nullptr &&
           calculate_hash_ != nullptr && enumerate_catalog_ != nullptr &&
           catalog_information_ != nullptr && release_catalog_ != nullptr &&
           release_context_ != nullptr && provider_data_ != nullptr &&
           provider_signer_ != nullptr;
  }

  [[nodiscard]] DWORD error() const noexcept { return error_; }

  decltype(&CryptCATAdminAcquireContext2) acquire_context_{};
  decltype(&CryptCATAdminCalcHashFromFileHandle2) calculate_hash_{};
  decltype(&CryptCATAdminEnumCatalogFromHash) enumerate_catalog_{};
  decltype(&CryptCATCatalogInfoFromContext) catalog_information_{};
  decltype(&CryptCATAdminReleaseCatalogContext) release_catalog_{};
  decltype(&CryptCATAdminReleaseContext) release_context_{};
  decltype(&WTHelperProvDataFromStateData) provider_data_{};
  decltype(&WTHelperGetProvSignerFromChain) provider_signer_{};

 private:
  HMODULE module_{};
  DWORD error_{ERROR_SUCCESS};
};

void close_trust_state(WINTRUST_DATA& trust_data) noexcept {
  GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  trust_data.dwStateAction = WTD_STATEACTION_CLOSE;
  (void)WinVerifyTrust(nullptr, &policy, &trust_data);
}

Status verify_microsoft_provider_signer(
    const WintrustFunctions& functions,
    const WINTRUST_DATA& trust_data) {
  auto* const provider = functions.provider_data_(trust_data.hWVTStateData);
  auto* const signer = provider == nullptr
      ? nullptr
      : functions.provider_signer_(provider, 0, FALSE, 0);
  if (signer == nullptr || signer->csCertChain == 0 ||
      signer->pasCertChain == nullptr ||
      signer->pasCertChain[0].pCert == nullptr) {
    return Status::failure(boot_error(
        ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"Microsoft標準ツール署名者情報",
        L"信頼プロバイダーから署名証明書を取得できません"));
  }

  const PCCERT_CONTEXT certificate = signer->pasCertChain[0].pCert;
  const DWORD organization_length = CertGetNameStringW(
      certificate,
      CERT_NAME_ATTR_TYPE,
      0,
      const_cast<char*>(szOID_ORGANIZATION_NAME),
      nullptr,
      0);
  std::vector<wchar_t> organization(organization_length, L'\0');
  if (organization_length <= 1 ||
      CertGetNameStringW(
          certificate,
          CERT_NAME_ATTR_TYPE,
          0,
          const_cast<char*>(szOID_ORGANIZATION_NAME),
          organization.data(),
          static_cast<DWORD>(organization.size())) != organization_length) {
    return Status::failure(boot_error(
        ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"Microsoft標準ツール署名者名",
        L"署名証明書の組織名を取得できません"));
  }

  constexpr std::wstring_view kMicrosoftOrganization =
      L"Microsoft Corporation";
  const std::wstring organization_name(organization.data());
  if (organization_name.size() != kMicrosoftOrganization.size() ||
      _wcsicmp(organization_name.c_str(), kMicrosoftOrganization.data()) != 0) {
    return Status::failure(boot_error(
        ErrorCode::verification_failed,
        static_cast<DWORD>(TRUST_E_SUBJECT_NOT_TRUSTED),
        L"Microsoft標準ツール署名者",
        L"有効な署名ですが組織名がMicrosoft Corporationではありません"));
  }
  return clonecore::success_status();
}

std::wstring hash_member_tag(const std::span<const BYTE> hash) {
  constexpr wchar_t kHexadecimal[] = L"0123456789ABCDEF";
  std::wstring tag;
  tag.reserve(hash.size() * 2U);
  for (const BYTE value : hash) {
    tag.push_back(kHexadecimal[value >> 4U]);
    tag.push_back(kHexadecimal[value & 0x0FU]);
  }
  return tag;
}

Status verify_catalog_signature(
    const std::wstring& executable_path,
    const WintrustFunctions& functions) {
  UniqueHandle executable(CreateFileW(
      executable_path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  if (!executable) {
    return Status::failure(clonecore::make_win32_error(
        ErrorCode::verification_failed,
        L"Microsoft標準ツールカタログ対象ファイル",
        GetLastError()));
  }

  HCATADMIN catalog_admin = nullptr;
  GUID catalog_subsystem = DRIVER_ACTION_VERIFY;
  if (!functions.acquire_context_(
          &catalog_admin, &catalog_subsystem, nullptr, nullptr, 0)) {
    return Status::failure(clonecore::make_win32_error(
        ErrorCode::verification_failed,
        L"Microsoft標準ツールカタログ管理コンテキスト",
        GetLastError()));
  }

  DWORD hash_size = 0;
  if (!functions.calculate_hash_(
          catalog_admin, executable.get(), &hash_size, nullptr, 0) ||
      hash_size == 0 || hash_size > 128) {
    const DWORD native_code = GetLastError();
    functions.release_context_(catalog_admin, 0);
    return Status::failure(boot_error(
        ErrorCode::verification_failed,
        native_code == ERROR_SUCCESS ? ERROR_INVALID_DATA : native_code,
        L"Microsoft標準ツールカタログハッシュサイズ",
        L"カタログ照合用ハッシュのサイズを安全に取得できません"));
  }
  std::vector<BYTE> hash(hash_size);
  if (!functions.calculate_hash_(
          catalog_admin,
          executable.get(),
          &hash_size,
          hash.data(),
          0)) {
    const DWORD native_code = GetLastError();
    functions.release_context_(catalog_admin, 0);
    return Status::failure(clonecore::make_win32_error(
        ErrorCode::verification_failed,
        L"Microsoft標準ツールカタログハッシュ",
        native_code));
  }

  HCATINFO catalog = functions.enumerate_catalog_(
      catalog_admin, hash.data(), hash_size, 0, nullptr);
  if (catalog == nullptr) {
    const DWORD native_code = GetLastError();
    functions.release_context_(catalog_admin, 0);
    return Status::failure(boot_error(
        ErrorCode::verification_failed,
        native_code == ERROR_SUCCESS
            ? static_cast<DWORD>(TRUST_E_NOSIGNATURE)
            : native_code,
        L"Microsoft標準ツールカタログ検索",
        L"対象ファイルを含むWindows署名カタログがありません"));
  }

  CATALOG_INFO catalog_info{};
  catalog_info.cbStruct = sizeof(catalog_info);
  if (!functions.catalog_information_(catalog, &catalog_info, 0)) {
    const DWORD native_code = GetLastError();
    functions.release_catalog_(catalog_admin, catalog, 0);
    functions.release_context_(catalog_admin, 0);
    return Status::failure(clonecore::make_win32_error(
        ErrorCode::verification_failed,
        L"Microsoft標準ツールカタログ情報",
        native_code));
  }

  const std::wstring member_tag = hash_member_tag(hash);
  WINTRUST_CATALOG_INFO member{};
  member.cbStruct = sizeof(member);
  member.pcwszCatalogFilePath = catalog_info.wszCatalogFile;
  member.pcwszMemberTag = member_tag.c_str();
  member.pcwszMemberFilePath = executable_path.c_str();
  member.hMemberFile = executable.get();
  member.pbCalculatedFileHash = hash.data();
  member.cbCalculatedFileHash = hash_size;
  member.hCatAdmin = catalog_admin;

  WINTRUST_DATA trust_data{};
  trust_data.cbStruct = sizeof(trust_data);
  trust_data.dwUIChoice = WTD_UI_NONE;
  trust_data.fdwRevocationChecks = WTD_REVOKE_NONE;
  trust_data.dwUnionChoice = WTD_CHOICE_CATALOG;
  trust_data.pCatalog = &member;
  trust_data.dwStateAction = WTD_STATEACTION_VERIFY;
  trust_data.dwProvFlags =
      WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
  GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG trust_status = WinVerifyTrust(nullptr, &policy, &trust_data);
  const Status signer_status = trust_status == ERROR_SUCCESS
      ? verify_microsoft_provider_signer(functions, trust_data)
      : Status::failure(boot_error(
            ErrorCode::verification_failed,
            static_cast<DWORD>(trust_status),
            L"Microsoft標準ツールカタログ署名検証",
            L"対象ファイルのWindowsカタログ署名を信頼できません"));
  close_trust_state(trust_data);
  functions.release_catalog_(catalog_admin, catalog, 0);
  functions.release_context_(catalog_admin, 0);
  return signer_status;
}

class WindowsAuthenticodeVerifier final : public IExecutableTrustVerifier {
 public:
  Status verify_microsoft_signed(
      const std::wstring& executable_path) override {
    const DWORD attributes = GetFileAttributesW(executable_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      return Status::failure(boot_error(
          ErrorCode::invalid_data,
          GetLastError(),
          L"Microsoft標準ツール実行ファイル属性",
          L"通常ファイルとして存在するMicrosoft標準ツールを確認できません"));
    }

    const WintrustFunctions functions;
    if (!functions.available()) {
      return Status::failure(boot_error(
          ErrorCode::verification_failed,
          functions.error(),
          L"Microsoft標準ツール署名検証API",
          L"Windows署名検証APIを安全に読み込めません"));
    }

    WINTRUST_FILE_INFO file_info{};
    file_info.cbStruct = sizeof(file_info);
    file_info.pcwszFilePath = executable_path.c_str();
    WINTRUST_DATA trust_data{};
    trust_data.cbStruct = sizeof(trust_data);
    trust_data.dwUIChoice = WTD_UI_NONE;
    trust_data.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust_data.dwUnionChoice = WTD_CHOICE_FILE;
    trust_data.pFile = &file_info;
    trust_data.dwStateAction = WTD_STATEACTION_VERIFY;
    trust_data.dwProvFlags =
        WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG trust_status = WinVerifyTrust(nullptr, &policy, &trust_data);
    if (trust_status == ERROR_SUCCESS) {
      const Status signer_status =
          verify_microsoft_provider_signer(functions, trust_data);
      close_trust_state(trust_data);
      return signer_status;
    }
    close_trust_state(trust_data);
    if (trust_status != TRUST_E_NOSIGNATURE) {
      return Status::failure(boot_error(
          ErrorCode::verification_failed,
          static_cast<DWORD>(trust_status),
          L"Microsoft標準ツールAuthenticode検証",
          L"対象ファイルの埋込みデジタル署名を信頼できません"));
    }
    return verify_catalog_signature(executable_path, functions);
  }
};

class WindowsProcessRunner final : public IProcessRunner {
 public:
  explicit WindowsProcessRunner(const DWORD timeout_milliseconds)
      : timeout_milliseconds_(timeout_milliseconds) {}

  Result<ProcessResult> run(
      const std::wstring& executable_path,
      const std::vector<std::wstring>& arguments,
      const std::wstring& working_directory) override {
    return run_impl(
        executable_path, arguments, working_directory, {});
  }

  Result<ProcessResult> run_streamed(
      const std::wstring& executable_path,
      const std::vector<std::wstring>& arguments,
      const std::wstring& working_directory,
      const ProcessOutputCallback& standard_output_callback) override {
    return run_impl(
        executable_path,
        arguments,
        working_directory,
        standard_output_callback);
  }

 private:
  Result<ProcessResult> run_impl(
      const std::wstring& executable_path,
      const std::vector<std::wstring>& arguments,
      const std::wstring& working_directory,
      const ProcessOutputCallback& standard_output_callback) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE stdout_read_raw = nullptr;
    HANDLE stdout_write_raw = nullptr;
    HANDLE stderr_read_raw = nullptr;
    HANDLE stderr_write_raw = nullptr;
    if (!CreatePipe(&stdout_read_raw, &stdout_write_raw, &security, 0) ||
        !CreatePipe(&stderr_read_raw, &stderr_write_raw, &security, 0)) {
      if (stdout_read_raw != nullptr) {
        CloseHandle(stdout_read_raw);
      }
      if (stdout_write_raw != nullptr) {
        CloseHandle(stdout_write_raw);
      }
      if (stderr_read_raw != nullptr) {
        CloseHandle(stderr_read_raw);
      }
      if (stderr_write_raw != nullptr) {
        CloseHandle(stderr_write_raw);
      }
      return Result<ProcessResult>::failure(clonecore::make_win32_error(
          ErrorCode::io_failed,
          L"Microsoft標準ツール出力パイプ作成",
          GetLastError()));
    }
    UniqueHandle stdout_read(stdout_read_raw);
    UniqueHandle stdout_write(stdout_write_raw);
    UniqueHandle stderr_read(stderr_read_raw);
    UniqueHandle stderr_write(stderr_write_raw);
    if (!SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(stderr_read.get(), HANDLE_FLAG_INHERIT, 0)) {
      return Result<ProcessResult>::failure(clonecore::make_win32_error(
          ErrorCode::io_failed,
          L"Microsoft標準ツールパイプ継承設定",
          GetLastError()));
    }

    std::wstring command_line = quote_windows_argument(executable_path);
    for (const auto& argument : arguments) {
      command_line.push_back(L' ');
      command_line.append(quote_windows_argument(argument));
    }
    std::vector<wchar_t> mutable_command(
        command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = stdout_write.get();
    startup.hStdError = stderr_write.get();
    PROCESS_INFORMATION process_info{};
    if (!CreateProcessW(
            executable_path.c_str(),
            mutable_command.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
            nullptr,
            working_directory.c_str(),
            &startup,
            &process_info)) {
      return Result<ProcessResult>::failure(clonecore::make_win32_error(
          ErrorCode::io_failed,
          L"Microsoft標準ツールプロセス起動",
          GetLastError()));
    }
    UniqueHandle process(process_info.hProcess);
    UniqueHandle thread(process_info.hThread);
    stdout_write.reset();
    stderr_write.reset();

    Result<std::string> stdout_result =
        Result<std::string>::success(std::string{});
    Result<std::string> stderr_result =
        Result<std::string>::success(std::string{});
    std::thread stdout_thread([&] {
      stdout_result = read_pipe_to_string(
          stdout_read.get(), standard_output_callback);
    });
    std::thread stderr_thread([&] {
      stderr_result = read_pipe_to_string(stderr_read.get());
    });
    const DWORD wait_result =
        WaitForSingleObject(process.get(), timeout_milliseconds_);
    if (wait_result == WAIT_TIMEOUT) {
      TerminateProcess(process.get(), ERROR_TIMEOUT);
      WaitForSingleObject(process.get(), 30'000);
    }
    stdout_thread.join();
    stderr_thread.join();
    if (wait_result != WAIT_OBJECT_0) {
      return Result<ProcessResult>::failure(boot_error(
          ErrorCode::io_failed,
          wait_result == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError(),
          L"Microsoft標準ツールプロセス待機",
          L"Microsoft標準ツールが制限時間内に正常終了しませんでした"));
    }
    if (!stdout_result) {
      return Result<ProcessResult>::failure(stdout_result.error());
    }
    if (!stderr_result) {
      return Result<ProcessResult>::failure(stderr_result.error());
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process.get(), &exit_code)) {
      return Result<ProcessResult>::failure(clonecore::make_win32_error(
          ErrorCode::io_failed,
          L"Microsoft標準ツール終了コード取得",
          GetLastError()));
    }
    return Result<ProcessResult>::success(ProcessResult{
        .exit_code = exit_code,
        .standard_output = stdout_result.take_value(),
        .standard_error = stderr_result.take_value(),
    });
  }

  DWORD timeout_milliseconds_{};
};

class WindowsBcdStoreFileSystem final : public IBcdStoreFileSystem {
 public:
  Result<bool> is_regular_non_reparse_file(
      const std::wstring& path) override {
    SetLastError(ERROR_SUCCESS);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD native_code = GetLastError();
      if (native_code == ERROR_FILE_NOT_FOUND ||
          native_code == ERROR_PATH_NOT_FOUND) {
        return Result<bool>::success(false);
      }
      return Result<bool>::failure(clonecore::make_win32_error(
          ErrorCode::query_failed,
          L"BCDストア属性確認",
          native_code));
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      return Result<bool>::failure(boot_error(
          ErrorCode::invalid_data,
          ERROR_REPARSE_TAG_INVALID,
          L"BCDストア属性確認",
          L"通常ファイルかつreparseでないBCDストアだけを扱えます"));
    }
    return Result<bool>::success(true);
  }

  Status move_file_no_replace(
      const std::wstring& source,
      const std::wstring& destination) override {
    if (!MoveFileExW(
            source.c_str(),
            destination.c_str(),
            MOVEFILE_WRITE_THROUGH)) {
      return Status::failure(clonecore::make_win32_error(
          ErrorCode::io_failed,
          L"既存BCDストア退避",
          GetLastError()));
    }
    return clonecore::success_status();
  }

  Status remove_file(const std::wstring& path) override {
    if (!DeleteFileW(path.c_str())) {
      return Status::failure(clonecore::make_win32_error(
          ErrorCode::io_failed,
          L"BCDストア一時退避ファイル削除",
          GetLastError()));
    }
    return clonecore::success_status();
  }
};

}  // namespace

Result<ProcessResult> IProcessRunner::run_streamed(
    const std::wstring& executable_path,
    const std::vector<std::wstring>& arguments,
    const std::wstring& working_directory,
    const ProcessOutputCallback& standard_output_callback) {
  auto result = run(executable_path, arguments, working_directory);
  if (result && standard_output_callback &&
      !result.value().standard_output.empty()) {
    try {
      standard_output_callback(result.value().standard_output);
    } catch (...) {
      // Observers cannot change a completed process result.
    }
  }
  return result;
}

Result<std::vector<std::wstring>> build_bcdboot_arguments(
    const BcdBootRequest& request) {
  const Status validation = validate_bcdboot_paths(request);
  if (!validation) {
    return Result<std::vector<std::wstring>>::failure(validation.error());
  }
  std::vector<std::wstring> arguments{
      normalize_slashes(request.target_windows_directory),
      L"/s",
      normalize_slashes(request.target_system_partition_root),
      L"/f",
      request.firmware == BcdBootFirmware::uefi ? L"UEFI" : L"BIOS",
      L"/v",
  };
  if (request.store_policy ==
      BcdBootStorePolicy::rebuild_fresh) {
    arguments.emplace_back(L"/c");
  }
  return Result<std::vector<std::wstring>>::success(
      std::move(arguments));
}

Result<std::wstring> bcd_store_path(const BcdBootRequest& request) {
  const Status validation = validate_bcdboot_paths(request);
  if (!validation) {
    return Result<std::wstring>::failure(validation.error());
  }
  const std::wstring root =
      normalize_slashes(request.target_system_partition_root);
  const std::wstring relative =
      request.firmware == BcdBootFirmware::uefi
          ? L"EFI\\Microsoft\\Boot\\BCD"
          : L"Boot\\BCD";
  return Result<std::wstring>::success(root + relative);
}

std::wstring quote_windows_argument(const std::wstring_view argument) {
  std::wstring quoted;
  quoted.push_back(L'"');
  std::size_t backslash_count = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslash_count;
      continue;
    }
    if (character == L'"') {
      quoted.append(backslash_count * 2 + 1, L'\\');
      quoted.push_back(L'"');
      backslash_count = 0;
      continue;
    }
    quoted.append(backslash_count, L'\\');
    backslash_count = 0;
    quoted.push_back(character);
  }
  quoted.append(backslash_count * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

Result<BcdBootReport> execute_bcdboot(
    const BcdBootRequest& request,
    const std::wstring& trusted_system_directory,
    IExecutableTrustVerifier& trust_verifier,
    IProcessRunner& process_runner) {
  const Status system_status =
      validate_system_directory(trusted_system_directory);
  if (!system_status) {
    return Result<BcdBootReport>::failure(system_status.error());
  }
  const auto arguments = build_bcdboot_arguments(request);
  if (!arguments) {
    return Result<BcdBootReport>::failure(arguments.error());
  }
  const std::wstring system_directory =
      normalize_slashes(trusted_system_directory);
  const std::wstring executable_path = system_directory + L"\\bcdboot.exe";
  const Status trust_status =
      trust_verifier.verify_microsoft_signed(executable_path);
  if (!trust_status) {
    return Result<BcdBootReport>::failure(trust_status.error());
  }
  const auto process = process_runner.run(
      executable_path, arguments.value(), system_directory);
  if (!process) {
    return Result<BcdBootReport>::failure(process.error());
  }
  if (process.value().exit_code != 0) {
    return Result<BcdBootReport>::failure(boot_error(
        ErrorCode::verification_failed,
        process.value().exit_code,
        L"BCDBoot終了コード",
        L"BCDBootが失敗終了したためコピー先を成功扱いにできません"));
  }
  return Result<BcdBootReport>::success(BcdBootReport{
      .executable_path = executable_path,
      .exit_code = process.value().exit_code,
      .standard_output = process.value().standard_output,
      .standard_error = process.value().standard_error,
      .microsoft_signature_verified = true,
  });
}

Result<BcdBootReport> execute_bcdboot_with_store_transaction(
    const BcdBootRequest& request,
    const std::wstring& trusted_system_directory,
    IExecutableTrustVerifier& trust_verifier,
    IProcessRunner& process_runner,
    IBcdStoreFileSystem& file_system) {
  if (request.store_policy != BcdBootStorePolicy::rebuild_fresh) {
    return execute_bcdboot(
        request,
        trusted_system_directory,
        trust_verifier,
        process_runner);
  }

  const Status system_status =
      validate_system_directory(trusted_system_directory);
  if (!system_status) {
    return Result<BcdBootReport>::failure(system_status.error());
  }
  const auto arguments = build_bcdboot_arguments(request);
  if (!arguments) {
    return Result<BcdBootReport>::failure(arguments.error());
  }
  const auto store_path_result = bcd_store_path(request);
  if (!store_path_result) {
    return Result<BcdBootReport>::failure(store_path_result.error());
  }
  const std::wstring system_directory =
      normalize_slashes(trusted_system_directory);
  const std::wstring executable_path = system_directory + L"\\bcdboot.exe";

  // The signed executable is verified before the target BCD is changed.
  const Status trust_status =
      trust_verifier.verify_microsoft_signed(executable_path);
  if (!trust_status) {
    return Result<BcdBootReport>::failure(trust_status.error());
  }

  const std::wstring store_path = store_path_result.value();
  const std::wstring backup_path = store_path + L".ytec-rebuild-backup";
  const auto existing = file_system.is_regular_non_reparse_file(store_path);
  if (!existing) {
    return Result<BcdBootReport>::failure(existing.error());
  }
  bool prior_store_moved = false;
  if (existing.value()) {
    const auto backup_exists =
        file_system.is_regular_non_reparse_file(backup_path);
    if (!backup_exists) {
      return Result<BcdBootReport>::failure(backup_exists.error());
    }
    if (backup_exists.value()) {
      return Result<BcdBootReport>::failure(boot_error(
          ErrorCode::invalid_data,
          ERROR_ALREADY_EXISTS,
          L"既存BCDストア退避先確認",
          L"前回処理の退避ファイルが残っているため新しいBCDを作成できません"));
    }
    const Status moved =
        file_system.move_file_no_replace(store_path, backup_path);
    if (!moved) {
      return Result<BcdBootReport>::failure(moved.error());
    }
    prior_store_moved = true;
  }

  const auto rollback_prior_store = [&]() -> Status {
    if (!prior_store_moved) {
      return clonecore::success_status();
    }
    const auto replacement =
        file_system.is_regular_non_reparse_file(store_path);
    if (!replacement) {
      return Status::failure(replacement.error());
    }
    if (replacement.value()) {
      const Status removed = file_system.remove_file(store_path);
      if (!removed) {
        return removed;
      }
    }
    return file_system.move_file_no_replace(backup_path, store_path);
  };

  // Re-verify immediately before execution as well.  Target mutation cannot
  // weaken the executable trust boundary between preparation and launch.
  auto report = execute_bcdboot(
      request,
      trusted_system_directory,
      trust_verifier,
      process_runner);
  if (!report) {
    const Status rolled_back = rollback_prior_store();
    if (!rolled_back) {
      return Result<BcdBootReport>::failure(rolled_back.error());
    }
    return report;
  }

  const auto fresh_store =
      file_system.is_regular_non_reparse_file(store_path);
  if (!fresh_store || !fresh_store.value()) {
    Error failure = fresh_store
        ? boot_error(
              ErrorCode::verification_failed,
              ERROR_FILE_NOT_FOUND,
              L"新規BCDストア検証",
              L"BCDBoot成功後に新しいBCDストアを確認できません")
        : fresh_store.error();
    const Status rolled_back = rollback_prior_store();
    if (!rolled_back) {
      return Result<BcdBootReport>::failure(rolled_back.error());
    }
    return Result<BcdBootReport>::failure(std::move(failure));
  }

  if (prior_store_moved) {
    const Status removed = file_system.remove_file(backup_path);
    if (!removed) {
      return Result<BcdBootReport>::failure(removed.error());
    }
  }
  report.value().prior_store_replaced = prior_store_moved;
  report.value().fresh_store_verified = true;
  return report;
}

Result<BcdBootReport> execute_bcdboot_with_windows_apis(
    const BcdBootRequest& request) {
  std::vector<wchar_t> system_directory(MAX_PATH, L'\0');
  const UINT length = GetSystemDirectoryW(
      system_directory.data(), static_cast<UINT>(system_directory.size()));
  if (length == 0 || length >= system_directory.size()) {
    return Result<BcdBootReport>::failure(clonecore::make_win32_error(
        ErrorCode::internal_error,
        L"現在のSystem32パス取得",
        GetLastError()));
  }
  auto verifier = make_windows_authenticode_verifier();
  auto runner = make_windows_process_runner();
  WindowsBcdStoreFileSystem file_system;
  return execute_bcdboot_with_store_transaction(
      request,
      std::wstring(system_directory.data(), length),
      *verifier,
      *runner,
      file_system);
}

std::unique_ptr<IExecutableTrustVerifier>
make_windows_authenticode_verifier() {
  return std::make_unique<WindowsAuthenticodeVerifier>();
}

std::unique_ptr<IProcessRunner> make_windows_process_runner(
    const std::uint32_t timeout_milliseconds) {
  const DWORD bounded_timeout =
      (std::max)(1U, (std::min)(
                         timeout_milliseconds,
                         4U * 60U * 60U * 1000U));
  return std::make_unique<WindowsProcessRunner>(bounded_timeout);
}

}  // namespace ytec::bootrepair
