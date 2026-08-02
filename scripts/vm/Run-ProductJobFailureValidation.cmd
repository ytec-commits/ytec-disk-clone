@echo off
setlocal EnableExtensions DisableDelayedExpansion

mode con cols=132 lines=45
chcp 65001 >nul
cls

set "SCENARIO=%~1"
set "PAYLOAD=%~d0"
set "FIXTURE=%PAYLOAD%\ytec-product-job-fixture-vm.exe"
set "FAILURE_HARNESS=%PAYLOAD%\ytec-product-job-failure-vm.exe"
set "FIXTURE_OUTPUT=X:\ydc-failure-fixture.txt"
set "RESULT_OUTPUT=X:\ydc-failure-result.txt"

echo === Y-TEC TSUMUGI DRIVE PRODUCT RESTORE FAILURE VM ===
echo SCENARIO=%SCENARIO%
echo PAYLOAD=%PAYLOAD%

if /i "%SCENARIO%"=="corrupt-image" (
  set "EXPECTED_MARKER=YDC_PRODUCT_RESTORE_CORRUPT_IMAGE_PASS"
) else if /i "%SCENARIO%"=="tampered-job" (
  set "EXPECTED_MARKER=YDC_PRODUCT_RESTORE_TAMPERED_JOB_PASS"
) else (
  set "FAILURE_DETAIL=invalid-scenario"
  goto :failure
)

if not exist "%FIXTURE%" (
  set "FAILURE_DETAIL=fixture-not-found"
  goto :failure
)
if not exist "%FAILURE_HARNESS%" (
  set "FAILURE_DETAIL=failure-harness-not-found"
  goto :failure
)
if exist X:\TsumugiValidation (
  set "FAILURE_DETAIL=ram-output-not-new"
  goto :failure
)

"%FIXTURE%" --profile restore-synthetic --authorization YTEC-VM-ONLY-PRODUCT-RESTORE-FIXTURE >"%FIXTURE_OUTPUT%" 2>&1
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

"%FAILURE_HARNESS%" --scenario "%SCENARIO%" --confirmation "%CONFIRMATION%" --authorization YTEC-VM-ONLY-PRODUCT-JOB-FAILURE >"%RESULT_OUTPUT%" 2>&1
set "RESULT_EXIT=%ERRORLEVEL%"
type "%RESULT_OUTPUT%"
echo FAILURE_HARNESS_EXIT=%RESULT_EXIT%
if not "%RESULT_EXIT%"=="0" (
  set "FAILURE_DETAIL=failure-harness-exit"
  goto :failure
)

set "MARKER_FOUND="
for /f "tokens=1" %%M in (%RESULT_OUTPUT%) do (
  if "%%M"=="%EXPECTED_MARKER%" set "MARKER_FOUND=1"
)
if not defined MARKER_FOUND (
  set "FAILURE_DETAIL=expected-marker-not-found"
  goto :failure
)

cls
echo === Y-TEC TSUMUGI DRIVE PRODUCT RESTORE FAILURE VM ===
echo.
type "%RESULT_OUTPUT%"
echo.
echo FINAL_RESULT=PASS
echo EXPECTED_MARKER=%EXPECTED_MARKER%
echo Controlled shutdown will begin after the evidence window.
ping -n 21 127.0.0.1 >nul
wpeutil shutdown
echo YDC_PRODUCT_JOB_FAILURE_VM_FAIL shutdown-returned
exit /b 89

:failure
echo.
echo YDC_PRODUCT_JOB_FAILURE_VM_FAIL %FAILURE_DETAIL%
echo Controlled failure shutdown will begin shortly.
ping -n 4 127.0.0.1 >nul
wpeutil shutdown
exit /b 90
