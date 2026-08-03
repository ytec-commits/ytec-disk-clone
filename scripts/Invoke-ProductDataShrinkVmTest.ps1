[CmdletBinding()]
param(
    [ValidateRange(30, 90)]
    [int] $TimeoutMinutes = 60,
    [switch] $AllowVerifiedAbortedRecovery
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workspaceRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot '..\..'))
$labRoot = Join-Path $workspaceRoot 'business-apps\ytec-windows-backup'
$credentialRoot = Join-Path $labRoot '.validation\vm-secrets'
$vboxManage = 'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe'
$vmName = 'YDC-Phase5-VSS-x64'
$expectedVmUuid = 'c921017d-c4e3-4f07-b569-b5c89286d5b1'
$guestUser = 'YbcTest'
$passwordFile = Join-Path $credentialRoot `
    'YWB-Win10-22H2-x64-Clean.password.txt'
$harness = Join-Path $repoRoot `
    'out\build\msvc-x64-vm-destructive\tests\ytec-product-data-shrink-vm.exe'
$guestRunnerSource = Join-Path $repoRoot `
    'scripts\vm\Run-ProductDataShrinkValidationElevated.ps1'
$vmFolder = Join-Path $repoRoot ".validation\vms\$vmName"
$runStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$evidenceRoot = Join-Path $repoRoot `
    ".validation\evidence\product-data-shrink-vm\$runStamp"
$sourceDisk = Join-Path $vmFolder `
    "YDC-Data-Shrink-Source-4GiB-$runStamp.vdi"
$targetDisk = Join-Path $vmFolder `
    "YDC-Data-Shrink-Target-2GiB-$runStamp.vdi"
$guestRoot = "C:\Users\$guestUser\YDC-Data-Shrink-$runStamp"
$guestHarness = "$guestRoot\ytec-product-data-shrink-vm.exe"
$guestRunner = "$guestRoot\Run-ProductDataShrinkValidationElevated.ps1"
$guestLauncher = `
    "C:\Users\$guestUser\Desktop\YDC_DATA_SHRINK_$runStamp.cmd"
$guestDone = "$guestRoot\done.txt"
$guestArtifacts = @(
    'summary.json',
    'done.txt',
    'harness-stdout.txt',
    'harness-stderr.txt',
    'disks-before.json',
    'disks-after.json',
    'shadows-before.json',
    'shadows-after.json'
)
$started = $false
$attached = $false
$poweredOff = $false
$forcedStuckShutdownRecovery = $false
$originalPorts = @{}

foreach ($requiredFile in @(
    $vboxManage,
    $passwordFile,
    $harness,
    $guestRunnerSource
)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "必要ファイルがありません: $requiredFile"
    }
}
if (-not (Test-Path -LiteralPath $vmFolder -PathType Container)) {
    throw "Phase 5専用VMフォルダーがありません: $vmFolder"
}

function Invoke-VBox {
    param(
        [Parameter(Mandatory)]
        [string[]] $Arguments,
        [Parameter(Mandatory)]
        [string] $Operation
    )

    $output = @(& $vboxManage @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "VirtualBox操作が失敗しました: $Operation`n$($output -join "`n")"
    }
    return $output
}

function Invoke-GuestControlTimed {
    param(
        [Parameter(Mandatory)]
        [string[]] $Arguments,
        [ValidateRange(5, 120)]
        [int] $TimeoutSeconds = 30
    )

    $process = Start-Process `
        -FilePath $vboxManage `
        -ArgumentList (@('guestcontrol', $vmName) + $Arguments) `
        -PassThru `
        -WindowStyle Hidden
    try {
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            try {
                $process.Kill()
                $process.WaitForExit()
            }
            catch {
                # Timeoutを正本にする。
            }
            return [pscustomobject]@{
                ExitCode = 124
                TimedOut = $true
            }
        }
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            TimedOut = $false
        }
    }
    finally {
        $process.Dispose()
    }
}

function Get-MachineValue {
    param(
        [Parameter(Mandatory)]
        [string[]] $Information,
        [Parameter(Mandatory)]
        [string] $Name
    )

    $line = $Information |
        Where-Object { $_ -like "$Name=*" } |
        Select-Object -First 1
    if ($line -match '^[^=]+="(.*)"$') {
        return $Matches[1]
    }
    return $null
}

