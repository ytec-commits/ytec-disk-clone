[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string] $ExpectedHarnessSha256
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$workRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$harness = Join-Path $workRoot 'ytec-product-data-shrink-vm.exe'
$summaryPath = Join-Path $workRoot 'summary.json'
$donePath = Join-Path $workRoot 'done.txt'
$stdoutPath = Join-Path $workRoot 'harness-stdout.txt'
$stderrPath = Join-Path $workRoot 'harness-stderr.txt'
$authorization = 'YTEC-VM-ONLY-PRODUCT-DATA-SHRINK'
$systemBytes = [UInt64](96GB)
$sourceBytes = [UInt64](4GB)
$targetBytes = [UInt64](2GB)
$utf8 = [Text.UTF8Encoding]::new($false)
$stage = 'startup'
$diskDiagnosticsBefore = @()
$diskDiagnosticsAfter = @()
$shadowsBefore = @()
$shadowsAfter = @()
$harnessHash = $null
$shellServiceWasRunning = $false
$vmShellIsolationApplied = $false

function Write-Utf8Text {
    param(
        [Parameter(Mandatory)]
        [string] $Path,
        [Parameter(Mandatory)]
        [AllowEmptyString()]
        [string] $Value
    )

    [IO.File]::WriteAllText($Path, $Value, $utf8)
}

function Get-NoAutoMountValue {
    param(
        [Parameter(Mandatory)]
        [string] $MountManagerPath
    )

    $settings = Get-ItemProperty `
        -LiteralPath $MountManagerPath `
        -ErrorAction Stop
    $property = $settings.PSObject.Properties['NoAutoMount']
    if ($null -eq $property) {
        # mountmgrの既定値は自動マウント有効。値がない状態を0として扱う。
        return 0
    }
    return [int]$property.Value
}

