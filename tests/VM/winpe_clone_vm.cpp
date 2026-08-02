#include "vm_clone_execution_service.h"

#include "ytec/clonecore/log.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/winpeapp/app_runner.h"

#include <Windows.h>

#include <iostream>
#include <string>
#include <vector>

#ifndef YTEC_VM_DESTRUCTIVE_TEST_ONLY
#error This executable must never be built as a product target.
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
  ytec::vmtest::VmCloneExecutionService execution_service;
  return ytec::winpeapp::run_winpe_app(
      arguments,
      *provider,
      std::cout,
      std::cerr,
      &execution_service,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr);
}
