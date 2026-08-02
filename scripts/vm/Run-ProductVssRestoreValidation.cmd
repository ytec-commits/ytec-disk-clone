@echo off
setlocal EnableExtensions DisableDelayedExpansion

mode con cols=160 lines=60
chcp 65001 >nul
cls

echo === Y-TEC TSUMUGI DRIVE PRODUCT VSS RESTORE VM ===

set "YDC_HARNESS=%~dp0ytec-product-vss-restore-vm.exe"
if not exist "%YDC_HARNESS%" (
  set "YDC_HARNESS="
  for %%D in (D E F G H I J K L M N O P Q R S T U V W Y Z) do (
    if exist "%%D:\ytec-product-vss-restore-vm.exe" (
      set "YDC_HARNESS=%%D:\ytec-product-vss-restore-vm.exe"
    )
  )
)
if not defined YDC_HARNESS goto :fail

if not exist "X:\TsumugiValidation" mkdir "X:\TsumugiValidation"
"%YDC_HARNESS%" --authorization YTEC-VM-ONLY-PRODUCT-VSS-RESTORE >"X:\TsumugiValidation\vss-restore.log" 2>&1
if errorlevel 1 goto :fail_with_log

type "X:\TsumugiValidation\vss-restore.log"
echo.
echo FINAL_RESULT=PASS
echo Controlled shutdown will begin after the evidence window.
ping -n 31 127.0.0.1 >nul
wpeutil shutdown
exit /b 0

:fail_with_log
type "X:\TsumugiValidation\vss-restore.log"
:fail
echo.
echo FINAL_RESULT=FAIL
echo Failure evidence will remain visible for one minute.
ping -n 61 127.0.0.1 >nul
wpeutil shutdown
exit /b 1
