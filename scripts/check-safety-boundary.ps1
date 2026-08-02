$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$sourceRoot = Join-Path $repoRoot 'src'

$forbiddenSourcePatterns = [ordered]@{
    '\bWriteFile\s*\('             = 'WriteFile'
    '\bGENERIC_WRITE\b'             = 'GENERIC_WRITE'
    '\bCREATE_ALWAYS\b'             = 'CREATE_ALWAYS'
    '\bTRUNCATE_EXISTING\b'         = 'TRUNCATE_EXISTING'
    '\bMoveFileExW\s*\('            = 'MoveFileExW'
    '\bDeleteFileW\s*\('            = 'DeleteFileW'
    '\bSetEndOfFile\s*\('           = 'SetEndOfFile'
    '\bSetFileInformationByHandle\s*\(' = 'SetFileInformationByHandle'
    '\bDELETE\b'                      = 'DELETE access'
    '\bIOCTL_DISK_SET_[A-Z0-9_]+\b' = 'IOCTL_DISK_SET_*'
    '\bIOCTL_DISK_CREATE_DISK\b'    = 'IOCTL_DISK_CREATE_DISK'
    '\bIOCTL_DISK_DELETE_DRIVE_LAYOUT\b' = 'IOCTL_DISK_DELETE_DRIVE_LAYOUT'
    '\bFSCTL_LOCK_VOLUME\b'         = 'FSCTL_LOCK_VOLUME'
    '\bFSCTL_DISMOUNT_VOLUME\b'     = 'FSCTL_DISMOUNT_VOLUME'
    '\bFSCTL_EXTEND_VOLUME\b'       = 'FSCTL_EXTEND_VOLUME'
    '\bIOCTL_VOLUME_OFFLINE\b'      = 'IOCTL_VOLUME_OFFLINE'
    '\bIOCTL_VOLUME_ONLINE\b'       = 'IOCTL_VOLUME_ONLINE'
}

$sourceFiles = Get-ChildItem -LiteralPath $sourceRoot -Recurse -File |
    Where-Object { $_.Extension -in @('.h', '.hpp', '.c', '.cpp', '.cxx') }
$failures = @()
$auditedPhysicalWriterPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'DiskModel\src\physical_disk.cpp'))
$auditedPhysicalWriterPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
    '\bIOCTL_DISK_SET_[A-Z0-9_]+\b'
)
$auditedImageFileWriterPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'ImageFormat\src\windows_file_staging.cpp'))
$auditedImageFileWriterPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
    '\bDeleteFileW\s*\('
    '\bSetEndOfFile\s*\('
    '\bSetFileInformationByHandle\s*\('
    '\bDELETE\b'
)
$auditedJobFileWriterPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'ImageFormat\src\job_file.cpp'))
$auditedJobFileWriterPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
    '\bDeleteFileW\s*\('
)
$auditedLogFileWriterPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'CloneCore\src\log.cpp'))
$auditedLogFileWriterPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
)
$auditedWinPeDiskPartScriptWriterPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WinPEApp\src\mbr2gpt_job_execution_service.cpp'))
$auditedWinPeDiskPartScriptWriterPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
    '\bDeleteFileW\s*\('
)
$auditedBcdStoreTransactionPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'BootRepair\src\bcdboot.cpp'))
$auditedBcdStoreTransactionPatterns = @(
    '\bMoveFileExW\s*\('
    '\bDeleteFileW\s*\('
)

