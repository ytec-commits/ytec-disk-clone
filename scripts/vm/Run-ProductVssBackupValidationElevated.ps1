[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string] $ExpectedHarnessSha256,
    [switch] $ResetOwnedOutputFiles
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$workRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$harness = Join-Path $workRoot 'ytec-product-vss-backup-vm.exe'
$summaryPath = Join-Path $workRoot 'summary.json'
$donePath = Join-Path $workRoot 'done.txt'
$capacityPath = 'T:\YDC-Product-VSS\capacity.dcimg'
$cancelPath = 'U:\YDC-Product-VSS\cancel.dcimg'
$finalPath = 'U:\YDC-Product-VSS\system.dcimg'
$authorization = 'YTEC-VM-ONLY-PRODUCT-VSS-BACKUP'
$systemBytes = [UInt64](96GB)
$sourceFixtureBytes = [UInt64](128MB)
$capacityBytes = [UInt64](512MB)
$outputBytes = [UInt64](32GB)
$utf8 = [Text.UTF8Encoding]::new($false)
$stage = 'startup'
$harnessHash = $null
$shadowsBefore = $null
$shadowsAfterCapacity = $null
$shadowsAfterCancel = $null
$shadowsAfterSuccess = $null
$modeReports = [ordered]@{}
$diskDiagnostics = @()
$outputPartitionDiagnostics = @()
$outputVolumeReused = $false
$ownedOutputFilesReset = $false

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
            installDate = if ($null -eq $_.InstallDate) {
                $null
            }
            else {
                ([DateTimeOffset]$_.InstallDate).ToUniversalTime().
                    ToString('O')
            }
        }
    })
}

function Write-ShadowEvidence {
    param(
        [Parameter(Mandatory)]
        [string] $Name,
        [Parameter(Mandatory)]
        [AllowEmptyCollection()]
        [object[]] $Copies
    )

    $payload = [ordered]@{
        schemaVersion = 1
        count = $Copies.Count
        copies = @($Copies)
        observedUtc = [DateTimeOffset]::UtcNow
    }
    Write-Utf8Text `
        -Path (Join-Path $workRoot "$Name.json") `
        -Value ($payload | ConvertTo-Json -Depth 6)
}

function Assert-NoShadowCopies {
    param(
        [Parameter(Mandatory)]
        [string] $Name
    )

    $copies = @(Get-SanitizedShadowCopies)
    Write-ShadowEvidence -Name $Name -Copies $copies
    if ($copies.Count -ne 0) {
        throw "$Name の時点でShadow Copyが残留しています。自動削除せず停止しました。"
    }
    return $copies.Count
}

function Convert-KeyValueOutput {
    param(
        [Parameter(Mandatory)]
        [string] $Text
    )

    $values = [ordered]@{}
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line -match '^([A-Z0-9_]+)=(.*)$') {
            $values[$Matches[1]] = $Matches[2]
        }
    }
    return $values
}

