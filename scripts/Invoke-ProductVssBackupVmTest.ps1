[CmdletBinding()]
param(
    [ValidateRange(30, 90)]
    [int] $TimeoutMinutes = 75,
    [switch] $AllowVerifiedAbortedRecovery,
    [string] $ResumePreparedEvidenceRoot,
    [switch] $ResetOwnedOutputFiles
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
$guestUser = 'YbcTest'
$passwordFile = Join-Path $credentialRoot `
    'YWB-Win10-22H2-x64-Clean.password.txt'
$harness = Join-Path $repoRoot `
    'out\build\msvc-x64-vm-destructive\tests\ytec-product-vss-backup-vm.exe'
$guestRunnerSource = Join-Path $repoRoot `
    'scripts\vm\Run-ProductVssBackupValidationElevated.ps1'
$vmFolder = Join-Path $repoRoot ".validation\vms\$vmName"
$runStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$allowedEvidenceRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot `
    '.validation\evidence\product-vss-backup-vm'))
$resumeMode = -not [string]::IsNullOrWhiteSpace(
    $ResumePreparedEvidenceRoot)
if ($ResetOwnedOutputFiles -and -not $resumeMode) {
    throw '-ResetOwnedOutputFilesは検証済み出力VDIの再開時だけ指定できます。'
}
if ($resumeMode) {
    $preparedEvidenceRoot = [IO.Path]::GetFullPath(
        $ResumePreparedEvidenceRoot)
    if (-not $preparedEvidenceRoot.StartsWith(
            $allowedEvidenceRoot + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $preparedEvidenceRoot `
            -PathType Container) -or
        ((Get-Item -LiteralPath $preparedEvidenceRoot).Attributes -band `
            [IO.FileAttributes]::ReparsePoint)) {
        throw '再開元証跡は固定製品VSS証跡配下の通常ディレクトリだけを指定できます。'
    }
    $preparedPath = Join-Path $preparedEvidenceRoot `
        'host-preparation.json'
    if (-not (Test-Path -LiteralPath $preparedPath -PathType Leaf)) {
        throw "再開元host-preparation.jsonがありません: $preparedPath"
    }
    $prepared = Get-Content -LiteralPath $preparedPath -Raw |
        ConvertFrom-Json
    if ($prepared.status -ne 'PREPARED' -or
        $prepared.vmName -ne $vmName -or
        [UInt64]$prepared.outputDiskBytes -ne [UInt64](32GB) -or
        [string]::IsNullOrWhiteSpace([string]$prepared.outputDisk) -or
        [string]::IsNullOrWhiteSpace([string]$prepared.guestRoot)) {
        throw '再開元host-preparation.jsonの固定条件が一致しません。'
    }
    $outputDisk = [IO.Path]::GetFullPath([string]$prepared.outputDisk)
    if (-not $outputDisk.StartsWith(
            [IO.Path]::GetFullPath($vmFolder) +
                [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $outputDisk -PathType Leaf) -or
        ((Get-Item -LiteralPath $outputDisk).Attributes -band
            [IO.FileAttributes]::ReparsePoint)) {
        throw '再開対象VDIは固定VMフォルダー内の既存通常ファイルではありません。'
    }
    $guestRoot = [string]$prepared.guestRoot
    $evidenceRoot = Join-Path $allowedEvidenceRoot `
        "$runStamp-resume"
}
else {
    $outputDisk = Join-Path $vmFolder `
        "YDC-Product-VSS-Output-32GiB-$runStamp.vdi"
    $evidenceRoot = Join-Path $allowedEvidenceRoot $runStamp
    $guestRoot = "C:\Users\$guestUser\YDC-Product-VSS-$runStamp"
}
$guestHarness = "$guestRoot\ytec-product-vss-backup-vm.exe"
$guestRunner = "$guestRoot\Run-ProductVssBackupValidationElevated.ps1"
$guestLauncher = `
    "C:\Users\$guestUser\Desktop\YDC_Product_VSS__DOUBLE_CLICK.cmd"
$guestDone = "$guestRoot\done.txt"
$guestResultArtifacts = @(
    'summary.json',
    'done.txt',
    'capacity-stdout.txt',
    'capacity-stderr.txt',
    'cancel-stdout.txt',
    'cancel-stderr.txt',
    'success-stdout.txt',
    'success-stderr.txt',
    'shadows-before.json',
    'shadows-after-capacity.json',
    'shadows-after-cancel.json',
    'shadows-after-success.json',
    'shadows-after-failure.json'
)

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
                # Timeoutを正本にし、停止試行失敗で置き換えない。
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

$running = @(& $vboxManage list runningvms 2>&1)
if ($LASTEXITCODE -ne 0) {
    throw '稼働中VirtualBox VMを確認できませんでした。'
}
if ($running.Count -ne 0) {
    throw "別VMが稼働中のため開始しません: $($running -join ', ')"
}

$initialInfo = @(Invoke-VBox `
    -Arguments @('showvminfo', $vmName, '--machinereadable') `
    -Operation '製品VSS専用VM事前確認')
$initialState =
    Get-MachineValue -Information $initialInfo -Name 'VMState'
if ($initialState -eq 'aborted' -and
    -not $AllowVerifiedAbortedRecovery) {
    throw 'aborted状態です。ゲスト正常終了済みと確認した場合だけ-AllowVerifiedAbortedRecoveryを指定できます。'
}
if (($resumeMode -and $initialState -notin @('saved', 'poweroff')) -or
    (-not $resumeMode -and $initialState -notin @('poweroff', 'aborted'))) {
    throw "製品VSS専用VMの開始状態が再開条件と一致しません: $initialState"
}
if ((Get-MachineValue -Information $initialInfo -Name 'nic1') -ne 'none' -or
    (Get-MachineValue -Information $initialInfo -Name 'firmware') -ne 'EFI64') {
    throw '製品VSS専用VMのNICまたはUEFI設定が安全条件と一致しません。'
}

$port0 = Get-AttachedMedium -Information $initialInfo -Port 0
$port1 = Get-AttachedMedium -Information $initialInfo -Port 1
$port2 = Get-AttachedMedium -Information $initialInfo -Port 2
$port3 = Get-AttachedMedium -Information $initialInfo -Port 3
$normalizedPort1 = if ([string]::IsNullOrWhiteSpace($port1) -or
    $port1 -eq 'none') {
    $null
}
else {
    [IO.Path]::GetFullPath($port1)
}
$normalizedPort2 = if ([string]::IsNullOrWhiteSpace($port2) -or
    $port2 -eq 'none') {
    $null
}
else {
    [IO.Path]::GetFullPath($port2)
}
if ([string]::IsNullOrWhiteSpace($port0) -or $port0 -eq 'none' -or
    $null -eq $normalizedPort1 -or
    -not $normalizedPort1.StartsWith(
        (Join-Path $vmFolder 'YDC-Phase5-FileSource-128MiB-'),
        [StringComparison]::OrdinalIgnoreCase) -or
    $null -eq $normalizedPort2 -or
    -not $normalizedPort2.StartsWith(
        (Join-Path $vmFolder 'YDC-Phase5-FileDestination-512MiB-'),
        [StringComparison]::OrdinalIgnoreCase) -or
    (-not $resumeMode -and
     -not [string]::IsNullOrWhiteSpace($port3) -and $port3 -ne 'none') -or
    ($resumeMode -and
     ([string]::IsNullOrWhiteSpace($port3) -or $port3 -eq 'none' -or
      -not [IO.Path]::GetFullPath($port3).Equals(
          $outputDisk,
          [StringComparison]::OrdinalIgnoreCase)))) {
    throw 'SATA 0/1/2の固定媒体またはport 3再開条件が一致しません。'
}

New-Item -ItemType Directory -Path $evidenceRoot -Force | Out-Null
$initialInfo | Set-Content `
    -LiteralPath (Join-Path $evidenceRoot 'vm-before.txt') `
    -Encoding utf8

if (-not $resumeMode) {
    $null = Invoke-VBox `
        -Arguments @(
            'createmedium', 'disk',
            '--filename', $outputDisk,
            '--size', '32768',
            '--format', 'VDI',
            '--variant', 'Standard'
        ) `
        -Operation '32GiB製品VSS出力VDI作成'
    $null = Invoke-VBox `
        -Arguments @(
            'storageattach', $vmName,
            '--storagectl', 'SATA',
            '--port', '3',
            '--device', '0',
            '--type', 'hdd',
            '--medium', $outputDisk,
            '--nonrotational', 'on'
        ) `
        -Operation '32GiB製品VSS出力VDI接続'
}

$attachedInfo = @(Invoke-VBox `
    -Arguments @('showvminfo', $vmName, '--machinereadable') `
    -Operation '製品VSS出力VDI接続後確認')
$actualPort3 = Get-AttachedMedium -Information $attachedInfo -Port 3
if ([string]::IsNullOrWhiteSpace($actualPort3) -or
    $actualPort3 -eq 'none' -or
    -not [IO.Path]::GetFullPath($actualPort3).Equals(
        [IO.Path]::GetFullPath($outputDisk),
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'SATA port 3の製品VSS出力VDIが一致しません。'
}

$harnessHash = (
    Get-FileHash -LiteralPath $harness -Algorithm SHA256).Hash
$stagingRoot = Join-Path $evidenceRoot 'guest-scripts-utf8-bom'
New-Item -ItemType Directory -Path $stagingRoot -Force | Out-Null
$stagedRunner = Join-Path $stagingRoot `
    ([IO.Path]::GetFileName($guestRunnerSource))
[IO.File]::WriteAllText(
    $stagedRunner,
    [IO.File]::ReadAllText($guestRunnerSource),
    [Text.UTF8Encoding]::new($true))
$hostLauncher = Join-Path $evidenceRoot `
    'YDC_Product_VSS__DOUBLE_CLICK.cmd'
$guestRunnerArguments =
    "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File $guestRunner " +
    "-ExpectedHarnessSha256 $harnessHash"
if ($ResetOwnedOutputFiles) {
    $guestRunnerArguments += ' -ResetOwnedOutputFiles'
}
$launcherContent = @"
@echo off
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe' -Verb RunAs -ArgumentList '$guestRunnerArguments' -WindowStyle Hidden"
exit /b
"@
[IO.File]::WriteAllText(
    $hostLauncher,
    $launcherContent,
    [Text.ASCIIEncoding]::new())

$started = $false
try {
    $null = Invoke-VBox `
        -Arguments @('startvm', $vmName, '--type', 'gui') `
        -Operation '製品VSS統合VM起動'
    $started = $true
    Wait-GuestReady

    $null = Invoke-GuestRetry `
        -Operation 'ゲスト製品VSS検証ディレクトリ作成' `
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

    # A resumed guest folder deliberately retains scripts, but a stale done
    # marker must never be accepted as evidence for the new run. Remove only
    # the fixed result filenames owned by this validation workflow.
    foreach ($artifact in $guestResultArtifacts) {
        $guestArtifact = "$guestRoot\$artifact"
        $null = & $vboxManage guestcontrol $vmName stat `
            "--username=$guestUser" `
            "--passwordfile=$passwordFile" `
            $guestArtifact 2>&1
        if ($LASTEXITCODE -eq 0) {
            $null = Invoke-GuestRetry `
                -Operation "前回結果マーカー削除 $artifact" `
                -Arguments @(
                    'rm',
                    "--username=$guestUser",
                    "--passwordfile=$passwordFile",
                    $guestArtifact
                )
        }
    }

    [ordered]@{
        schemaVersion = 1
        status = 'PREPARED'
        resumeMode = $resumeMode
        resetOwnedOutputFiles = [bool]$ResetOwnedOutputFiles
        resumedFromEvidence = if ($resumeMode) {
            $preparedEvidenceRoot
        }
        else {
            $null
        }
        vmName = $vmName
        previousVmState = $initialState
        verifiedAbortedRecovery = [bool]$AllowVerifiedAbortedRecovery
        guestRoot = $guestRoot
        guestLauncher = $guestLauncher
        outputDisk = $outputDisk
        outputDiskBytes = [UInt64](32GB)
        harnessSha256 = $harnessHash
        nic = 'none'
        firmware = 'EFI64'
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
        $doneProbe = Invoke-GuestControlTimed `
            -Arguments @(
                'stat',
                "--username=$guestUser",
                "--passwordfile=$passwordFile",
                $guestDone
            ) `
            -TimeoutSeconds 20
        if ($doneProbe.ExitCode -eq 0) {
            $doneFound = $true
            break
        }
        Start-Sleep -Seconds 10
    }
    if (-not $doneFound) {
        throw "UAC承認後の製品VSS VM試験が${TimeoutMinutes}分以内に完了しませんでした。"
    }

    foreach ($artifact in $guestResultArtifacts) {
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
    if ($done -ne 'PASS' -or $summary.result -ne 'PASS') {
        throw "製品VSS統合VM試験が失敗しました: $($summary.message)"
    }

    [ordered]@{
        schemaVersion = 1
        result = 'PASS'
        vmName = $vmName
        firmware = 'EFI64'
        nic = 'none'
        interactiveUac = $true
        physicalDiskOrUsbUsed = $false
        capacityFailurePassed = $true
        cancellationPassed = $true
        successPassed = $true
        shadowsRemaining = 0
        imageBytes = [UInt64]$summary.finalBytes
        imageSha256 = [string]$summary.finalSha256
        outputDisk = $outputDisk
        outputDiskRetained = $true
        ownedOutputFilesReset = [bool]$summary.ownedOutputFilesReset
        completedUtc = [DateTimeOffset]::UtcNow
    } | ConvertTo-Json -Depth 5 | Set-Content `
        -LiteralPath (Join-Path $evidenceRoot 'host-summary.json') `
        -Encoding utf8

    Write-Output 'Product VSS backup VM test: PASS'
    Write-Output "Evidence: $evidenceRoot"
}
finally {
    if ($started) {
        $currentInfo = @(& $vboxManage showvminfo `
            $vmName `
            --machinereadable 2>&1)
        $currentState =
            Get-MachineValue -Information $currentInfo -Name 'VMState'
        if ($currentState -eq 'running') {
            $null = & $vboxManage controlvm $vmName savestate 2>&1
            if ($LASTEXITCODE -ne 0) {
                Write-Warning '製品VSS専用VMを保存状態へ戻せませんでした。'
            }
        }
    }
}