foreach ($pattern in $forbiddenSourcePatterns.Keys) {
    $matches = $sourceFiles | Select-String -Pattern $pattern -CaseSensitive
    foreach ($match in $matches) {
        $isAuditedPhysicalWriter = [IO.Path]::GetFullPath($match.Path).Equals(
            $auditedPhysicalWriterPath,
            [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedPhysicalWriter -and $auditedPhysicalWriterPatterns -contains $pattern) {
            continue
        }
        $isAuditedImageFileWriter = [IO.Path]::GetFullPath($match.Path).Equals(
            $auditedImageFileWriterPath,
            [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedImageFileWriter -and $auditedImageFileWriterPatterns -contains $pattern) {
            continue
        }
        $isAuditedJobFileWriter = [IO.Path]::GetFullPath($match.Path).Equals(
            $auditedJobFileWriterPath,
            [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedJobFileWriter -and $auditedJobFileWriterPatterns -contains $pattern) {
            continue
        }
        $isAuditedLogFileWriter = [IO.Path]::GetFullPath($match.Path).Equals(
            $auditedLogFileWriterPath,
            [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedLogFileWriter -and $auditedLogFileWriterPatterns -contains $pattern) {
            continue
        }
        $isAuditedWinPeDiskPartScriptWriter =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedWinPeDiskPartScriptWriterPath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedWinPeDiskPartScriptWriter -and
            $auditedWinPeDiskPartScriptWriterPatterns -contains $pattern) {
            continue
        }
        $isAuditedBcdStoreTransaction =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedBcdStoreTransactionPath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedBcdStoreTransaction -and
            $auditedBcdStoreTransactionPatterns -contains $pattern) {
            continue
        }
        $relative = $match.Path.Substring($repoRoot.Length).TrimStart('\')
        $failures += "$relative`:$($match.LineNumber) contains $($forbiddenSourcePatterns[$pattern])"
    }
}

$logWriterText = Get-Content -LiteralPath $auditedLogFileWriterPath -Raw
$logWriterRequiredPatterns = [ordered]@{
    '\bCREATE_NEW\b'      = 'CREATE_NEW'
    '\bFILE_SHARE_READ\b' = 'FILE_SHARE_READ'
}
foreach ($pattern in $logWriterRequiredPatterns.Keys) {
    if ($logWriterText -notmatch $pattern) {
        $failures += "CloneCore\src\log.cpp must retain $($logWriterRequiredPatterns[$pattern])"
    }
}

$logWriterForbiddenPatterns = [ordered]@{
    '\bCREATE_ALWAYS\b'       = 'CREATE_ALWAYS'
    '\bOPEN_ALWAYS\b'         = 'OPEN_ALWAYS'
    '\bOPEN_EXISTING\b'       = 'OPEN_EXISTING'
    '\bTRUNCATE_EXISTING\b'   = 'TRUNCATE_EXISTING'
    '\bFILE_SHARE_WRITE\b'    = 'FILE_SHARE_WRITE'
    '\bFILE_SHARE_DELETE\b'   = 'FILE_SHARE_DELETE'
    '\bDeviceIoControl\s*\('  = 'DeviceIoControl'
    '\bPhysicalDrive\b'       = 'PhysicalDrive'
    '\bIOCTL_[A-Z0-9_]+\b'    = 'IOCTL_*'
    '\bFSCTL_[A-Z0-9_]+\b'    = 'FSCTL_*'
}
foreach ($pattern in $logWriterForbiddenPatterns.Keys) {
    if ($logWriterText -match $pattern) {
        $failures += "CloneCore\src\log.cpp contains forbidden log-writer token $($logWriterForbiddenPatterns[$pattern])"
    }
}

$logWriteFileCount = ([regex]::Matches(
        $logWriterText,
        '\bWriteFile\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$logGenericWriteCount = ([regex]::Matches(
        $logWriterText,
        '\bGENERIC_WRITE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($logWriteFileCount -ne 1 -or $logGenericWriteCount -ne 1) {
    $failures += 'CloneCore\src\log.cpp may contain exactly one WriteFile call and one GENERIC_WRITE access request'
}

$winPeDiskPartWriterText = Get-Content `
    -LiteralPath $auditedWinPeDiskPartScriptWriterPath `
    -Raw
$winPeDiskPartWriterRequiredPatterns = [ordered]@{
    '\brun_verified_diskpart_script\b' = 'single audited DiskPart writer function'
    '\brun_diskpart_partition_letter_command\b' = 'audited drive-letter command builder'
    '\brun_diskpart_rescan_command\b' = 'separate post-MBR2GPT DiskPart rescan process'
    '\brun_diskpart_create_msr_command\b' = 'audited MSR command builder'
    'trusted_system_directory\.substr\(0U, 3U\) \+ L"Windows\\\\Temp\\\\"' = 'WinPE system-drive temp path'
    '\bFILE_ATTRIBUTE_REPARSE_POINT\b' = 'temp-directory reparse rejection'
    '\bCREATE_NEW\b' = 'create-new temporary script'
    '\bFILE_ATTRIBUTE_TEMPORARY\b' = 'temporary-file attribute'
    'trusted_system_directory \+ L"\\\\diskpart\.exe"' = 'System32 DiskPart path'
    '\bverify_microsoft_signed\s*\(' = 'Microsoft signature verification'
    '\bselect disk\b' = 'explicit disk selection'
    '\bselect partition\b' = 'explicit partition selection'
    '"rescan\\r\\nexit\\r\\n"' = 'completed DiskPart rescan script before a new MSR process'
    '\brefresh_disk_partition_cache\b' = 'target-only system partition-cache refresh'
    '\bIOCTL_DISK_UPDATE_PROPERTIES\b' = 'documented partition-cache invalidation control'
    'expected_path\.c_str\(\),\s*0,\s*FILE_SHARE_READ \| FILE_SHARE_WRITE' = 'cache refresh handle with zero requested disk access'
    '\brefresh_virtual_disk_service_cache\b' = 'post-conversion VDS cache refresh'
    '\bWaitForServiceReady\s*\(' = 'VDS initialization completion wait'
    '\bReenumerate\s*\(' = 'documented VDS disk reenumeration'
    '\bRefresh\s*\(' = 'documented VDS existing-layout cache refresh'
    '\brestart_virtual_disk_service\b' = 'WinPE-only stale VDS provider restart'
    'L"vds"' = 'exact Microsoft Virtual Disk Service name'
    '\bSERVICE_QUERY_STATUS \| SERVICE_START \| SERVICE_STOP\b' = 'minimal VDS service control access'
    '\bSERVICE_CONTROL_STOP\b' = 'bounded stale VDS provider stop'
    '\bStartServiceW\s*\(' = 'bounded VDS provider restart'
    'create partition msr size=16 offset=' = 'fixed 16 MiB MSR creation command'
    '\bselect_msr_creation_offset\b' = 'bounded MSR free-space selection'
    'partition\.offset_bytes == created_msr_offset\.value\(\)' = 'MSR starting-offset readback'
    'partition\.size_bytes ==\s*kMicrosoftReservedPartitionBytes' = 'MSR size readback'
    '\bverify_direct_partition_handle\s*\(' = 'physical partition readback'
}
foreach ($pattern in $winPeDiskPartWriterRequiredPatterns.Keys) {
    if ($winPeDiskPartWriterText -notmatch $pattern) {
        $failures += "WinPEApp\src\mbr2gpt_job_execution_service.cpp must retain $($winPeDiskPartWriterRequiredPatterns[$pattern])"
    }
}

$winPeDiskPartWriteFileCount = ([regex]::Matches(
        $winPeDiskPartWriterText,
        '\bWriteFile\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$winPeDiskPartGenericWriteCount = ([regex]::Matches(
        $winPeDiskPartWriterText,
        '\bGENERIC_WRITE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$winPeDiskPartDeleteFileCount = ([regex]::Matches(
        $winPeDiskPartWriterText,
        '\bDeleteFileW\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($winPeDiskPartWriteFileCount -ne 1 -or
    $winPeDiskPartGenericWriteCount -ne 1 -or
    $winPeDiskPartDeleteFileCount -ne 2) {
    $failures += 'WinPEApp MBR2GPT service may contain exactly one WriteFile, one GENERIC_WRITE, and two DeleteFileW calls for the audited RAM-disk script'
}

$productVmHarnessPath = Join-Path $repoRoot `
    'scripts\Invoke-ProductMbr2GptVmTest.ps1'
$productVmHarnessText = Get-Content -LiteralPath $productVmHarnessPath -Raw
$productVmHarnessRequiredPatterns = [ordered]@{
    "'--uart1', '0x3F8', '4'" = 'isolated virtual COM1 evidence channel'
    "'--uartmode1', 'file'" = 'host-side UART evidence sink'
    'YDC_TARGET_SECURE_BOOT_PASS_V1' = 'in-guest Secure Boot PASS marker'
    'YDCUART=' = 'delimited UART evidence records'
    'Architecture64=1' = '64-bit target evidence requirement'
    'VM-only pre-staged startup probe' = 'credential-free target validation transport'
    'probePreStagedByVmOnlyVerifier = \$true' = 'explicit VM-only probe staging evidence'
    'validationMediaAttached = \$false' = 'target boot without validation media'
    "'acpipowerbutton'" = 'normal-shutdown request before cleanup'
    'hard power-off was' = 'fail-closed target shutdown policy'
    'intentionally not attempted' = 'explicit no-hard-power-off policy'
    '\bDisable-SerialEvidence\b' = 'UART cleanup and carrier restoration'
}
foreach ($pattern in $productVmHarnessRequiredPatterns.Keys) {
    if ($productVmHarnessText -notmatch $pattern) {
        $failures += "Invoke-ProductMbr2GptVmTest.ps1 must retain $($productVmHarnessRequiredPatterns[$pattern])"
    }
}
foreach ($pattern in @(
    '\bguestcontrol\b',
    '\bpasswordfile\b',
    '\bInvoke-KeyboardRunCommand\b',
    '\bkeyboardputstring\b',
    'target-validation\.viso'
)) {
    if ($productVmHarnessText -match $pattern) {
        $failures += 'Product MBR2GPT target verification must use only the pre-staged probe and UART'
    }
}
foreach ($pattern in @(
    'keyboardputfile',
    'C:\\YdcValidation',
    'YdcValidation\.hex',
    'certutil\.exe -f -decodehex'
)) {
    if ($productVmHarnessText -match $pattern) {
        $failures += 'Product target validation must not stage or launch a host-delivered script'
    }
}

$productMbr2GptVerifierPath = Join-Path $repoRoot `
    'tests\VM\product_mbr2gpt_verifier_vm.cpp'
$productMbr2GptVerifierText = Get-Content `
    -LiteralPath $productMbr2GptVerifierPath -Raw
$productMbr2GptVerifierRequiredPatterns = [ordered]@{
    'YTEC_VM_DESTRUCTIVE_TEST_ONLY' = 'destructive VM-only compile guard'
    'kTargetBytes' = 'fixed 64 GiB target identity'
    'YDC_TARGET_PROBE_STAGED_V1' = 'probe staging completion marker'
    'YDC_TARGET_SECURE_BOOT_PASS_V1' = 'target startup PASS marker'
    'program_data = windows_root \+ L"ProgramData\\\\' = 'fixed target ProgramData root'
    'Microsoft\\\\Windows\\\\Start Menu\\\\Programs\\\\Startup' = 'fixed target-only startup directory'
    '%ProgramData%\\\\YdcMbr2GptValidation\.ps1' = 'non-Startup probe script location'
    '\bCREATE_NEW\b' = 'non-overwriting probe file creation'
    '\bquery_windows_volume_bitmap_bindings\b' = 'physical target volume mapping'
    '\bopen_verified_read_only_physical_disk_with_windows_apis\b' = 'reverified target disk identity'
}
foreach ($pattern in $productMbr2GptVerifierRequiredPatterns.Keys) {
    if ($productMbr2GptVerifierText -notmatch $pattern) {
        $failures += "product_mbr2gpt_verifier_vm.cpp must retain $($productMbr2GptVerifierRequiredPatterns[$pattern])"
    }
}
$hardPowerOffPattern = [regex]::Escape(
    "'controlvm', `$vmName, 'poweroff'")
$hardPowerOffCount = ([regex]::Matches(
        $productVmHarnessText,
        $hardPowerOffPattern,
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($hardPowerOffCount -ne 1 -or
    $productVmHarnessText -notmatch
        'Stop the diskless UEFI NVRAM initialization') {
    $failures += 'The VM harness may hard-power-off only the diskless UEFI NVRAM initialization'
}

$allowedCreateProcessPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'BootRepair\src\bcdboot.cpp'))
$processLaunchMatches = $sourceFiles |
    Select-String -Pattern '\bCreateProcessW\s*\(' -CaseSensitive
foreach ($match in $processLaunchMatches) {
    if (-not [IO.Path]::GetFullPath($match.Path).Equals(
            $allowedCreateProcessPath,
            [StringComparison]::OrdinalIgnoreCase)) {
        $relative = $match.Path.Substring($repoRoot.Length).TrimStart('\')
        $failures += "$relative`:$($match.LineNumber) contains an unapproved process launch"
    }
}

$excludedRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'out'))
$forbiddenMicrosoftNames = @(
    'mbr2gpt.exe'
    'bcdboot.exe'
    'dism.exe'
    'diskpart.exe'
    'reagentc.exe'
    'copype.cmd'
    'MakeWinPEMedia.cmd'
    'oscdimg.exe'
)
$forbiddenExtensions = @('.wim', '.iso', '.cab')

$repositoryFiles = @(
    Get-ChildItem -LiteralPath $repoRoot -File
)
$topLevelDirectories = Get-ChildItem -LiteralPath $repoRoot -Directory |
    Where-Object {
        -not $_.FullName.Equals(
            $excludedRoot,
            [StringComparison]::OrdinalIgnoreCase)
    }
foreach ($directory in $topLevelDirectories) {
    if ($directory.Name -ne '.validation') {
        $repositoryFiles += Get-ChildItem `
            -LiteralPath $directory.FullName `
            -Recurse `
            -File
        continue
    }

    # VM credentials belong to the shared lab and are deliberately
    # inaccessible to repository checks. Scan every other validation asset,
    # but never request access to .validation\vm-secrets.
    $repositoryFiles += Get-ChildItem -LiteralPath $directory.FullName -File
    $validationDirectories =
        Get-ChildItem -LiteralPath $directory.FullName -Directory |
        Where-Object { $_.Name -ne 'vm-secrets' }
    foreach ($validationDirectory in $validationDirectories) {
        $repositoryFiles += Get-ChildItem `
            -LiteralPath $validationDirectory.FullName `
            -Recurse `
            -File
    }
}
foreach ($file in $repositoryFiles) {
    if ($forbiddenMicrosoftNames -contains $file.Name -or
        $forbiddenExtensions -contains $file.Extension.ToLowerInvariant()) {
        $relative = $file.FullName.Substring($repoRoot.Length).TrimStart('\')
        $failures += "禁止された Microsoft / WinPE 配布物の候補: $relative"
    }
}

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Error $_ }
    throw 'Safety boundary check: FAIL'
}

Write-Output 'Safety boundary check: PASS (audited writers plus VM-only UART validation and normal-shutdown boundary enforced)'
