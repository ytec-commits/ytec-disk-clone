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
$auditedShrinkBundleWriterPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'MigrationEngine\src\bundle_capture.cpp'))
$auditedShrinkBundleWriterPatterns = @(
    '\bWriteFile\s*\('
    '\bGENERIC_WRITE\b'
    '\bMoveFileExW\s*\('
    '\bDeleteFileW\s*\('
)
$auditedOnlineShrinkBackupWriterPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WindowsApp\src\online_shrink_backup_job.cpp'))
$auditedOnlineShrinkBackupWriterPatterns = @(
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
        $isAuditedShrinkBundleWriter =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedShrinkBundleWriterPath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedShrinkBundleWriter -and
            $auditedShrinkBundleWriterPatterns -contains $pattern) {
            continue
        }
        $isAuditedOnlineShrinkBackupWriter =
            [IO.Path]::GetFullPath($match.Path).Equals(
                $auditedOnlineShrinkBackupWriterPath,
                [StringComparison]::OrdinalIgnoreCase)
        if ($isAuditedOnlineShrinkBackupWriter -and
            $auditedOnlineShrinkBackupWriterPatterns -contains $pattern) {
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

$shrinkBundleWriterText = Get-Content `
    -LiteralPath $auditedShrinkBundleWriterPath `
    -Raw
$shrinkBundleRequiredPatterns = [ordered]@{
    '\bCREATE_NEW\b' = 'non-overwriting manifest creation'
    '\bFILE_FLAG_WRITE_THROUGH\b' = 'write-through manifest creation'
    '\bFlushFileBuffers\s*\(' = 'manifest flush before verification'
    '\bMOVEFILE_WRITE_THROUGH\b' = 'write-through final directory commit'
    '\bverify_shrink_bundle_read_only\s*\(' = 'full bundle verification before and after commit'
    '\bFILE_ATTRIBUTE_REPARSE_POINT\b' = 'scratch reparse-point rejection'
}
foreach ($pattern in $shrinkBundleRequiredPatterns.Keys) {
    if ($shrinkBundleWriterText -notmatch $pattern) {
        $failures += "MigrationEngine\src\bundle_capture.cpp must retain $($shrinkBundleRequiredPatterns[$pattern])"
    }
}
$shrinkBundleWriteFileCount = ([regex]::Matches(
        $shrinkBundleWriterText,
        '\bWriteFile\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$shrinkBundleGenericWriteCount = ([regex]::Matches(
        $shrinkBundleWriterText,
        '\bGENERIC_WRITE\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$shrinkBundleMoveCount = ([regex]::Matches(
        $shrinkBundleWriterText,
        '\bMoveFileExW\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$shrinkBundleDeleteCount = ([regex]::Matches(
        $shrinkBundleWriterText,
        '\bDeleteFileW\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($shrinkBundleWriteFileCount -ne 1 -or
    $shrinkBundleGenericWriteCount -ne 1 -or
    $shrinkBundleMoveCount -ne 1 -or
    $shrinkBundleDeleteCount -ne 1) {
    $failures += 'MigrationEngine bundle writer may contain exactly one WriteFile, GENERIC_WRITE, MoveFileExW, and DeleteFileW call'
}

$onlineShrinkBackupWriterText = Get-Content `
    -LiteralPath $auditedOnlineShrinkBackupWriterPath `
    -Raw
$onlineShrinkRequiredPatterns = [ordered]@{
    'GetFileAttributesW\(request\.final_bundle_directory\.c_str\(\)\)\s*!=\s*\r?\n?\s*INVALID_FILE_ATTRIBUTES' = 'final-path no-overwrite gate'
    '\bquery_single_disk_number_for_local_path\s*\(' = 'destination physical-disk resolution'
    'destination_disk\.value\(\)\s*==\s*source_disk_number' = 'source and destination physical separation'
    '\bconst auto separated\s*=\s*validate_destination_disk_separation' = 'destination revalidation immediately before snapshot capture'
    'destination_still_separated' = 'destination revalidation before final-name commit'
    '\bMOVEFILE_WRITE_THROUGH\b' = 'post-VSS write-through final directory commit'
    '\bverify_shrink_bundle_read_only\s*\(' = 'full final bundle verification'
    '\bfinal_bundle_committed_after_vss_cleanup\s*=\s*true' = 'post-cleanup completion evidence'
}
foreach ($pattern in $onlineShrinkRequiredPatterns.Keys) {
    if ($onlineShrinkBackupWriterText -notmatch $pattern) {
        $failures += "WindowsApp\src\online_shrink_backup_job.cpp must retain $($onlineShrinkRequiredPatterns[$pattern])"
    }
}
$onlineShrinkMoveCount = ([regex]::Matches(
        $onlineShrinkBackupWriterText,
        '\bMoveFileExW\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
$onlineShrinkDeleteCount = ([regex]::Matches(
        $onlineShrinkBackupWriterText,
        '\bDeleteFileW\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($onlineShrinkMoveCount -ne 1 -or $onlineShrinkDeleteCount -ne 2) {
    $failures += 'WindowsApp online shrink backup may contain exactly one MoveFileExW and two DeleteFileW calls'
}
$onlineShrinkSeparationCheckCount = ([regex]::Matches(
        $onlineShrinkBackupWriterText,
        '\bvalidate_destination_disk_separation\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($onlineShrinkSeparationCheckCount -ne 4) {
    $failures += 'WindowsApp online shrink backup must define one destination separation gate and call it at all three write boundaries'
}

$shrinkExecutionServicePath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'WinPEApp\src\shrink_execution_service.cpp'))
$shrinkExecutionServiceText = Get-Content -LiteralPath $shrinkExecutionServicePath -Raw
$systemScratchCheckCount = ([regex]::Matches(
        $shrinkExecutionServiceText,
        '\bvalidate_system_scratch_directory\s*\(',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($systemScratchCheckCount -ne 4 -or
    $shrinkExecutionServiceText -notmatch '\bFILE_ATTRIBUTE_REPARSE_POINT\b') {
    $failures += 'WinPE shrink execution must define one system-drive scratch gate and call it before all capture/restore target-write flows'
}

$shrinkVolumeApplyPath = [IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'MigrationEngine\src\volume_apply.cpp'))
$shrinkVolumeApplyText = Get-Content -LiteralPath $shrinkVolumeApplyPath -Raw
$shrinkVolumeApplyRequiredPatterns = [ordered]@{
    '\bQueryDosDeviceW\b' = 'target Volume GUID to NT device resolution'
    '\bDefineDosDeviceW\b' = 'temporary exact Volume GUID discovery link'
    '\bGetVolumeNameForVolumeMountPointW\b' = 'Volume GUID identity verification'
    '\bSetVolumeMountPointW\b' = 'Mount Manager drive-letter assignment'
    '\bDeleteVolumeMountPointW\b' = 'owned Mount Manager drive-letter cleanup'
    'DDD_EXACT_MATCH_ON_REMOVE' = 'exact temporary DOS-device cleanup'
    'status = execute_format\(\s*mount\.root\(\)' = 'verified Mount Manager drive root FORMAT target'
    '\bdecode_process_diagnostic\b' = 'bounded Microsoft FORMAT diagnostics'
}
foreach ($pattern in $shrinkVolumeApplyRequiredPatterns.Keys) {
    if ($shrinkVolumeApplyText -notmatch $pattern) {
        $failures += "MigrationEngine\src\volume_apply.cpp must retain $($shrinkVolumeApplyRequiredPatterns[$pattern])"
    }
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

$rescueMediaBuilderPath = Join-Path $repoRoot `
    'scripts\New-WinPEAppValidationMedia.ps1'
$rescueMediaBuilderText = Get-Content `
    -LiteralPath $rescueMediaBuilderPath `
    -Raw
$rescueUsbInitializationPatterns = [ordered]@{
    'function Initialize-VerifiedUsbTarget' = 'single target-bound USB initialization function'
    '-AllowUnpartitioned' = 'explicit zero-partition recovery gate'
    '\bGet-VerifiedUsbDisk\b' = 'stable USB identity recheck before and after clearing'
    'Clear-Disk\s+`\s*-InputObject \$before\.disk' = 'clear only the already verified USB object'
    'Initialize-Disk\s+`\s*-InputObject \$cleared\.disk\s+`\s*-PartitionStyle MBR' = 'initialize only the reverified RAW USB object as MBR'
    'Set-Disk\s+`\s*-InputObject \$cleared\.disk\s+`\s*-PartitionStyle MBR' = 'convert only the empty reverified GPT USB object to MBR'
    '\$clearedPartitions\.Count -ne 0' = 'require zero partitions after clearing before any style operation'
    '\$maximumFat32Bytes = \[UInt64\]\(30GB\)' = 'bounded FAT32 partition for large USB media'
    'Format-Volume\s+`[\s\S]*?-Partition \$partition\s+`[\s\S]*?-FileSystem FAT32' = 'format only the newly created partition as FAT32'
    '-RemoveData\s+`\s*-RemoveOEM\s+`\s*-Confirm:\$false' = 'explicit whole-target erase after confirmation'
    '-RequireMbr' = 'post-initialization MBR readback gate'
}
foreach ($pattern in $rescueUsbInitializationPatterns.Keys) {
    if ($rescueMediaBuilderText -notmatch $pattern) {
        $failures += "New-WinPEAppValidationMedia.ps1 must retain $($rescueUsbInitializationPatterns[$pattern])"
    }
}
foreach ($singleWriter in @(
        'Clear-Disk', 'Initialize-Disk', 'Set-Disk', 'Format-Volume')) {
    $writerCount = ([regex]::Matches(
            $rescueMediaBuilderText,
            "\b${singleWriter}\b")).Count
    if ($writerCount -ne 1) {
        $failures += "New-WinPEAppValidationMedia.ps1 may contain exactly one $singleWriter USB writer"
    }
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

$productDataShrinkVmHarnessPath = Join-Path $repoRoot `
    'scripts\Invoke-ProductDataShrinkVmTest.ps1'
$productDataShrinkVmHarnessText = Get-Content `
    -LiteralPath $productDataShrinkVmHarnessPath `
    -Raw
$productDataShrinkVmHarnessRequiredPatterns = [ordered]@{
    '\$expectedVmUuid = ''c921017d-c4e3-4f07-b569-b5c89286d5b1''' = 'fixed dedicated VM UUID'
    '\bInvoke-VerifiedStuckVmRecovery\b' = 'bounded stopping-state recovery'
    '-Filter "Name = ''VirtualBoxVM\.exe''"' = 'VirtualBoxVM-only process enumeration'
    '\$commandLine\.IndexOf\(\s*\$vmName' = 'process command-line VM-name verification'
    '\$commandLine\.IndexOf\(\s*\$ExpectedUuid' = 'process command-line UUID verification'
    '\$allVmProcesses\.Count -eq 0' = 'no-process fail-closed gate'
    '対象外のVirtualBoxVMプロセスがあるため、強制終了しません。' = 'other-process fail-closed gate'
    '\bStop-Process -Id \$processId -Force\b' = 'verified process-only termination'
    '\$afterWaitState -ceq ''stopping''' = 'stopping-only forced recovery gate'
    '\bhost-shutdown-recovery\.json\b' = 'forced-recovery evidence'
}
foreach ($pattern in $productDataShrinkVmHarnessRequiredPatterns.Keys) {
    if ($productDataShrinkVmHarnessText -notmatch $pattern) {
        $failures += "Invoke-ProductDataShrinkVmTest.ps1 must retain $($productDataShrinkVmHarnessRequiredPatterns[$pattern])"
    }
}
$dataShrinkStopProcessCount = ([regex]::Matches(
        $productDataShrinkVmHarnessText,
        '\bStop-Process\s+-Id\s+\$processId\s+-Force\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($dataShrinkStopProcessCount -ne 1 -or
    $productDataShrinkVmHarnessText -match
        '\bcontrolvm\s+[^\r\n]+\s+poweroff\b') {
    $failures += 'Product data shrink VM recovery must terminate only verified processes after ACPI shutdown, never use VBoxManage hard poweroff'
}

$productDataShrinkGuestRunnerPath = Join-Path $repoRoot `
    'scripts\vm\Run-ProductDataShrinkValidationElevated.ps1'
$productDataShrinkGuestRunnerText = Get-Content `
    -LiteralPath $productDataShrinkGuestRunnerPath `
    -Raw
$productDataShrinkGuestRunnerRequiredPatterns = [ordered]@{
    '\bfunction Get-NoAutoMountValue\b' = 'missing-value-safe automount reader'
    '\$settings\.PSObject\.Properties\[''NoAutoMount''\]' = 'StrictMode-safe property lookup'
    '\$null -eq \$property' = 'missing NoAutoMount value gate'
    '\breturn 0\b' = 'Windows default automount-enabled value'
    '\$vmShellIsolationApplied = \$true\s+\$mountvolOutput = @\(& "\$env:SystemRoot\\System32\\mountvol\.exe" /N' = 'restore-required marker before automount disable'
    'Get-NoAutoMountValue[\s\S]+-ne 1' = 'automount-disable readback'
    'Get-NoAutoMountValue[\s\S]+-ne\s+0' = 'automount-restore readback'
    '\$restoreIssues \+= ''NoAutoMountが0へ戻っていません。''' = 'failed-restore evidence'
}
foreach ($pattern in $productDataShrinkGuestRunnerRequiredPatterns.Keys) {
    if ($productDataShrinkGuestRunnerText -notmatch $pattern) {
        $failures += "Run-ProductDataShrinkValidationElevated.ps1 must retain $($productDataShrinkGuestRunnerRequiredPatterns[$pattern])"
    }
}
$noAutoMountReaderCount = ([regex]::Matches(
        $productDataShrinkGuestRunnerText,
        '\bGet-NoAutoMountValue\b',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)).Count
if ($noAutoMountReaderCount -ne 5) {
    $failures += 'Product data shrink guest runner must define one NoAutoMount reader and use it at all four isolation boundaries'
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