function Test-Administrator {
    $principal = [Security.Principal.WindowsPrincipal]::new(
        [Security.Principal.WindowsIdentity]::GetCurrent())
    return $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-VirtualBoxGuest {
    $system = Get-CimInstance `
        -ClassName Win32_ComputerSystem `
        -ErrorAction Stop
    $identity = "$($system.Manufacturer) $($system.Model)"
    return $identity -match '(?i)(VirtualBox|innotek)'
}

function Get-SanitizedShadowCopies {
    return @(Get-CimInstance `
        -ClassName Win32_ShadowCopy `
        -ErrorAction Stop | ForEach-Object {
        [ordered]@{
            id = [string]$_.ID
            setId = [string]$_.SetID
            volumeName = [string]$_.VolumeName
        }
    })
}

function Write-DiskEvidence {
    param(
        [Parameter(Mandatory)]
        [string] $Name,
        [Parameter(Mandatory)]
        [AllowEmptyCollection()]
        [object[]] $Disks
    )

    Write-Utf8Text `
        -Path (Join-Path $workRoot "$Name.json") `
        -Value ($Disks | ConvertTo-Json -Depth 5)
}

try {
    if (-not (Test-Administrator)) {
        throw 'UACで昇格した管理者トークンではありません。'
    }
    if (-not (Test-VirtualBoxGuest)) {
        throw 'VirtualBox専用VM以外では実行できません。'
    }
    $activeAdapters = @(Get-CimInstance `
        -ClassName Win32_NetworkAdapter `
        -Filter 'NetEnabled = True' `
        -ErrorAction Stop)
    if ($activeAdapters.Count -ne 0) {
        throw 'NICが有効なためデータ縮小検証を開始しません。'
    }
    if ($env:SystemDrive -cne 'C:' -or
        -not [IO.File]::Exists($harness)) {
        throw '固定C:または製品データ縮小ハーネスを確認できません。'
    }
    $harnessHash = (
        Get-FileHash -LiteralPath $harness -Algorithm SHA256).Hash
    if ($harnessHash -cne $ExpectedHarnessSha256.ToUpperInvariant()) {
        throw '製品データ縮小ハーネスのSHA-256がホスト期待値と一致しません。'
    }

    $stage = 'disk-gate'
    $disks = @(Get-Disk -ErrorAction Stop)
    $diskDiagnosticsBefore = @($disks | Select-Object `
        Number,
        IsSystem,
        IsBoot,
        Size,
        PartitionStyle,
        NumberOfPartitions,
        IsOffline,
        IsReadOnly)
    Write-DiskEvidence `
        -Name 'disks-before' `
        -Disks $diskDiagnosticsBefore
    if ($disks.Count -ne 6) {
        throw '固定6台構成ではないため、新規VDIを初期化しません。'
    }
    $systemDisks = @($disks | Where-Object {
        ($_.IsSystem -or $_.IsBoot) -and
        [UInt64]$_.Size -eq $systemBytes -and
        [string]$_.PartitionStyle -ceq 'GPT'
    })
    $sourceDisks = @($disks | Where-Object {
        -not $_.IsSystem -and -not $_.IsBoot -and
        [UInt64]$_.Size -eq $sourceBytes -and
        [string]$_.PartitionStyle -ceq 'RAW' -and
        [int]$_.NumberOfPartitions -eq 0 -and
        -not $_.IsReadOnly -and -not $_.IsOffline
    })
    $targetDisks = @($disks | Where-Object {
        -not $_.IsSystem -and -not $_.IsBoot -and
        [UInt64]$_.Size -eq $targetBytes -and
        [string]$_.PartitionStyle -ceq 'RAW' -and
        [int]$_.NumberOfPartitions -eq 0 -and
        -not $_.IsReadOnly -and -not $_.IsOffline
    })
    if ($systemDisks.Count -ne 1 -or
        $sourceDisks.Count -ne 1 -or
        $targetDisks.Count -ne 1) {
        throw '96GiBシステム/4GiB RAW原本/2GiB RAW復元先の固定構成が一致しません。'
    }
    if ($null -ne (Get-Volume -DriveLetter S `
            -ErrorAction SilentlyContinue)) {
        throw '初期化前からS:が存在するため変更しません。'
    }
    $shadowsBefore = @(Get-SanitizedShadowCopies)
    Write-Utf8Text `
        -Path (Join-Path $workRoot 'shadows-before.json') `
        -Value (@{
            count = $shadowsBefore.Count
            copies = $shadowsBefore
        } | ConvertTo-Json -Depth 5)
    if ($shadowsBefore.Count -ne 0) {
        throw '開始前からShadow Copyが残留しています。自動削除せず停止しました。'
    }

    $stage = 'source-fixture'
    $initialized = $sourceDisks[0] |
        Initialize-Disk -PartitionStyle GPT -PassThru -ErrorAction Stop
    $partition = $initialized |
        New-Partition -UseMaximumSize -DriveLetter S -ErrorAction Stop
    $null = $partition |
        Format-Volume `
            -FileSystem NTFS `
            -NewFileSystemLabel 'YDC_DATA_SHRINK_SRC' `
            -Confirm:$false `
            -Force `
            -ErrorAction Stop
    $fixtureRoot = 'S:\YDC-Shrink-Fixture'
    $null = New-Item `
        -ItemType Directory `
        -Path $fixtureRoot `
        -ErrorAction Stop
    $bytes = [byte[]]::new(4MB)
    [Random]::new(20260803).NextBytes($bytes)
    [IO.File]::WriteAllBytes(
        (Join-Path $fixtureRoot 'sentinel.bin'),
        $bytes)
    Write-Utf8Text `
        -Path (Join-Path $fixtureRoot '説明.txt') `
        -Value "Y-TEC Tsumugi Drive データ専用縮小移行 VM 合成試験`r`n"

    # 原本側のS:を作成してから、復元先の新規ボリュームだけがExplorerへ
    # 通知されないように固定VMの自動マウントとシェル検出を一時停止する。
    $stage = 'vm-shell-isolation'
    $mountManagerPath =
        'HKLM:\SYSTEM\CurrentControlSet\Services\mountmgr'
    $noAutoMount = Get-NoAutoMountValue `
        -MountManagerPath $mountManagerPath
    if ($noAutoMount -ne 0) {
        throw '固定VMの自動マウント開始状態が有効ではありません。'
    }
    $shellService = Get-Service `
        -Name ShellHWDetection `
        -ErrorAction Stop
    $shellServiceWasRunning = $shellService.Status -eq 'Running'
    if ($shellServiceWasRunning) {
        Stop-Service `
            -Name ShellHWDetection `
            -Force `
            -ErrorAction Stop
    }
    # ここから先の失敗では、/Eとサービス状態復元を必ず試みる。
    $vmShellIsolationApplied = $true
    $mountvolOutput = @(& "$env:SystemRoot\System32\mountvol.exe" /N 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "VM自動マウントを一時停止できませんでした: $($mountvolOutput -join ' ')"
    }
    if ((Get-NoAutoMountValue -MountManagerPath $mountManagerPath) -ne 1) {
        throw 'VM自動マウントの一時停止状態をレジストリで再確認できませんでした。'
    }

    $stage = 'harness'
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $harness
    $startInfo.Arguments =
        "--authorization $authorization"
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardOutputEncoding = [Text.Encoding]::UTF8
    $startInfo.StandardErrorEncoding = [Text.Encoding]::UTF8
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    $startedUtc = [DateTimeOffset]::UtcNow
    if (-not $process.Start()) {
        throw '製品データ縮小ハーネスを開始できませんでした。'
    }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(45 * 60 * 1000)) {
        try {
            $process.Kill()
        }
        catch {
            # Timeoutを正本にし、停止試行失敗で置き換えない。
        }
        throw '製品データ縮小ハーネスが45分以内に完了しませんでした。'
    }
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    Write-Utf8Text -Path $stdoutPath -Value $stdout
    Write-Utf8Text -Path $stderrPath -Value $stderr
    if ($process.ExitCode -ne 0 -or
        $stdout -notmatch 'YDC_PRODUCT_DATA_SHRINK_PASS') {
        throw "製品データ縮小ハーネスが失敗しました。終了コード=$($process.ExitCode)"
    }

    $stage = 'post-validation'
    $afterDisks = @(Get-Disk -ErrorAction Stop)
    $diskDiagnosticsAfter = @($afterDisks | Select-Object `
        Number,
        IsSystem,
        IsBoot,
        Size,
        PartitionStyle,
        NumberOfPartitions,
        IsOffline,
        IsReadOnly)
    Write-DiskEvidence `
        -Name 'disks-after' `
        -Disks $diskDiagnosticsAfter
    $sourceAfter = @($afterDisks | Where-Object {
        [UInt64]$_.Size -eq $sourceBytes -and
        [string]$_.PartitionStyle -ceq 'GPT' -and
        [int]$_.NumberOfPartitions -ge 1
    })
    $targetAfter = @($afterDisks | Where-Object {
        [UInt64]$_.Size -eq $targetBytes -and
        [string]$_.PartitionStyle -ceq 'GPT' -and
        [int]$_.NumberOfPartitions -ge 1
    })
    if ($sourceAfter.Count -ne 1 -or $targetAfter.Count -ne 1) {
        throw '完了後の4GiB原本または2GiB復元先を確認できません。'
    }
    $shadowsAfter = @(Get-SanitizedShadowCopies)
    Write-Utf8Text `
        -Path (Join-Path $workRoot 'shadows-after.json') `
        -Value (@{
            count = $shadowsAfter.Count
            copies = $shadowsAfter
        } | ConvertTo-Json -Depth 5)
    if ($shadowsAfter.Count -ne 0) {
        throw '完了後にShadow Copyが残留しています。自動削除せず停止しました。'
    }

    $stage = 'vm-environment-restore'
    $mountvolOutput = @(& "$env:SystemRoot\System32\mountvol.exe" /E 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "VM自動マウントを復元できませんでした: $($mountvolOutput -join ' ')"
    }
    if ($shellServiceWasRunning) {
        Start-Service -Name ShellHWDetection -ErrorAction Stop
    }
    if ((Get-NoAutoMountValue -MountManagerPath $mountManagerPath) -ne 0 -or
        ($shellServiceWasRunning -and
         (Get-Service -Name ShellHWDetection -ErrorAction Stop).Status -ne
             'Running')) {
        throw 'VMの自動マウントまたはShell Hardware Detectionを元の状態へ復元できませんでした。'
    }
    $vmShellIsolationApplied = $false

    $summary = [ordered]@{
        schemaVersion = 1
        result = 'PASS'
        stage = $stage
        harnessSha256 = $harnessHash
        sourceDiskBytes = $sourceBytes
        targetDiskBytes = $targetBytes
        sourceLargerThanTarget = $true
        dataOnly = $true
        sourceUnchanged = $true
        fileContentMatched = $true
        bootFinalizationRequired = $false
        vmShellIsolationRestored = $true
        shadowCopiesBefore = $shadowsBefore.Count
        shadowCopiesAfter = $shadowsAfter.Count
        startedUtc = $startedUtc
        completedUtc = [DateTimeOffset]::UtcNow
    }
    Write-Utf8Text `
        -Path $summaryPath `
        -Value ($summary | ConvertTo-Json -Depth 6)
    Write-Utf8Text -Path $donePath -Value 'PASS'
}
catch {
    $originalFailure = $_.Exception.Message
    if ($vmShellIsolationApplied) {
        $restoreIssues = @()
        try {
            $restoreOutput = @(
                & "$env:SystemRoot\System32\mountvol.exe" /E 2>&1)
            if ($LASTEXITCODE -ne 0) {
                $restoreIssues +=
                    "mountvol /E: $($restoreOutput -join ' ')"
            }
            if ($shellServiceWasRunning) {
                Start-Service `
                    -Name ShellHWDetection `
                    -ErrorAction Stop
            }
            if ((Get-NoAutoMountValue -MountManagerPath $mountManagerPath) -ne
                0) {
                $restoreIssues += 'NoAutoMountが0へ戻っていません。'
            }
            if ($shellServiceWasRunning -and
                (Get-Service `
                    -Name ShellHWDetection `
                    -ErrorAction Stop).Status -ne 'Running') {
                $restoreIssues += 'ShellHWDetectionがRunningへ戻っていません。'
            }
        }
        catch {
            $restoreIssues += $_.Exception.Message
        }
        if ($restoreIssues.Count -eq 0) {
            $vmShellIsolationApplied = $false
        }
        else {
            $originalFailure +=
                " / VM環境復元エラー: $($restoreIssues -join ' | ')"
        }
    }
    $failure = [ordered]@{
        schemaVersion = 1
        result = 'FAIL'
        stage = $stage
        message = $originalFailure
        harnessSha256 = $harnessHash
        disksBefore = $diskDiagnosticsBefore
        disksAfter = $diskDiagnosticsAfter
        completedUtc = [DateTimeOffset]::UtcNow
    }
    try {
        Write-Utf8Text `
            -Path $summaryPath `
            -Value ($failure | ConvertTo-Json -Depth 7)
        Write-Utf8Text -Path $donePath -Value 'FAIL'
    }
    catch {
        # 元の検証失敗を正本にする。
    }
    exit 1
}
