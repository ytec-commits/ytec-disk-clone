#include "ytec/vssrequester/windows_backend.h"
#include "ytec/windowsapp/system_state.h"

#include <Windows.h>
#include <objbase.h>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void confirmation_notes_fail_closed() {
  using ytec::windowsapp::PendingRestartState;
  using ytec::windowsapp::pending_restart_confirmation_note;

  check(
      pending_restart_confirmation_note(PendingRestartState::absent).empty(),
      "Proven-absent state should not add a warning");
  const std::wstring required =
      pending_restart_confirmation_note(PendingRestartState::required);
  check(
      required.find(L"再起動が必要") != std::wstring::npos &&
          required.find(L"推奨") != std::wstring::npos,
      "Required state should explain the restart recommendation");
  const std::wstring unknown =
      pending_restart_confirmation_note(PendingRestartState::unknown);
  check(
      unknown.find(L"確認できません") != std::wstring::npos &&
          !unknown.empty(),
      "Unknown state must remain a visible warning");
}

void live_probe_returns_a_bounded_state() {
  const auto observation =
      ytec::windowsapp::query_windows_update_pending_restart();
  using ytec::windowsapp::PendingRestartState;
  check(
      observation.state == PendingRestartState::absent ||
          observation.state == PendingRestartState::required ||
          observation.state == PendingRestartState::unknown,
      "Live read-only probe must return a bounded state");
}

void vss_process_security_precedes_other_com_clients() {
  const HRESULT initialized =
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  check(
      SUCCEEDED(initialized),
      "The application apartment should initialize before COM security");

  const auto first =
      ytec::vssrequester::initialize_vss_process_security();
  check(
      first.has_value(),
      "VSS process security should initialize before another COM client");

  const auto observation =
      ytec::windowsapp::query_windows_update_pending_restart();
  using ytec::windowsapp::PendingRestartState;
  check(
      observation.state == PendingRestartState::absent ||
          observation.state == PendingRestartState::required ||
          observation.state == PendingRestartState::unknown,
      "The later COM client should still return a bounded state");

  const auto repeated =
      ytec::vssrequester::initialize_vss_process_security();
  check(
      repeated.has_value(),
      "The VSS backend safety check should reuse startup security");
  CoUninitialize();
}

}  // namespace

int main() {
  try {
    vss_process_security_precedes_other_com_clients();
    confirmation_notes_fail_closed();
    live_probe_returns_a_bounded_state();
    std::cout << "windows system state tests: PASS\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "windows system state tests: FAIL: "
              << exception.what() << '\n';
    return 1;
  }
}