function Get-AttachedMedium {
    param(
        [Parameter(Mandatory)]
        [string[]] $Information,
        [Parameter(Mandatory)]
        [ValidateRange(0, 29)]
        [int] $Port
    )

    $key = '"SATA-' + $Port + '-0"'
    $line = $Information |
        Where-Object { $_ -like "$key=*" } |
        Select-Object -First 1
    if ($line -match '^"SATA-[0-9]+-0"="(.*)"$') {
        return $Matches[1]
    }
    return $null
}

function Wait-GuestReady {
    $deadline = (Get-Date).AddMinutes(12)
    while ((Get-Date) -lt $deadline) {
        $probe = Invoke-GuestControlTimed `
            -Arguments @(
                'stat',
                "--username=$guestUser",
                "--passwordfile=$passwordFile",
                'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'
            ) `
            -TimeoutSeconds 20
        if ($probe.ExitCode -eq 0) {
            return
        }
        $null = Invoke-GuestControlTimed `
            -Arguments @('closesession', '--all') `
            -TimeoutSeconds 20
        Start-Sleep -Seconds 10
    }
    throw "VMのGuestControl準備を確認できませんでした: $vmName"
}

function Invoke-GuestRetry {
    param(
        [Parameter(Mandatory)]
        [string[]] $Arguments,
        [Parameter(Mandatory)]
        [string] $Operation
    )

    $lastResult = $null
    foreach ($attempt in 1..8) {
        $lastResult = Invoke-GuestControlTimed `
            -Arguments $Arguments `
            -TimeoutSeconds 60
        if ($lastResult.ExitCode -eq 0) {
            return
        }
        Start-Sleep -Seconds 3
    }
    throw "GuestControlが失敗しました: $Operation; " +
        "exitCode=$($lastResult.ExitCode); timedOut=$($lastResult.TimedOut)"
}

function Copy-GuestArtifact {
    param(
        [Parameter(Mandatory)]
        [string] $GuestPath,
        [Parameter(Mandatory)]
        [string] $HostPath
    )

    $null = Invoke-GuestRetry `
        -Operation "証跡回収 $GuestPath" `
        -Arguments @(
            'copyfrom',
            "--username=$guestUser",
            "--passwordfile=$passwordFile",
            $GuestPath,
            $HostPath
        )
}

