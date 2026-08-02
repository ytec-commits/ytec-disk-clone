#include "ytec/clonecore/log.h"
#include "ytec/bootrepair/standalone_repair.h"
#include "ytec/bootrepair/winre_diagnostic.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/winpeapp/app_runner.h"

#include <Windows.h>

#include <iostream>
#include <string>
#include <vector>

#ifndef YTEC_WINPE_PRODUCT_BOUNDARY
#error WinPEApp must be built with the product safety boundary enabled.
#endif

int wmain(const int argc, wchar_t* argv[]) {
  SetConsoleOutputCP(CP_UTF8);

  std::vector<std::wstring> arguments;
  if (argc > 1) {
    arguments.reserve(static_cast<std::size_t>(argc - 1));
  }
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }

  const ytec::clonecore::Logger logger = ytec::clonecore::make_stderr_logger();
  auto provider = ytec::diskmodel::make_windows_disk_inventory_provider(&logger);
  auto boot_repair =
      ytec::bootrepair::make_windows_standalone_boot_repair_service(*provider);
  auto job_manifest_loader =
      ytec::winpeapp::make_windows_job_manifest_loader();
  auto restore_image_verifier =
      ytec::winpeapp::make_windows_restore_image_verifier();
  auto restore_safety_probe =
      ytec::winpeapp::make_windows_restore_execution_safety_probe();
  auto restore_image_candidates =
      ytec::winpeapp::make_windows_restore_image_candidate_provider();
  auto restore_execution =
      ytec::winpeapp::make_windows_restore_execution_service();
  auto clone_job_execution =
      ytec::winpeapp::make_windows_clone_job_execution_service();
  auto mbr2gpt_job_execution =
      ytec::winpeapp::make_windows_mbr2gpt_job_execution_service();
  auto winre_diagnostic =
      ytec::bootrepair::make_windows_winre_diagnostic_service();
  return ytec::winpeapp::run_winpe_app(
      arguments,
      *provider,
      std::cout,
      std::cerr,
      nullptr,
      boot_repair.get(),
      job_manifest_loader.get(),
      restore_image_verifier.get(),
      restore_safety_probe.get(),
      restore_image_candidates.get(),
      nullptr,
      restore_execution.get(),
      clone_job_execution.get(),
      winre_diagnostic.get(),
      mbr2gpt_job_execution.get());
}