function Invoke-HarnessMode {
    param(
        [Parameter(Mandatory)]
        [ValidateSet('capacity', 'cancel', 'success')]
        [string] $Mode,
        [Parameter(Mandatory)]
        [ValidateRange(1, 90)]
        [int] $TimeoutMinutes,
        [Parameter(Mandatory)]
        [string] $ExpectedMarker
    )

    $stdoutPath = Join-Path $workRoot "$Mode-stdout.txt"
    $stderrPath = Join-Path $workRoot "$Mode-stderr.txt"
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $harness
    $startInfo.Arguments =
        "--mode $Mode --authorization $authorization"
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
        throw "$Mode ハーネスを開始できませんでした。"
    }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($TimeoutMinutes * 60 * 1000)) {
        try {
            $process.Kill()
        }
        catch {
            # Timeoutを正本とし、停止試行失敗で置き換えない。
        }
        throw "$Mode ハーネスが${TimeoutMinutes}分以内に完了しませんでした。"
    }
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    Write-Utf8Text -Path $stdoutPath -Value $stdout
    Write-Utf8Text -Path $stderrPath -Value $stderr
    if ($process.ExitCode -ne 0) {
        throw "$Mode ハーネスが終了コード$($process.ExitCode)で失敗しました。"
    }
    if ($stdout -notmatch [Regex]::Escape($ExpectedMarker)) {
        throw "$Mode ハーネスのPASSマーカーがありません。"
    }
    return [ordered]@{
        mode = $Mode
        exitCode = $process.ExitCode
        marker = $ExpectedMarker
        startedUtc = $startedUtc
        completedUtc = [DateTimeOffset]::UtcNow
        values = Convert-KeyValueOutput -Text $stdout
    }
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
        throw 'NICが有効なため製品VSS検証を開始しません。'
    }
    if ($env:SystemDrive -cne 'C:') {
        throw '固定C:がシステムドライブではありません。'
    }
    if (-not [IO.File]::Exists($harness)) {
        throw "製品VSSハーネスがありません: $harness"
    }
    $harnessHash = (
        Get-FileHash -LiteralPath $harness -Algorithm SHA256).Hash
    if ($harnessHash -cne $ExpectedHarnessSha256.ToUpperInvariant()) {
        throw '製品VSSハーネスのSHA-256がホスト側期待値と一致しません。'
    }

    $stage = 'disk-gate'
    $disks = @(Get-Disk -ErrorAction Stop)
    $diskDiagnostics = @($disks | Select-Object `
        Number,
        IsSystem,
        IsBoot,
        Size,
        PartitionStyle,
        NumberOfPartitions,
        IsOffline,
        IsReadOnly)
    if ($disks.Count -ne 4) {
        throw '固定4台構成ではないため、追加VDIを初期化しません。'
    }
    $systemDisks = @($disks | Where-Object {
        ($_.IsSystem -or $_.IsBoot) -and
        [UInt64]$_.Size -eq $systemBytes -and
        [string]$_.PartitionStyle -ceq 'GPT'
    })
    $sourceFixtures = @($disks | Where-Object {
        -not $_.IsSystem -and -not $_.IsBoot -and
        [UInt64]$_.Size -eq $sourceFixtureBytes -and
        [string]$_.PartitionStyle -ceq 'RAW' -and
        [int]$_.NumberOfPartitions -eq 0
    })
    $capacityDisks = @($disks | Where-Object {
        -not $_.IsSystem -and -not $_.IsBoot -and
        [UInt64]$_.Size -eq $capacityBytes -and
        [string]$_.PartitionStyle -ceq 'MBR' -and
        [int]$_.NumberOfPartitions -eq 1
    })
    $rawOutputDisks = @($disks | Where-Object {
        -not $_.IsSystem -and -not $_.IsBoot -and
        [UInt64]$_.Size -eq $outputBytes -and
        [string]$_.PartitionStyle -ceq 'RAW' -and
        [int]$_.NumberOfPartitions -eq 0 -and
        -not $_.IsReadOnly -and -not $_.IsOffline
    })
    $preparedOutputDisks = @($disks | Where-Object {
        -not $_.IsSystem -and -not $_.IsBoot -and
        [UInt64]$_.Size -eq $outputBytes -and
        [string]$_.PartitionStyle -ceq 'GPT' -and
        [int]$_.NumberOfPartitions -in @(1, 2) -and
        -not $_.IsReadOnly -and -not $_.IsOffline
    })
    if ($systemDisks.Count -ne 1 -or
        $sourceFixtures.Count -ne 1 -or
        $capacityDisks.Count -ne 1 -or
        ($rawOutputDisks.Count + $preparedOutputDisks.Count) -ne 1) {
        throw '96GiBシステム/128MiB RAW/512MiB MBR/32GiB RAWまたは検証済みGPT出力の固定構成が一致しません。'
    }
    $capacityVolume = Get-Volume -DriveLetter T -ErrorAction Stop
    $capacityDisk = Get-Partition -DriveLetter T -ErrorAction Stop |
        Get-Disk -ErrorAction Stop
    if ($capacityDisk.Number -ne $capacityDisks[0].Number -or
        [string]$capacityVolume.FileSystem -cne 'NTFS' -or
        [string]$capacityVolume.FileSystemLabel -cne 'YDC_PHASE5_STAGE') {
        throw '固定T:が512MiB容量不足検証ディスクへ対応していません。'
    }

    $stage = 'output-format'
    if ($rawOutputDisks.Count -eq 1) {
        if ($null -ne (Get-Volume -DriveLetter U `
                -ErrorAction SilentlyContinue)) {
            throw 'RAW出力VDIの初期化前からU:が存在するため変更しません。'
        }
        $initialized = $rawOutputDisks[0] |
            Initialize-Disk -PartitionStyle GPT -PassThru -ErrorAction Stop
        $partition = $initialized |
            New-Partition -UseMaximumSize -DriveLetter U -ErrorAction Stop
        $null = $partition |
            Format-Volume `
                -FileSystem NTFS `
                -NewFileSystemLabel 'YDC_PRODUCT_VSS' `
                -Confirm:$false `
                -Force `
                -ErrorAction Stop
        $expectedOutputDiskNumber = $rawOutputDisks[0].Number
        $outputVolumeReused = $false
    }
    else {
        $expectedOutputDiskNumber = $preparedOutputDisks[0].Number
        $outputVolumeReused = $true
    }
    $outputDisk = Get-Partition -DriveLetter U -ErrorAction Stop |
        Get-Disk -ErrorAction Stop
    $outputVolume = Get-Volume -DriveLetter U -ErrorAction Stop
    $outputPartitions = @(
        Get-Partition -DiskNumber $outputDisk.Number -ErrorAction Stop)
    $outputPartitionDiagnostics = @($outputPartitions | Select-Object `
        DiskNumber,
        PartitionNumber,
        DriveLetter,
        GptType,
        Type,
        Size,
        Offset)
    $outputDataPartitions = @($outputPartitions | Where-Object {
        [string]$_.DriveLetter -ceq 'U'
    })
    $outputAuxiliaryPartitions = @($outputPartitions | Where-Object {
        [string]$_.DriveLetter -cne 'U'
    })
    $microsoftReservedType =
        '{E3C9E316-0B5C-4DB8-817D-F92DF00215AE}'
    $auxiliaryPartitionsSafe = @($outputAuxiliaryPartitions |
        Where-Object {
            $letter = [string]$_.DriveLetter
            $letterAbsent = $letter.Length -eq 0 -or
                [int][char]$letter[0] -eq 0
            [string]$_.GptType -ieq $microsoftReservedType -and
            [UInt64]$_.Size -le [UInt64](128MB) -and
            $letterAbsent
        }).Count -eq $outputAuxiliaryPartitions.Count
    if ($outputDisk.Number -ne $expectedOutputDiskNumber -or
        [UInt64]$outputDisk.Size -ne $outputBytes -or
        $outputDataPartitions.Count -ne 1 -or
        $outputAuxiliaryPartitions.Count -gt 1 -or
        -not $auxiliaryPartitionsSafe -or
        [string]$outputVolume.FileSystem -cne 'NTFS' -or
        [string]$outputVolume.FileSystemLabel -cne 'YDC_PRODUCT_VSS') {
        throw 'U:が固定32GiB GPT/NTFS出力VDIへ一意対応していません。'
    }

    $stage = 'output-preflight'
    [IO.Directory]::CreateDirectory(
        (Split-Path -Parent $capacityPath)) | Out-Null
    [IO.Directory]::CreateDirectory(
        (Split-Path -Parent $finalPath)) | Out-Null
    $ownedOutputPaths = @(
        $capacityPath,
        "$capacityPath.partial",
        $cancelPath,
        "$cancelPath.partial",
        $finalPath,
        "$finalPath.partial"
    )
    foreach ($parent in @(
        (Split-Path -Parent $capacityPath),
        (Split-Path -Parent $finalPath)
    )) {
        $parentItem = Get-Item -LiteralPath $parent -Force -ErrorAction Stop
        if (-not $parentItem.PSIsContainer -or
            ($parentItem.Attributes -band
                [IO.FileAttributes]::ReparsePoint)) {
            throw "製品VSS固定出力ディレクトリが通常ディレクトリではありません: $parent"
        }
    }
    if ($ResetOwnedOutputFiles) {
        foreach ($path in $ownedOutputPaths) {
            if (-not [IO.File]::Exists($path)) {
                continue
            }
            $item = Get-Item -LiteralPath $path -Force -ErrorAction Stop
            if ($item.PSIsContainer -or
                ($item.Attributes -band
                    [IO.FileAttributes]::ReparsePoint)) {
                throw "削除対象の検証出力が通常ファイルではありません: $path"
            }
            Remove-Item -LiteralPath $path -Force -ErrorAction Stop
            if ([IO.File]::Exists($path)) {
                throw "所有中の検証出力を削除できませんでした: $path"
            }
            $ownedOutputFilesReset = $true
        }
    }
    foreach ($path in $ownedOutputPaths) {
        if ([IO.File]::Exists($path)) {
            throw "新規出力条件に反するファイルがあります: $path"
        }
    }

    $stage = 'shadow-before'
    $shadowsBefore = Assert-NoShadowCopies -Name 'shadows-before'

    $stage = 'capacity'
    $modeReports.capacity = Invoke-HarnessMode `
        -Mode capacity `
        -TimeoutMinutes 15 `
        -ExpectedMarker 'YDC_PRODUCT_VSS_CAPACITY_PASS'
    $shadowsAfterCapacity =
        Assert-NoShadowCopies -Name 'shadows-after-capacity'

    $stage = 'cancel'
    $modeReports.cancel = Invoke-HarnessMode `
        -Mode cancel `
        -TimeoutMinutes 20 `
        -ExpectedMarker 'YDC_PRODUCT_VSS_CANCEL_PASS'
    $shadowsAfterCancel =
        Assert-NoShadowCopies -Name 'shadows-after-cancel'

    $stage = 'success'
    $modeReports.success = Invoke-HarnessMode `
        -Mode success `
        -TimeoutMinutes 60 `
        -ExpectedMarker 'YDC_PRODUCT_VSS_SUCCESS_PASS'
    $shadowsAfterSuccess =
        Assert-NoShadowCopies -Name 'shadows-after-success'

    $stage = 'final-verification'
    foreach ($path in @(
        $capacityPath,
        "$capacityPath.partial",
        $cancelPath,
        "$cancelPath.partial",
        "$finalPath.partial"
    )) {
        if ([IO.File]::Exists($path)) {
            throw "失敗/中止後または確定後に不要ファイルが残っています: $path"
        }
    }
    $finalInfo = Get-Item -LiteralPath $finalPath -ErrorAction Stop
    if ([UInt64]$finalInfo.Length -le [UInt64](1GB) -or
        [UInt64]$finalInfo.Length -ge $outputBytes) {
        throw '完成dcimgのサイズが固定検証範囲外です。'
    }
    $reportedBytes = [UInt64]$modeReports.success.values.IMAGE_BYTES
    if ([UInt64]$finalInfo.Length -ne $reportedBytes) {
        throw '完成dcimgの実サイズと製品レポートが一致しません。'
    }
    $finalSha256 = (
        Get-FileHash -LiteralPath $finalPath -Algorithm SHA256).Hash

    $stage = 'complete'
    $summary = [ordered]@{
        schemaVersion = 1
        result = 'PASS'
        stage = $stage
        virtualBox = $true
        nicEnabledCount = 0
        systemDrive = 'C:'
        systemDiskBytes = $systemBytes
        capacityDiskBytes = $capacityBytes
        outputDiskBytes = $outputBytes
        outputDrive = 'U:'
        outputFileSystem = 'NTFS'
        outputVolumeReused = $outputVolumeReused
        ownedOutputFilesReset = $ownedOutputFilesReset
        harnessSha256 = $harnessHash
        modes = $modeReports
        shadowsBefore = $shadowsBefore
        shadowsAfterCapacity = $shadowsAfterCapacity
        shadowsAfterCancel = $shadowsAfterCancel
        shadowsAfterSuccess = $shadowsAfterSuccess
        finalPath = $finalPath
        finalBytes = [UInt64]$finalInfo.Length
        finalSha256 = $finalSha256
        partialExists = $false
        completedUtc = [DateTimeOffset]::UtcNow
    }
    Write-Utf8Text `
        -Path $summaryPath `
        -Value ($summary | ConvertTo-Json -Depth 8)
    Write-Utf8Text -Path $donePath -Value 'PASS'
    exit 0
}
catch {
    $failure = $_.Exception.Message
    $observedShadows = $null
    try {
        $observedShadows = @(Get-SanitizedShadowCopies)
        Write-ShadowEvidence `
            -Name 'shadows-after-failure' `
            -Copies $observedShadows
    }
    catch {
        # 元の失敗を正本にし、確認不能はsummaryへ残す。
    }
    $summary = [ordered]@{
        schemaVersion = 1
        result = 'FAIL'
        stage = $stage
        message = $failure
        harnessSha256 = $harnessHash
        modes = $modeReports
        diskDiagnostics = $diskDiagnostics
        outputPartitionDiagnostics = $outputPartitionDiagnostics
        ownedOutputFilesReset = $ownedOutputFilesReset
        shadowsObservedAfterFailure = if ($null -eq $observedShadows) {
            $null
        }
        else {
            $observedShadows.Count
        }
        capacityFinalExists = [IO.File]::Exists($capacityPath)
        capacityPartialExists = [IO.File]::Exists("$capacityPath.partial")
        cancelFinalExists = [IO.File]::Exists($cancelPath)
        cancelPartialExists = [IO.File]::Exists("$cancelPath.partial")
        finalExists = [IO.File]::Exists($finalPath)
        finalPartialExists = [IO.File]::Exists("$finalPath.partial")
        completedUtc = [DateTimeOffset]::UtcNow
    }
    Write-Utf8Text `
        -Path $summaryPath `
        -Value ($summary | ConvertTo-Json -Depth 8)
    Write-Utf8Text -Path $donePath -Value 'FAIL'
    exit 1
}
