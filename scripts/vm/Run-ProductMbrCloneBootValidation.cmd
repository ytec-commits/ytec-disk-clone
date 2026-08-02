@echo off
setlocal EnableExtensions DisableDelayedExpansion

mode con cols=132 lines=45
chcp 65001 >nul
cls

set "PAYLOAD=%~dp0"
set "FIXTURE=%PAYLOAD%ytec-product-job-fixture-vm.exe"
set "PRODUCT=%PAYLOAD%ytec-winpe-app.exe"
set "FIXTURE_OUTPUT=X:\ydc-mbr-clone-fixture.txt"
set "PRODUCT_OUTPUT=X:\ydc-mbr-clone-product.json"

echo === Y-TEC TSUMUGI DRIVE PRODUCT MBR CLONE VM ===
if not exist "%FIXTURE%" goto :missing_fixture
if not exist "%PRODUCT%" goto :missing_product
if exist X:\TsumugiValidation goto :output_not_new

"%FIXTURE%" --profile legacy-bios-x64 --authorization YTEC-VM-ONLY-PRODUCT-MBR-X64-JOB-FIXTURE >"%FIXTURE_OUTPUT%" 2>&1
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

echo YDCUART=YDC_PRODUCT_MBR_CLONE_PASS;END>COM1
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

:failure
echo YDCUART=YDC_PRODUCT_MBR_CLONE_FAIL;END>COM1
echo.
echo YDC_PRODUCT_MBR_CLONE_VM_FAIL %FAILURE_DETAIL%
if exist "%PRODUCT_OUTPUT%" type "%PRODUCT_OUTPUT%"
echo Failure evidence will remain visible for one minute.
ping -n 61 127.0.0.1 >nul
wpeutil shutdown
exit /b 90
