@echo off
setlocal EnableExtensions DisableDelayedExpansion

mode con cols=132 lines=45
chcp 65001 >nul
cls

set "PROFILE=%~1"
set "MODE=%~2"
if not defined MODE set "MODE=cancellation"
set "PAYLOAD=%~d0"
set "FIXTURE=%PAYLOAD%\ytec-product-job-fixture-vm.exe"
set "CANCELLATION=%PAYLOAD%\ytec-product-job-cancellation-vm.exe"
set "FIXTURE_OUTPUT=X:\ydc-cancel-fixture.txt"
set "CANCEL_OUTPUT=X:\ydc-cancel-result.txt"

echo === Y-TEC TSUMUGI DRIVE PRODUCT JOB CANCELLATION VM ===
echo PROFILE=%PROFILE%
echo MODE=%MODE%
echo PAYLOAD=%PAYLOAD%

if /i "%PROFILE%"=="clone" goto :configure_clone
if /i "%PROFILE%"=="restore" goto :configure_restore
set "FAILURE_DETAIL=invalid-profile"
goto :failure

:configure_clone
if /i "%MODE%"=="auto-once" (
  set "FIXTURE_PROFILE=synthetic-auto-once"
  set "FIXTURE_AUTH=YTEC-VM-ONLY-PRODUCT-AUTO-ONCE-FIXTURE"
  set "EXPECTED_MARKER=YDC_PRODUCT_AUTO_ONCE_PASS"
) else if /i "%MODE%"=="success" (
  set "FIXTURE_PROFILE=synthetic"
  set "FIXTURE_AUTH=YTEC-VM-ONLY-PRODUCT-JOB-FIXTURE"
  set "EXPECTED_MARKER=YDC_PRODUCT_CLONE_SUCCESS_PASS"
) else (
  set "FIXTURE_PROFILE=synthetic"
  set "FIXTURE_AUTH=YTEC-VM-ONLY-PRODUCT-JOB-FIXTURE"
  set "EXPECTED_MARKER=YDC_PRODUCT_CLONE_CANCEL_PASS"
)
goto :run

:configure_restore
set "FIXTURE_PROFILE=restore-synthetic"
set "FIXTURE_AUTH=YTEC-VM-ONLY-PRODUCT-RESTORE-FIXTURE"
if /i "%MODE%"=="success" (
  set "EXPECTED_MARKER=YDC_PRODUCT_RESTORE_SUCCESS_PASS"
) else (
  set "EXPECTED_MARKER=YDC_PRODUCT_RESTORE_CANCEL_PASS"
)

:run
if not exist "%FIXTURE%" (
  set "FAILURE_DETAIL=fixture-not-found"
  goto :failure
)
if not exist "%CANCELLATION%" (
  set "FAILURE_DETAIL=cancellation-harness-not-found"
  goto :failure
)
if exist X:\TsumugiValidation (
  if /i "%MODE%"=="auto-once" exit /b 0
  set "FAILURE_DETAIL=ram-output-not-new"
  goto :failure
)

"%FIXTURE%" --profile "%FIXTURE_PROFILE%" --authorization "%FIXTURE_AUTH%" >"%FIXTURE_OUTPUT%" 2>&1
set "FIXTURE_EXIT=%ERRORLEVEL%"
type "%FIXTURE_OUTPUT%"
echo FIXTURE_EXIT=%FIXTURE_EXIT%
if not "%FIXTURE_EXIT%"=="0" (
  set "FAILURE_DETAIL=fixture-exit"
  goto :failure
)

set "CONFIRMATION="
for /f "tokens=1,* delims==" %%A in (%FIXTURE_OUTPUT%) do (
  if "%%A"=="CONFIRMATION" set "CONFIRMATION=%%B"
)
if not defined CONFIRMATION (
  set "FAILURE_DETAIL=confirmation-not-found"
  goto :failure
)

if /i "%MODE%"=="auto-once" goto :run_auto_once

"%CANCELLATION%" --profile "%PROFILE%" --mode "%MODE%" --confirmation "%CONFIRMATION%" --authorization YTEC-VM-ONLY-PRODUCT-JOB-CANCELLATION >"%CANCEL_OUTPUT%" 2>&1
set "CANCEL_EXIT=%ERRORLEVEL%"
goto :operation_complete

