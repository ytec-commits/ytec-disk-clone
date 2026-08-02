@echo off
setlocal EnableExtensions DisableDelayedExpansion

mode con cols=132 lines=45
chcp 65001 >nul
cls

set "PAYLOAD=%~dp0"
set "FIXTURE=%PAYLOAD%ytec-product-job-fixture-vm.exe"
set "PRODUCT=%PAYLOAD%ytec-winpe-app.exe"
set "VERIFIER=%PAYLOAD%ytec-product-mbr2gpt-verifier-vm.exe"
set "FIXTURE_OUTPUT=X:\ydc-mbr2gpt-fixture.txt"
set "PRODUCT_OUTPUT=X:\ydc-mbr2gpt-product.json"
set "VERIFIER_OUTPUT=X:\ydc-mbr2gpt-verifier.txt"

echo === Y-TEC TSUMUGI DRIVE PRODUCT MBR2GPT VM ===
echo PAYLOAD=%PAYLOAD%

if not exist "%FIXTURE%" goto :missing_fixture
if not exist "%PRODUCT%" goto :missing_product
if not exist "%VERIFIER%" goto :missing_verifier
if exist X:\TsumugiValidation goto :output_not_new

"%FIXTURE%" --profile legacy-bios-x64-mbr-to-gpt --authorization YTEC-VM-ONLY-PRODUCT-MBR2GPT-JOB-FIXTURE >"%FIXTURE_OUTPUT%" 2>&1
set "FIXTURE_EXIT=%ERRORLEVEL%"
type "%FIXTURE_OUTPUT%"
if not "%FIXTURE_EXIT%"=="0" goto :fixture_failed

set "CONFIRMATION="
for /f "tokens=1,* delims==" %%A in (%FIXTURE_OUTPUT%) do (
  if "%%A"=="CONFIRMATION" set "CONFIRMATION=%%B"
)
if not defined CONFIRMATION goto :confirmation_missing

"%PRODUCT%" --job-execute --job-path X:\TsumugiValidation\clone-job.json --acknowledge-target-erasure --confirmation "%CONFIRMATION%" --json >"%PRODUCT_OUTPUT%" 2>&1
set "PRODUCT_EXIT=%ERRORLEVEL%"
type "%PRODUCT_OUTPUT%"
if not "%PRODUCT_EXIT%"=="0" goto :product_failed

for %%F in ("%PRODUCT_OUTPUT%") do if %%~zF LSS 2 goto :product_report_invalid

"%VERIFIER%" --authorization YTEC-VM-ONLY-PRODUCT-MBR2GPT-VERIFIER >"%VERIFIER_OUTPUT%" 2>&1
set "VERIFIER_EXIT=%ERRORLEVEL%"
type "%VERIFIER_OUTPUT%"
if not "%VERIFIER_EXIT%"=="0" goto :verifier_failed
set "VERIFIER_MARKER="
set "PROBE_STAGED_MARKER="
for /f "usebackq delims=" %%L in ("%VERIFIER_OUTPUT%") do (
  if "%%L"=="YDC_PRODUCT_MBR2GPT_PASS_V1" set "VERIFIER_MARKER=present"
  if "%%L"=="YDC_TARGET_PROBE_STAGED_V1" set "PROBE_STAGED_MARKER=present"
)
if not defined VERIFIER_MARKER goto :verifier_marker_missing
if not defined PROBE_STAGED_MARKER goto :probe_staged_marker_missing

cls
echo === Y-TEC TSUMUGI DRIVE PRODUCT MBR2GPT VM ===
echo.
type "%PRODUCT_OUTPUT%"
echo.
type "%VERIFIER_OUTPUT%"
echo.
echo FINAL_RESULT=PASS
echo Controlled shutdown will begin after the evidence window.
ping -n 31 127.0.0.1 >nul
wpeutil shutdown
exit /b 89

:missing_fixture
set "FAILURE_DETAIL=fixture-not-found"
goto :failure
:missing_product
set "FAILURE_DETAIL=product-not-found"
goto :failure
:missing_verifier
set "FAILURE_DETAIL=verifier-not-found"
goto :failure
:output_not_new
set "FAILURE_DETAIL=ram-output-not-new"
goto :failure
:fixture_failed
set "FAILURE_DETAIL=fixture-exit"
goto :failure
:confirmation_missing
set "FAILURE_DETAIL=confirmation-not-found"
goto :failure
:product_failed
set "FAILURE_DETAIL=product-exit"
goto :failure
:product_report_invalid
set "FAILURE_DETAIL=product-report-invalid"
goto :failure
:verifier_failed
set "FAILURE_DETAIL=verifier-exit"
goto :failure
:verifier_marker_missing
set "FAILURE_DETAIL=verifier-marker-missing"
goto :failure
:probe_staged_marker_missing
set "FAILURE_DETAIL=probe-staged-marker-missing"
goto :failure

:failure
echo.
echo YDC_PRODUCT_MBR2GPT_VM_FAIL %FAILURE_DETAIL%
if /i "%FAILURE_DETAIL%"=="product-exit" (
  echo.
  echo === MICROSOFT MBR2GPT DIAGNOSTIC ===
  if exist X:\Windows\setuperr.log type X:\Windows\setuperr.log
  echo === END MBR2GPT DIAGNOSTIC ===
  echo.
  echo === READ-ONLY DISK INVENTORY ===
  "%PRODUCT%" --text
  echo === END READ-ONLY DISK INVENTORY ===
  echo.
  echo === PRODUCT ERROR REPEAT ===
  type "%PRODUCT_OUTPUT%"
  echo === END PRODUCT ERROR REPEAT ===
)
echo Failure evidence will remain visible for one minute.
ping -n 61 127.0.0.1 >nul
wpeutil shutdown
exit /b 90