function Wait-PoweredOff {
    param([ValidateRange(1, 10)][int] $Minutes = 5)

    $deadline = (Get-Date).AddMinutes($Minutes)
    while ((Get-Date) -lt $deadline) {
        $info = @(& $vboxManage showvminfo `
            $vmName --machinereadable 2>&1)
        if ($LASTEXITCODE -eq 0 -and
            (Get-MachineValue -Information $info -Name 'VMState') -in
                @('poweroff', 'aborted')) {
            return $true
        }
        Start-Sleep -Seconds 5
    }
    return $false
}

function Invoke-VerifiedStuckVmRecovery {
    param(
        [Parameter(Mandatory)]
        [string] $ExpectedUuid
    )

    $stuckInfo = @(& $vboxManage showvminfo `
        $vmName --machinereadable 2>&1)
    if ($LASTEXITCODE -ne 0 -or
        (Get-MachineValue -Information $stuckInfo -Name 'VMState') -cne
            'stopping' -or
        (Get-MachineValue -Information $stuckInfo -Name 'UUID') -cne
            $ExpectedUuid) {
        throw '固定VMのstopping状態とUUIDを再確認できないため、強制終了しません。'
    }

    $runningEntries = @(& $vboxManage list runningvms 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw '強制終了前に稼働中VirtualBox VMを再確認できません。'
    }
    foreach ($entry in $runningEntries) {
        if ($entry.IndexOf(
                $vmName,
                [StringComparison]::OrdinalIgnoreCase) -lt 0 -or
            $entry.IndexOf(
                $ExpectedUuid,
                [StringComparison]::OrdinalIgnoreCase) -lt 0) {
            throw '別VMが稼働中のため、固定VMを強制終了しません。'
        }
    }

    $expectedExecutable = [IO.Path]::GetFullPath(
        (Join-Path (Split-Path -Parent $vboxManage) 'VirtualBoxVM.exe'))
    $allVmProcesses = @(Get-CimInstance `
        -ClassName Win32_Process `
        -Filter "Name = 'VirtualBoxVM.exe'" `
        -ErrorAction Stop)
    if ($allVmProcesses.Count -eq 0) {
        throw '照合できるVirtualBoxVMプロセスがないため、強制終了しません。'
    }
    foreach ($process in $allVmProcesses) {
        $commandLine = [string]$process.CommandLine
        $executablePath = [string]$process.ExecutablePath
        if ([string]::IsNullOrWhiteSpace($commandLine) -or
            [string]::IsNullOrWhiteSpace($executablePath) -or
            -not [IO.Path]::GetFullPath($executablePath).Equals(
                $expectedExecutable,
                [StringComparison]::OrdinalIgnoreCase) -or
            $commandLine.IndexOf(
                $vmName,
                [StringComparison]::OrdinalIgnoreCase) -lt 0 -or
            $commandLine.IndexOf(
                $ExpectedUuid,
                [StringComparison]::OrdinalIgnoreCase) -lt 0) {
            throw '対象外のVirtualBoxVMプロセスがあるため、強制終了しません。'
        }
    }

    $processIds = @($allVmProcesses.ProcessId | Sort-Object -Descending)
    foreach ($processId in $processIds) {
        # A verified VirtualBoxVM process may exit naturally between discovery
        # and termination. The post-loop process/state checks remain authoritative.
        Stop-Process -Id $processId -Force -ErrorAction SilentlyContinue
    }

    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline) {
        $remaining = @(Get-CimInstance `
            -ClassName Win32_Process `
            -Filter "Name = 'VirtualBoxVM.exe'" `
            -ErrorAction Stop)
        $afterInfo = @(& $vboxManage showvminfo `
            $vmName --machinereadable 2>&1)
        if ($LASTEXITCODE -eq 0 -and
            $remaining.Count -eq 0 -and
            (Get-MachineValue -Information $afterInfo -Name 'VMState') -in
                @('poweroff', 'aborted')) {
            return [pscustomobject]@{
                attempted = $true
                recovered = $true
                vmUuid = $ExpectedUuid
                processIds = $processIds
                completedUtc = [DateTimeOffset]::UtcNow
            }
        }
        Start-Sleep -Seconds 1
    }
    throw '対象プロセス終了後も固定VMの停止を確認できませんでした。'
}

$running = @(& $vboxManage list runningvms 2>&1)
if ($LASTEXITCODE -ne 0) {
    throw '稼働中VirtualBox VMを確認できませんでした。'
}
if ($running.Count -ne 0) {
    throw "別VMが稼働中のため開始しません: $($running -join ', ')"
}
$initialInfo = @(Invoke-VBox `
    -Arguments @('showvminfo', $vmName, '--machinereadable') `
    -Operation 'データ縮小専用VM事前確認')
$initialState = Get-MachineValue `
    -Information $initialInfo `
    -Name 'VMState'
if ((Get-MachineValue -Information $initialInfo -Name 'UUID') -cne
        $expectedVmUuid) {
    throw 'データ縮小専用VMの固定UUIDが一致しません。'
}
if ($initialState -eq 'aborted' -and
    -not $AllowVerifiedAbortedRecovery) {
    throw 'aborted状態です。既知の検証VMだけ-AllowVerifiedAbortedRecoveryで通常起動できます。'
}
if ($initialState -notin @('poweroff', 'aborted')) {
    throw "VMの開始状態が安全条件と一致しません: $initialState"
}
if ((Get-MachineValue -Information $initialInfo -Name 'nic1') -ne 'none' -or
    (Get-MachineValue -Information $initialInfo -Name 'firmware') -ne 'EFI64') {
    throw 'VMのNICまたはUEFI設定が安全条件と一致しません。'
}
foreach ($port in 0..3) {
    $medium = Get-AttachedMedium -Information $initialInfo -Port $port
    if ([string]::IsNullOrWhiteSpace($medium) -or $medium -eq 'none') {
        throw "既存SATA port $port が空のため開始しません。"
    }
    $originalPorts[$port] = [IO.Path]::GetFullPath($medium)
}
foreach ($port in 4..5) {
    $medium = Get-AttachedMedium -Information $initialInfo -Port $port
    if (-not [string]::IsNullOrWhiteSpace($medium) -and $medium -ne 'none') {
        throw "新規検証用SATA port $port が既に使用中です。"
    }
}

New-Item -ItemType Directory -Path $evidenceRoot -Force | Out-Null
$initialInfo | Set-Content `
    -LiteralPath (Join-Path $evidenceRoot 'vm-before.txt') `
    -Encoding utf8

try {
    $null = Invoke-VBox `
        -Arguments @(
            'createmedium', 'disk',
            '--filename', $sourceDisk,
            '--size', '4096',
            '--format', 'VDI',
            '--variant', 'Standard'
        ) `
        -Operation '4GiBデータ原本VDI作成'
    $null = Invoke-VBox `
        -Arguments @(
            'createmedium', 'disk',
            '--filename', $targetDisk,
            '--size', '2048',
            '--format', 'VDI',
            '--variant', 'Standard'
        ) `
        -Operation '2GiB縮小復元先VDI作成'
    foreach ($attachment in @(
        @{ Port = '4'; Medium = $sourceDisk; Name = '4GiB原本' }
        @{ Port = '5'; Medium = $targetDisk; Name = '2GiB復元先' }
    )) {
        $null = Invoke-VBox `
            -Arguments @(
                'storageattach', $vmName,
                '--storagectl', 'SATA',
                '--port', $attachment.Port,
                '--device', '0',
                '--type', 'hdd',
                '--medium', $attachment.Medium,
                '--nonrotational', 'on'
            ) `
            -Operation "$($attachment.Name)VDI接続"
    }
    $attached = $true
    $attachedInfo = @(Invoke-VBox `
        -Arguments @('showvminfo', $vmName, '--machinereadable') `
        -Operation 'データ縮小VDI接続後確認')
    foreach ($port in 0..3) {
        $actual = Get-AttachedMedium `
            -Information $attachedInfo `
            -Port $port
        if ([string]::IsNullOrWhiteSpace($actual) -or
            -not [IO.Path]::GetFullPath($actual).Equals(
                $originalPorts[$port],
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "既存SATA port $port が変更されています。"
        }
    }
    foreach ($expected in @(
        @{ Port = 4; Path = $sourceDisk }
        @{ Port = 5; Path = $targetDisk }
    )) {
        $actual = Get-AttachedMedium `
            -Information $attachedInfo `
            -Port $expected.Port
        if ([string]::IsNullOrWhiteSpace($actual) -or
            -not [IO.Path]::GetFullPath($actual).Equals(
                [IO.Path]::GetFullPath($expected.Path),
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "新規SATA port $($expected.Port) が一致しません。"
        }
    }

    $harnessHash = (
        Get-FileHash -LiteralPath $harness -Algorithm SHA256).Hash
    $stagingRoot = Join-Path $evidenceRoot 'guest-script-utf8-bom'
    New-Item -ItemType Directory -Path $stagingRoot -Force | Out-Null
    $stagedRunner = Join-Path $stagingRoot `
        ([IO.Path]::GetFileName($guestRunnerSource))
    [IO.File]::WriteAllText(
        $stagedRunner,
        [IO.File]::ReadAllText($guestRunnerSource),
        [Text.UTF8Encoding]::new($true))
    $hostLauncher = Join-Path $evidenceRoot `
        "YDC_DATA_SHRINK_$runStamp.cmd"
    $guestRunnerArguments =
        "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File $guestRunner " +
        "-ExpectedHarnessSha256 $harnessHash"
    $launcherContent = @"
@echo off
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe' -Verb RunAs -ArgumentList '$guestRunnerArguments' -WindowStyle Hidden"
exit /b
"@
    [IO.File]::WriteAllText(
        $hostLauncher,
        $launcherContent,
        [Text.ASCIIEncoding]::new())

    $null = Invoke-VBox `
        -Arguments @('startvm', $vmName, '--type', 'gui') `
        -Operation 'データ縮小統合VM起動'
    $started = $true
    Wait-GuestReady
    $null = Invoke-GuestRetry `
        -Operation 'ゲストデータ縮小検証ディレクトリ作成' `
        -Arguments @(
            'mkdir',
            '--parents',
            "--username=$guestUser",
            "--passwordfile=$passwordFile",
            $guestRoot
        )
    foreach ($copy in @(
        @{ Host = $harness; Guest = $guestHarness }
        @{ Host = $stagedRunner; Guest = $guestRunner }
        @{ Host = $hostLauncher; Guest = $guestLauncher }
    )) {
        $null = Invoke-GuestRetry `
            -Operation "検証ファイル配置 $($copy.Guest)" `
            -Arguments @(
                'copyto',
                "--username=$guestUser",
                "--passwordfile=$passwordFile",
                $copy.Host,
                $copy.Guest
            )
    }

    [ordered]@{
        schemaVersion = 1
        status = 'PREPARED'
        vmName = $vmName
        previousVmState = $initialState
        verifiedAbortedRecovery = [bool]$AllowVerifiedAbortedRecovery
        guestRoot = $guestRoot
        guestLauncher = $guestLauncher
        sourceDisk = $sourceDisk
        sourceDiskBytes = [UInt64](4GB)
        targetDisk = $targetDisk
        targetDiskBytes = [UInt64](2GB)
        harnessSha256 = $harnessHash
        nic = 'none'
        firmware = 'EFI64'
        physicalDiskOrUsbUsed = $false
        preparedUtc = [DateTimeOffset]::UtcNow
    } | ConvertTo-Json -Depth 5 | Set-Content `
        -LiteralPath (Join-Path $evidenceRoot 'host-preparation.json') `
        -Encoding utf8

    Write-Output 'MANUAL_UAC_REQUIRED'
    Write-Output "VM: $vmName"
    Write-Output "Launcher: $guestLauncher"
    Write-Output "Evidence: $evidenceRoot"

    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    $doneFound = $false
    while ((Get-Date) -lt $deadline) {
        $probe = Invoke-GuestControlTimed `
            -Arguments @(
                'stat',
                "--username=$guestUser",
                "--passwordfile=$passwordFile",
                $guestDone
            ) `
            -TimeoutSeconds 20
        if ($probe.ExitCode -eq 0) {
            $doneFound = $true
            break
        }
        Start-Sleep -Seconds 10
    }
    if (-not $doneFound) {
        throw "UAC承認後のデータ縮小VM試験が${TimeoutMinutes}分以内に完了しませんでした。"
    }
    foreach ($artifact in $guestArtifacts) {
        $guestArtifact = "$guestRoot\$artifact"
        $artifactProbe = Invoke-GuestControlTimed `
            -Arguments @(
                'stat',
                "--username=$guestUser",
                "--passwordfile=$passwordFile",
                $guestArtifact
            ) `
            -TimeoutSeconds 20
        if ($artifactProbe.ExitCode -eq 0) {
            Copy-GuestArtifact `
                -GuestPath $guestArtifact `
                -HostPath (Join-Path $evidenceRoot $artifact)
        }
    }
    $done = (
        Get-Content -LiteralPath (Join-Path $evidenceRoot 'done.txt') -Raw
    ).Trim()
    $summary = Get-Content `
        -LiteralPath (Join-Path $evidenceRoot 'summary.json') `
        -Raw | ConvertFrom-Json
    if ($done -ne 'PASS' -or
        $summary.result -ne 'PASS' -or
        $summary.sourceLargerThanTarget -ne $true -or
        $summary.dataOnly -ne $true -or
        $summary.sourceUnchanged -ne $true -or
        $summary.fileContentMatched -ne $true -or
        $summary.bootFinalizationRequired -ne $false -or
        $summary.vmShellIsolationRestored -ne $true -or
        [int]$summary.shadowCopiesBefore -ne 0 -or
        [int]$summary.shadowCopiesAfter -ne 0) {
        $summaryMessage = if (
            $summary.PSObject.Properties.Name -contains 'message') {
            [string]$summary.message
        }
        else {
            'PASS証跡の必須項目が一致しません。'
        }
        throw "データ縮小VM試験が失敗しました: $summaryMessage"
    }
}
finally {
    $shutdownRecovery = $null
    if ($started) {
        $currentInfo = @(& $vboxManage showvminfo `
            $vmName --machinereadable 2>&1)
        $currentState = if ($LASTEXITCODE -eq 0) {
            Get-MachineValue `
                -Information $currentInfo `
                -Name 'VMState'
        }
        else {
            $null
        }
        if ($currentState -in @('poweroff', 'aborted')) {
            $poweredOff = $true
        }
        elseif ($currentState -eq 'running') {
            $null = & $vboxManage controlvm `
                $vmName acpipowerbutton 2>&1
            if ($LASTEXITCODE -eq 0) {
                $poweredOff = Wait-PoweredOff -Minutes 5
            }
        }
        if (-not $poweredOff) {
            $afterWaitInfo = @(& $vboxManage showvminfo `
                $vmName --machinereadable 2>&1)
            $afterWaitState = if ($LASTEXITCODE -eq 0) {
                Get-MachineValue `
                    -Information $afterWaitInfo `
                    -Name 'VMState'
            }
            else {
                $null
            }
            if ($afterWaitState -in @('poweroff', 'aborted')) {
                $poweredOff = $true
            }
            elseif ($afterWaitState -ceq 'stopping') {
                try {
                    $shutdownRecovery = Invoke-VerifiedStuckVmRecovery `
                        -ExpectedUuid $expectedVmUuid
                    $forcedStuckShutdownRecovery =
                        $shutdownRecovery.recovered -eq $true
                    $poweredOff = $forcedStuckShutdownRecovery
                }
                catch {
                    $shutdownRecovery = [pscustomobject]@{
                        attempted = $true
                        recovered = $false
                        vmUuid = $expectedVmUuid
                        message = $_.Exception.Message
                        completedUtc = [DateTimeOffset]::UtcNow
                    }
                    Write-Warning $_.Exception.Message
                }
            }
        }
    }
    else {
        $poweredOff = $true
    }
    if ($attached -and $poweredOff) {
        foreach ($port in 4..5) {
            $null = & $vboxManage storageattach `
                $vmName `
                --storagectl SATA `
                --port $port `
                --device 0 `
                --type hdd `
                --medium none 2>&1
            if ($LASTEXITCODE -ne 0) {
                Write-Warning "新規検証VDIをSATA port $port から切断できませんでした。"
            }
        }
    }
    elseif ($attached) {
        Write-Warning 'VMが正常停止しなかったため、新規検証VDIは接続したまま保持しています。'
    }
    if ($null -ne $shutdownRecovery) {
        try {
            $shutdownRecovery | ConvertTo-Json -Depth 5 | Set-Content `
                -LiteralPath (Join-Path $evidenceRoot `
                    'host-shutdown-recovery.json') `
                -Encoding utf8
        }
        catch {
            Write-Warning '固定VM終了復旧の証跡を書き込めませんでした。'
        }
    }
}

$finalInfo = @(Invoke-VBox `
    -Arguments @('showvminfo', $vmName, '--machinereadable') `
    -Operation 'データ縮小VM後始末確認')
$finalInfo | Set-Content `
    -LiteralPath (Join-Path $evidenceRoot 'vm-after.txt') `
    -Encoding utf8
$finalState = Get-MachineValue -Information $finalInfo -Name 'VMState'
if ($finalState -notin @('poweroff', 'aborted') -or
    (Get-MachineValue -Information $finalInfo -Name 'nic1') -ne 'none') {
    throw "VMの停止またはNIC復元を確認できません: state=$finalState"
}
foreach ($port in 0..3) {
    $actual = Get-AttachedMedium -Information $finalInfo -Port $port
    if ([string]::IsNullOrWhiteSpace($actual) -or
        -not [IO.Path]::GetFullPath($actual).Equals(
            $originalPorts[$port],
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "試験後に既存SATA port $port が変更されています。"
    }
}
foreach ($port in 4..5) {
    $actual = Get-AttachedMedium -Information $finalInfo -Port $port
    if (-not [string]::IsNullOrWhiteSpace($actual) -and $actual -ne 'none') {
        throw "試験後も新規SATA port $port が接続されています。"
    }
}

[ordered]@{
    schemaVersion = 1
    result = 'PASS'
    vmName = $vmName
    finalVmState = $finalState
    firmware = 'EFI64'
    nic = 'none'
    interactiveUac = $true
    physicalDiskOrUsbUsed = $false
    dataOnly = $true
    sourceDiskBytes = [UInt64](4GB)
    targetDiskBytes = [UInt64](2GB)
    sourceUnchanged = $true
    fileContentMatched = $true
    bootFinalizationRequired = $false
    vmShellIsolationRestored = $true
    shadowCopiesBefore = 0
    shadowCopiesAfter = 0
    sourceDisk = $sourceDisk
    targetDisk = $targetDisk
    createdVmMediaRetained = $true
    addedMediaDetached = $true
    originalMediaPreserved = $true
    forcedStuckShutdownRecovery = $forcedStuckShutdownRecovery
    completedUtc = [DateTimeOffset]::UtcNow
} | ConvertTo-Json -Depth 5 | Set-Content `
    -LiteralPath (Join-Path $evidenceRoot 'host-summary.json') `
    -Encoding utf8
Write-Output 'Product data shrink VM test: PASS'
Write-Output "Evidence: $evidenceRoot"