:run_auto_once
if not exist "%PAYLOAD%\ytec-winpe-gui.exe" (
  set "FAILURE_DETAIL=product-gui-not-found"
  goto :failure
)
if exist X:\TsumugiAuto (
  set "FAILURE_DETAIL=auto-once-output-not-new"
  goto :failure
)
if exist Y:\ (
  set "FAILURE_DETAIL=auto-once-drive-letter-not-free"
  goto :failure
)
mkdir X:\TsumugiAuto\Tsumugi
if errorlevel 1 (
  set "FAILURE_DETAIL=auto-once-output-create"
  goto :failure
)
copy /b X:\TsumugiValidation\clone-job.json X:\TsumugiAuto\Tsumugi\Tsumugi-clone-job.json >nul
if errorlevel 1 (
  set "FAILURE_DETAIL=auto-once-job-copy"
  goto :failure
)
subst Y: X:\TsumugiAuto
if errorlevel 1 (
  set "FAILURE_DETAIL=auto-once-drive-map"
  goto :failure
)
if not exist Y:\Tsumugi\Tsumugi-clone-job.json (
  set "FAILURE_DETAIL=auto-once-job-not-visible"
  goto :failure
)
start "" "%PAYLOAD%\ytec-winpe-gui.exe"
if errorlevel 1 (
  set "FAILURE_DETAIL=product-gui-launch"
  goto :failure
)
ping -n 3 127.0.0.1 >nul
"%CANCELLATION%" --profile clone --mode auto-once --confirmation "%CONFIRMATION%" --authorization YTEC-VM-ONLY-PRODUCT-AUTO-ONCE-MONITOR >"%CANCEL_OUTPUT%" 2>&1
set "CANCEL_EXIT=%ERRORLEVEL%"
taskkill /IM ytec-winpe-gui.exe /F >nul 2>&1
subst Y: /D >nul 2>&1

:operation_complete
type "%CANCEL_OUTPUT%"
echo CANCELLATION_EXIT=%CANCEL_EXIT%
if not "%CANCEL_EXIT%"=="0" (
  set "FAILURE_DETAIL=cancellation-exit"
  goto :failure
)

set "MARKER_FOUND="
for /f "tokens=1" %%M in (%CANCEL_OUTPUT%) do (
  if "%%M"=="%EXPECTED_MARKER%" set "MARKER_FOUND=1"
)
if not defined MARKER_FOUND (
  set "FAILURE_DETAIL=expected-marker-not-found"
  goto :failure
)

if /i "%MODE%"=="success" if /i "%PROFILE%"=="restore" goto :verify_restore
goto :show_result

:verify_restore
"%FIXTURE%" --profile restore-verify --authorization YTEC-VM-ONLY-PRODUCT-RESTORE-VERIFY >>"%CANCEL_OUTPUT%" 2>&1
set "VERIFY_EXIT=%ERRORLEVEL%"
type "%CANCEL_OUTPUT%"
if not "%VERIFY_EXIT%"=="0" (
  set "FAILURE_DETAIL=restore-independent-verification-exit"
  goto :failure
)

:show_result
cls
echo === Y-TEC TSUMUGI DRIVE PRODUCT JOB CANCELLATION VM ===
echo.
type "%CANCEL_OUTPUT%"
echo.
echo FINAL_RESULT=PASS
echo EXPECTED_MARKER=%EXPECTED_MARKER%
echo Controlled shutdown will begin after the evidence window.
ping -n 21 127.0.0.1 >nul
wpeutil shutdown
echo YDC_PRODUCT_JOB_CANCEL_VM_FAIL shutdown-returned
exit /b 89

:failure
taskkill /IM ytec-winpe-gui.exe /F >nul 2>&1
subst Y: /D >nul 2>&1
echo.
echo YDC_PRODUCT_JOB_CANCEL_VM_FAIL %FAILURE_DETAIL%
echo Controlled failure shutdown will begin shortly.
ping -n 4 127.0.0.1 >nul
wpeutil shutdown
exit /b 90
