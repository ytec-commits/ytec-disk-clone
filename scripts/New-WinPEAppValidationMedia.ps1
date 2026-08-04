param(
    [Parameter(Mandatory)]
    [string]$OutputRoot,

    [ValidateSet('2011CA', '2023CA')]
    [string]$CertificateGeneration = '2023CA',

    [ValidateSet('Inventory', 'StandaloneBootRepair')]
    [string]$ValidationScenario = 'Inventory',

    [string]$DiagnosticPath = '',

    [string]$WinPEAppPath = '',

    [string]$WinPEGuiPath = '',

    [string]$FinalIsoPath = '',

    [ValidatePattern('^$|^[A-Za-z]:$')]
    [string]$TargetUsbDrive = '',

    [int]$ExpectedUsbDiskNumber = -1,

    [UInt64]$ExpectedUsbSizeBytes = 0,

    [string]$ExpectedUsbSerialSuffix = '',

    [string]$ExpectedUsbDeviceInstanceId = '',

    [switch]$BuildUsb,

    [switch]$BuildMedia
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Text.UTF8Encoding]::new($false)

function Assert-ExternalNewOutputPath {
    param(
        [Parameter(Mandatory)]
        [string]$RepositoryRoot,
        [Parameter(Mandatory)]
        [string]$CandidatePath
    )

    $repositoryFullPath = [IO.Path]::GetFullPath($RepositoryRoot)
    $outputFullPath = [IO.Path]::GetFullPath($CandidatePath)
    $repositoryPrefix = $repositoryFullPath.TrimEnd('\') + '\'
    if ($outputFullPath.Equals(
            $repositoryFullPath,
            [StringComparison]::OrdinalIgnoreCase) -or
        $outputFullPath.StartsWith(
            $repositoryPrefix,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Microsoft WinPE/ADK生成物はリポジトリ外だけに作成できます。'
    }

    $pathRoot = [IO.Path]::GetPathRoot($outputFullPath)
    if ([string]::IsNullOrWhiteSpace($pathRoot) -or
        $outputFullPath.TrimEnd('\').Equals(
            $pathRoot.TrimEnd('\'),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'ドライブ直下を出力先には指定できません。'
    }

    if (Test-Path -LiteralPath $outputFullPath) {
        throw "既存の出力先は上書きしません: $outputFullPath"
    }

    $ancestor = Split-Path -Parent $outputFullPath
    while (-not [string]::IsNullOrWhiteSpace($ancestor) -and
        -not (Test-Path -LiteralPath $ancestor)) {
        $next = Split-Path -Parent $ancestor
        if ($next -eq $ancestor) {
            break
        }
        $ancestor = $next
    }
    while (-not [string]::IsNullOrWhiteSpace($ancestor)) {
        $item = Get-Item -LiteralPath $ancestor -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "出力先の既存祖先にreparse pointがあります: $ancestor"
        }
        $next = Split-Path -Parent $ancestor
        if ([string]::IsNullOrWhiteSpace($next) -or $next -eq $ancestor) {
            break
        }
        $ancestor = $next
    }

    return $outputFullPath
}

function Assert-RegularNonReparseFile {
    param(
        [Parameter(Mandatory)]
        [string]$Path,
        [Parameter(Mandatory)]
        [string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description が見つかりません: $Path"
    }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description のreparse pointは使用しません: $Path"
    }
}

function Assert-MicrosoftSignature {
    param(
        [Parameter(Mandatory)]
        [string]$Path,
        [Parameter(Mandatory)]
        [string]$Description
    )

    Assert-RegularNonReparseFile -Path $Path -Description $Description
    $signature = Get-AuthenticodeSignature -FilePath $Path
    if ($signature.Status -ne
            [Management.Automation.SignatureStatus]::Valid -or
        $null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Subject -notmatch
            '(^|,\s*)O=Microsoft Corporation(,|$)') {
        throw "$Description の有効なMicrosoft署名を確認できません: $Path"
    }
}

function Copy-VerifiedMountedWimEfiFile {
    param(
        [Parameter(Mandatory)]
        [string]$MountRoot,

        [Parameter(Mandatory)]
        [string]$SourcePath,

        [Parameter(Mandatory)]
        [string]$DestinationPath,

        [Parameter(Mandatory)]
        [string]$FsutilPath,

        [Parameter(Mandatory)]
        [string]$Description
    )

    $mountFullPath = [IO.Path]::GetFullPath($MountRoot).TrimEnd('\')
    $mountPrefix = $mountFullPath + '\'
    $sourceFullPath = [IO.Path]::GetFullPath($SourcePath)
    if (-not $sourceFullPath.StartsWith(
            $mountPrefix,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description が固定WIMマウント先の外側です: $sourceFullPath"
    }
    if (-not (Test-Path -LiteralPath $sourceFullPath -PathType Leaf)) {
        throw "$Description が見つかりません: $sourceFullPath"
    }

    $ancestor = Split-Path -Parent $sourceFullPath
    while ($ancestor.StartsWith(
            $mountPrefix,
            [StringComparison]::OrdinalIgnoreCase) -or
        $ancestor.Equals(
            $mountFullPath,
            [StringComparison]::OrdinalIgnoreCase)) {
        $ancestorItem = Get-Item -LiteralPath $ancestor -Force
        if (($ancestorItem.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Description の親フォルダーにreparse pointがあります: $ancestor"
        }
        if ($ancestor.Equals(
                $mountFullPath,
                [StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $ancestor = Split-Path -Parent $ancestor
    }

    $sourceItem = Get-Item -LiteralPath $sourceFullPath -Force
    $sourceIsWimProjection = ($sourceItem.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -ne 0
    if ($sourceIsWimProjection) {
        # DISMでマウントしたWIMは通常ファイルにもIO_REPARSE_TAG_WIMを付ける。
        # 任意のリンクは許可せず、Windows SDK定義のWIMタグだけを許可する。
        $reparseOutput = @(
            & $FsutilPath reparsepoint query $sourceFullPath 2>&1
        )
        $reparseExitCode = $LASTEXITCODE
        $reparseText = $reparseOutput -join [Environment]::NewLine
        if ($reparseExitCode -ne 0 -or
            $reparseText -notmatch '(?i)\b0x80000008\b') {
            throw "$Description が許可済みWIM投影ではありません: $sourceFullPath"
        }
    }

    if ($sourceItem.Length -lt 64KB -or $sourceItem.Length -gt 16MB) {
        throw "$Description のサイズが許可範囲外です: $($sourceItem.Length)"
    }
    if (Test-Path -LiteralPath $DestinationPath) {
        throw "$Description の検証用コピー先を上書きしません: $DestinationPath"
    }

    $sourceHashBefore = (Get-FileHash -LiteralPath $sourceFullPath `
        -Algorithm SHA256).Hash
    Copy-Item -LiteralPath $sourceFullPath -Destination $DestinationPath
    Assert-RegularNonReparseFile `
        -Path $DestinationPath `
        -Description "$Description の検証用コピー"

    $destinationItem = Get-Item -LiteralPath $DestinationPath -Force
    $bytes = [IO.File]::ReadAllBytes($destinationItem.FullName)
    if ($bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "$Description にDOS MZ署名がありません。"
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -lt 0x40 -or $peOffset -gt ($bytes.Length - 94) -or
        $bytes[$peOffset] -ne 0x50 -or
        $bytes[$peOffset + 1] -ne 0x45 -or
        $bytes[$peOffset + 2] -ne 0 -or
        $bytes[$peOffset + 3] -ne 0) {
        throw "$Description のPEヘッダーが不正です。"
    }
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    $optionalHeaderMagic = [BitConverter]::ToUInt16($bytes, $peOffset + 24)
    $subsystem = [BitConverter]::ToUInt16($bytes, $peOffset + 92)
    if ($machine -ne 0x8664 -or
        $optionalHeaderMagic -ne 0x020B -or
        $subsystem -ne 10) {
        throw ("$Description はAMD64 PE32+ EFI Applicationではありません: " +
            ('machine=0x{0:X4}, optionalMagic=0x{1:X4}, subsystem={2}' -f
                $machine, $optionalHeaderMagic, $subsystem))
    }
    Assert-MicrosoftSignature `
        -Path $destinationItem.FullName `
        -Description "$Description の検証用コピー"

    $sourceHashAfter = (Get-FileHash -LiteralPath $sourceFullPath `
        -Algorithm SHA256).Hash
    $destinationHash = (Get-FileHash -LiteralPath $destinationItem.FullName `
        -Algorithm SHA256).Hash
    if ($sourceHashBefore -ne $sourceHashAfter -or
        $sourceHashBefore -ne $destinationHash) {
        throw "$Description のコピー中にSHA-256が変化しました。"
    }

    return [ordered]@{
        sourceRelativePath = $sourceFullPath.Substring(
            $mountFullPath.Length).TrimStart('\')
        length = $destinationItem.Length
        sha256 = $destinationHash
        sourceWasWimProjection = $sourceIsWimProjection
        reparseTag = if ($sourceIsWimProjection) { '0x80000008' } else { '' }
        machine = 'AMD64'
        optionalHeader = 'PE32+'
        subsystem = 'EFI Application'
        microsoftSignatureVerified = $true
        trustAnchor = 'SHA-256-recorded ADK WinPE WIM plus materialized file verification'
    }
}

function Get-WinPEAppPeReport {
    param(
        [Parameter(Mandatory)]
        [string]$Path,

        [Parameter(Mandatory)]
        [string[]]$AllowedDependencies,

        [Parameter(Mandatory)]
        [string]$Description
    )

    Assert-RegularNonReparseFile -Path $Path -Description $Description
    $item = Get-Item -LiteralPath $Path
    if ($item.Length -lt 512 -or $item.Length -gt 64MB) {
        throw "$Description のファイルサイズが許可範囲外です: $($item.Length)"
    }

    $bytes = [IO.File]::ReadAllBytes($item.FullName)
    if ($bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "$Description にDOS MZ署名がありません。"
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -lt 0x40 -or $peOffset -gt ($bytes.Length - 26)) {
        throw "$Description のPEヘッダー位置が不正です。"
    }
    if ($bytes[$peOffset] -ne 0x50 -or
        $bytes[$peOffset + 1] -ne 0x45 -or
        $bytes[$peOffset + 2] -ne 0 -or
        $bytes[$peOffset + 3] -ne 0) {
        throw "$Description にPE署名がありません。"
    }

    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    $optionalHeaderMagic = [BitConverter]::ToUInt16($bytes, $peOffset + 24)
    if ($machine -ne 0x8664 -or $optionalHeaderMagic -ne 0x020B) {
        throw ("$Description はAMD64 PE32+でなければなりません: " +
            ('machine=0x{0:X4}, optionalMagic=0x{1:X4}' -f
                $machine, $optionalHeaderMagic))
    }

    $ascii = [Text.Encoding]::ASCII.GetString($bytes)
    $dependencies = @(
        [regex]::Matches($ascii, '(?i)[A-Za-z0-9._-]+\.dll') |
            ForEach-Object Value |
            Sort-Object -Unique
    )
    $unexpected = @(
        $dependencies |
            Where-Object { $AllowedDependencies -notcontains $_ }
    )
    $missing = @(
        $AllowedDependencies |
            Where-Object { $dependencies -notcontains $_ }
    )
    if ($unexpected.Count -gt 0 -or $missing.Count -gt 0) {
        throw ("$Description のDLL依存が固定許可リストと一致しません。" +
            " unexpected=[$($unexpected -join ',')]" +
            " missing=[$($missing -join ',')]")
    }

    return [ordered]@{
        path = $item.FullName
        length = $item.Length
        sha256 = (Get-FileHash -LiteralPath $item.FullName `
            -Algorithm SHA256).Hash
        machine = 'AMD64'
        optionalHeader = 'PE32+'
        dependentDlls = $dependencies
    }
}

function Invoke-CheckedNative {
    param(
        [Parameter(Mandatory)]
        [string]$Command,
        [Parameter(Mandatory)]
        [string[]]$Arguments,
        [Parameter(Mandatory)]
        [string]$Operation
    )

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Operation が終了コード $LASTEXITCODE で失敗しました。"
    }
}

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Write-MediaProgress {
    param(
        [Parameter(Mandatory)]
        [ValidateRange(0, 100)]
        [int]$Percent,

        [Parameter(Mandatory)]
        [ValidatePattern('^[a-z0-9-]+$')]
        [string]$Stage
    )

    Write-Output ("TSUMUGI_MEDIA_PROGRESS={0}|{1}" -f $Percent, $Stage)
}

function Get-NormalizedSerialSuffix {
    param([AllowNull()][string]$SerialNumber)

    if ([string]::IsNullOrWhiteSpace($SerialNumber)) {
        return ''
    }
    $printable = -join @(
        $SerialNumber.ToCharArray() |
            Where-Object {
                [int]$_ -ge 0x20 -and [int]$_ -le 0x7E
            }
    )
    $trimmed = $printable.Trim()
    if ($trimmed.Length -le 8) {
        return $trimmed
    }
    return $trimmed.Substring($trimmed.Length - 8)
}

function Get-VerifiedUsbDisk {
    param(
        [Parameter(Mandatory)]
        [ValidateRange(0, [int]::MaxValue)]
        [int]$DiskNumber,
        [Parameter(Mandatory)]
        [UInt64]$SizeBytes,
        [AllowEmptyString()]
        [string]$SerialSuffix,
        [Parameter(Mandatory)]
        [string]$DeviceInstanceId
    )

    $disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
    if ([UInt64]$disk.Size -ne $SizeBytes) {
        throw 'USBの容量が確認時から変化しました。'
    }
    if ([string]$disk.BusType -ne 'USB' -or
        [bool]$disk.IsSystem -or [bool]$disk.IsBoot -or
        [bool]$disk.IsReadOnly -or [bool]$disk.IsOffline) {
        throw '選択先がオンライン・書込み可能・非システムのUSBではありません。'
    }
    $operationalStatuses = @(
        $disk.OperationalStatus | ForEach-Object { [string]$_ }
    )
    if ($operationalStatuses -notcontains 'Online') {
        throw '選択USBがオンライン状態ではありません。'
    }

    $cimDisks = @(
        Get-CimInstance -ClassName Win32_DiskDrive -ErrorAction Stop |
            Where-Object { [int]$_.Index -eq $DiskNumber }
    )
    if ($cimDisks.Count -ne 1) {
        throw '選択USBのデバイス識別情報を一意に再取得できません。'
    }
    $observedDeviceId = [string]$cimDisks[0].PNPDeviceID
    if (-not $observedDeviceId.Equals(
            $DeviceInstanceId,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'USBのデバイス識別情報が確認時から変化しました。'
    }
    $observedSuffix = Get-NormalizedSerialSuffix `
        -SerialNumber ([string]$cimDisks[0].SerialNumber)
    if (-not [string]::IsNullOrEmpty($SerialSuffix) -and
        $observedSuffix -ne $SerialSuffix) {
        throw 'USBのシリアル末尾が確認時から変化しました。'
    }

    return [ordered]@{
        disk = $disk
        diskNumber = $DiskNumber
        sizeBytes = $SizeBytes
        serialSuffix = $SerialSuffix
        deviceInstanceId = $observedDeviceId
        partitionStyle = [string]$disk.PartitionStyle
    }
}

function Get-UsbPartitionsAllowEmpty {
    param(
        [Parameter(Mandatory)]
        [ValidateRange(0, [int]::MaxValue)]
        [int]$DiskNumber,
        [Parameter(Mandatory)]
        [UInt64]$SizeBytes,
        [AllowEmptyString()]
        [string]$SerialSuffix,
        [Parameter(Mandatory)]
        [string]$DeviceInstanceId
    )

    try {
        return @(Get-Partition -DiskNumber $DiskNumber -ErrorAction Stop)
    } catch {
        if ([string]$_.FullyQualifiedErrorId -notlike
            'CmdletizationQuery_NotFound_DiskNumber,Get-Partition*') {
            throw
        }

        # Some Storage providers report an empty initialized disk as an
        # ObjectNotFound error instead of returning an empty partition list.
        # Re-verify the stable USB identity before treating that result as empty.
        Get-VerifiedUsbDisk `
            -DiskNumber $DiskNumber `
            -SizeBytes $SizeBytes `
            -SerialSuffix $SerialSuffix `
            -DeviceInstanceId $DeviceInstanceId | Out-Null
        return @()
    }
}

function Get-VerifiedUsbTarget {
    param(
        [Parameter(Mandatory)]
        [ValidatePattern('^[A-Za-z]:$')]
        [string]$Drive,
        [Parameter(Mandatory)]
        [ValidateRange(0, [int]::MaxValue)]
        [int]$DiskNumber,
        [Parameter(Mandatory)]
        [UInt64]$SizeBytes,
        [AllowEmptyString()]
        [string]$SerialSuffix,
        [Parameter(Mandatory)]
        [string]$DeviceInstanceId,
        [switch]$AllowUnpartitioned,
        [switch]$RequireMbr
    )

    $verifiedDisk = Get-VerifiedUsbDisk `
        -DiskNumber $DiskNumber `
        -SizeBytes $SizeBytes `
        -SerialSuffix $SerialSuffix `
        -DeviceInstanceId $DeviceInstanceId
    $allowedStyles = if ($RequireMbr) {
        @('MBR')
    } elseif ($AllowUnpartitioned) {
        @('RAW', 'MBR', 'GPT')
    } else {
        @('MBR', 'GPT')
    }
    if ($allowedStyles -notcontains $verifiedDisk.partitionStyle) {
        throw '選択USBは初期化可能なRAW／GPT／MBR構成ではありません。'
    }

    $driveLetter = $Drive.Substring(0, 1).ToUpperInvariant()
    $partitions = @(
        Get-UsbPartitionsAllowEmpty `
            -DiskNumber $DiskNumber `
            -SizeBytes $SizeBytes `
            -SerialSuffix $SerialSuffix `
            -DeviceInstanceId $DeviceInstanceId
    )
    if ($verifiedDisk.partitionStyle -eq 'RAW' -and
        $partitions.Count -ne 0) {
        throw 'RAWとして列挙されたUSBにパーティションがあるため停止しました。'
    }
    $root = "$driveLetter`:\"
    if ($partitions.Count -eq 0 -and $AllowUnpartitioned) {
        if ($null -ne (Get-PSDrive `
                -Name $driveLetter `
                -PSProvider FileSystem `
                -ErrorAction SilentlyContinue) -or
            (Test-Path -LiteralPath $root -PathType Container)) {
            throw '区画のないUSBへ割当予定のドライブ文字が既に使用されています。'
        }
        $partitionNumber = 0
    } elseif ($partitions.Count -eq 1 -and
        [string]$partitions[0].DriveLetter -eq $driveLetter) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) {
            throw '選択USBのルートを再確認できません。'
        }
        $partitionNumber = [int]$partitions[0].PartitionNumber
    } else {
        throw '選択USBの単一パーティションとドライブ文字を再確認できません。'
    }

    return [ordered]@{
        disk = $verifiedDisk.disk
        diskNumber = $DiskNumber
        drive = "$driveLetter`:"
        root = $root
        sizeBytes = $SizeBytes
        serialSuffix = $SerialSuffix
        deviceInstanceId = $verifiedDisk.deviceInstanceId
        partitionStyle = $verifiedDisk.partitionStyle
        partitionNumber = $partitionNumber
    }
}

function Initialize-VerifiedUsbTarget {
    param(
        [Parameter(Mandatory)]
        [ValidatePattern('^[A-Za-z]:$')]
        [string]$Drive,
        [Parameter(Mandatory)]
        [ValidateRange(0, [int]::MaxValue)]
        [int]$DiskNumber,
        [Parameter(Mandatory)]
        [UInt64]$SizeBytes,
        [AllowEmptyString()]
        [string]$SerialSuffix,
        [Parameter(Mandatory)]
        [string]$DeviceInstanceId
    )

    $before = Get-VerifiedUsbTarget `
        -Drive $Drive `
        -DiskNumber $DiskNumber `
        -SizeBytes $SizeBytes `
        -SerialSuffix $SerialSuffix `
        -DeviceInstanceId $DeviceInstanceId `
        -AllowUnpartitioned
    $driveLetter = [char]$Drive.Substring(0, 1).ToUpperInvariant()

    if ($before.partitionNumber -ne 0) {
        Clear-Disk `
            -InputObject $before.disk `
            -RemoveData `
            -RemoveOEM `
            -Confirm:$false `
            -ErrorAction Stop
        Update-Disk -Number $DiskNumber -ErrorAction Stop | Out-Null
    }
    $cleared = Get-VerifiedUsbDisk `
        -DiskNumber $DiskNumber `
        -SizeBytes $SizeBytes `
        -SerialSuffix $SerialSuffix `
        -DeviceInstanceId $DeviceInstanceId
    $clearedPartitions = @(
        Get-UsbPartitionsAllowEmpty `
            -DiskNumber $DiskNumber `
            -SizeBytes $SizeBytes `
            -SerialSuffix $SerialSuffix `
            -DeviceInstanceId $DeviceInstanceId
    )
    if ($clearedPartitions.Count -ne 0) {
        throw '選択USBの消去後もパーティションが残っているため停止しました。'
    }

    if ($cleared.partitionStyle -eq 'RAW') {
        Initialize-Disk `
            -InputObject $cleared.disk `
            -PartitionStyle MBR `
            -ErrorAction Stop | Out-Null
    } elseif ($cleared.partitionStyle -eq 'GPT') {
        Set-Disk `
            -InputObject $cleared.disk `
            -PartitionStyle MBR `
            -ErrorAction Stop | Out-Null
    } elseif ($cleared.partitionStyle -ne 'MBR') {
        throw '選択USBの消去後のパーティション形式が不明なため停止しました。'
    }
    Update-Disk -Number $DiskNumber -ErrorAction Stop | Out-Null
    $initialized = Get-VerifiedUsbDisk `
        -DiskNumber $DiskNumber `
        -SizeBytes $SizeBytes `
        -SerialSuffix $SerialSuffix `
        -DeviceInstanceId $DeviceInstanceId
    $initializedPartitions = @(
        Get-UsbPartitionsAllowEmpty `
            -DiskNumber $DiskNumber `
            -SizeBytes $SizeBytes `
            -SerialSuffix $SerialSuffix `
            -DeviceInstanceId $DeviceInstanceId
    )
    if ($initialized.partitionStyle -ne 'MBR' -or
        $initializedPartitions.Count -ne 0) {
        throw '選択USBを空のMBRディスクとして確認できません。'
    }
    $maximumFat32Bytes = [UInt64](30GB)
    $partition = if ($SizeBytes -gt ($maximumFat32Bytes + 4MB)) {
        New-Partition `
            -InputObject $initialized.disk `
            -Size $maximumFat32Bytes `
            -DriveLetter $driveLetter `
            -ErrorAction Stop
    } else {
        New-Partition `
            -InputObject $initialized.disk `
            -UseMaximumSize `
            -DriveLetter $driveLetter `
            -ErrorAction Stop
    }
    Format-Volume `
        -Partition $partition `
        -FileSystem FAT32 `
        -NewFileSystemLabel 'TSUMUGI' `
        -Force `
        -Confirm:$false `
        -ErrorAction Stop | Out-Null

    return Get-VerifiedUsbTarget `
        -Drive $Drive `
        -DiskNumber $DiskNumber `
        -SizeBytes $SizeBytes `
        -SerialSuffix $SerialSuffix `
        -DeviceInstanceId $DeviceInstanceId `
        -RequireMbr
}

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$outputFullPath = Assert-ExternalNewOutputPath `
    -RepositoryRoot $repoRoot `
    -CandidatePath $OutputRoot

$finalIsoFullPath = ''
$finalManifestFullPath = ''
if (-not [string]::IsNullOrWhiteSpace($FinalIsoPath)) {
    $finalIsoFullPath = Assert-ExternalNewOutputPath `
        -RepositoryRoot $repoRoot `
        -CandidatePath $FinalIsoPath
    if (-not $finalIsoFullPath.EndsWith(
            '.iso',
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "完成メディアの拡張子は.isoでなければなりません: $finalIsoFullPath"
    }
    $finalParent = Split-Path -Parent $finalIsoFullPath
    if (-not (Test-Path -LiteralPath $finalParent -PathType Container)) {
        throw "完成ISOの親フォルダーがありません: $finalParent"
    }
    $outputPrefix = $outputFullPath.TrimEnd('\') + '\'
    if ($finalIsoFullPath.StartsWith(
            $outputPrefix,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw '完成ISOは一時作業フォルダーの外側に指定してください。'
    }
    $finalManifestFullPath = Assert-ExternalNewOutputPath `
        -RepositoryRoot $repoRoot `
        -CandidatePath ($finalIsoFullPath + '.manifest.json')
}
if ($BuildUsb) {
    if (-not [string]::IsNullOrWhiteSpace($FinalIsoPath)) {
        throw 'USB作成時はISO保存先を同時に指定できません。'
    }
    if ([string]::IsNullOrWhiteSpace($TargetUsbDrive) -or
        $ExpectedUsbDiskNumber -lt 0 -or
        $ExpectedUsbSizeBytes -eq 0 -or
        [string]::IsNullOrWhiteSpace($ExpectedUsbDeviceInstanceId)) {
        throw 'USB作成にはドライブ文字、ディスク番号、容量、デバイス識別情報が必要です。'
    }
} elseif (
    -not [string]::IsNullOrWhiteSpace($TargetUsbDrive) -or
    $ExpectedUsbDiskNumber -ne -1 -or
    $ExpectedUsbSizeBytes -ne 0 -or
    -not [string]::IsNullOrWhiteSpace($ExpectedUsbSerialSuffix) -or
    -not [string]::IsNullOrWhiteSpace($ExpectedUsbDeviceInstanceId)) {
    throw 'USB対象情報は-BuildUsbを指定した場合だけ使用できます。'
}

$diagnostic = if ([string]::IsNullOrWhiteSpace($DiagnosticPath)) {
    Join-Path $repoRoot `
        'out\build\msvc-x64\src\MediaBuilder\ytec-winpe-environment.exe'
} else {
    [IO.Path]::GetFullPath($DiagnosticPath)
}
Assert-RegularNonReparseFile `
    -Path $diagnostic `
    -Description 'WinPE環境診断CLI'

$diagnosticText = (& $diagnostic --json | Out-String)
$diagnosticExit = $LASTEXITCODE
if ($diagnosticExit -ne 0) {
    throw "WinPE環境診断が終了コード $diagnosticExit で作成を拒否しました。"
}
$diagnosticReport = $diagnosticText | ConvertFrom-Json
if (-not $diagnosticReport.mediaCreationPermitted -or
    $null -eq $diagnosticReport.selectedCandidateIndex) {
    throw 'WinPE環境診断の作成許可ゲートを通過していません。'
}

$candidate = $diagnosticReport.candidates[
    [int]$diagnosticReport.selectedCandidateIndex]
$adkRoot = [IO.Path]::GetFullPath([string]$candidate.root)
$winpeRoot = Join-Path $adkRoot 'Windows Preinstallation Environment'
$deploymentRoot = Join-Path $adkRoot 'Deployment Tools'
$sourceMedia = Join-Path $winpeRoot 'amd64\Media'
$sourceWim = Join-Path $winpeRoot 'amd64\en-us\winpe.wim'
$japaneseFontSupport = Join-Path $winpeRoot `
    'amd64\WinPE_OCs\WinPE-FontSupport-JA-JP.cab'
$dism = Join-Path $deploymentRoot 'amd64\DISM\dism.exe'
$oscdimgRoot = Join-Path $deploymentRoot 'amd64\Oscdimg'
$oscdimg = Join-Path $oscdimgRoot 'oscdimg.exe'
$bootsectRoot = Join-Path $deploymentRoot 'amd64\BCDBoot'
$bootsect = Join-Path $bootsectRoot 'bootsect.exe'
$makeWinPEMedia = Join-Path $winpeRoot 'MakeWinPEMedia.cmd'
$systemCmd = Join-Path $env:SystemRoot 'System32\cmd.exe'
$fsutil = Join-Path $env:SystemRoot 'System32\fsutil.exe'
$etfsboot = Join-Path $oscdimgRoot 'etfsboot.com'
$efiBootImage = if ($CertificateGeneration -eq '2023CA') {
    Join-Path $oscdimgRoot 'efisys_EX.bin'
} else {
    Join-Path $oscdimgRoot 'efisys.bin'
}

if (-not (Test-Path -LiteralPath $sourceMedia -PathType Container)) {
    throw "WinPE amd64 Mediaが見つかりません: $sourceMedia"
}
foreach ($required in @(
        $sourceWim,
        $japaneseFontSupport,
        $makeWinPEMedia,
        $etfsboot,
        $efiBootImage)) {
    Assert-RegularNonReparseFile -Path $required -Description 'ADK構成要素'
}
Assert-MicrosoftSignature -Path $dism -Description 'DISM'
Assert-MicrosoftSignature -Path $oscdimg -Description 'Oscdimg'
Assert-MicrosoftSignature -Path $bootsect -Description 'Bootsect'
Assert-MicrosoftSignature -Path $systemCmd -Description 'Windows cmd'
Assert-MicrosoftSignature -Path $fsutil -Description 'Windows Fsutil'

$winpeApp = if ([string]::IsNullOrWhiteSpace($WinPEAppPath)) {
    Join-Path $repoRoot `
        'out\build\msvc-x64-vm\src\WinPEApp\ytec-winpe-app.exe'
} else {
    [IO.Path]::GetFullPath($WinPEAppPath)
}
$winpeGui = if ([string]::IsNullOrWhiteSpace($WinPEGuiPath)) {
    Join-Path $repoRoot `
        'out\build\msvc-x64-vm\src\WinPEApp\ytec-winpe-gui.exe'
} else {
    [IO.Path]::GetFullPath($WinPEGuiPath)
}
$coreDependencies = @(
    'ADVAPI32.dll', 'bcrypt.dll', 'CRYPT32.dll', 'KERNEL32.dll',
    'ole32.dll', 'SETUPAPI.dll', 'WINTRUST.dll')
$appReport = Get-WinPEAppPeReport `
    -Path $winpeApp `
    -AllowedDependencies $coreDependencies `
    -Description 'WinPE CLI'
$guiReport = Get-WinPEAppPeReport `
    -Path $winpeGui `
    -AllowedDependencies ($coreDependencies + @(
        'GDI32.dll', 'USER32.dll')) `
    -Description 'WinPE GUI'
$guiReport['dynamicallyLoadedSystemDlls'] = @('comdlg32.dll')
$lineSeedLicense = Join-Path $repoRoot `
    'licenses\LINE-Seed-JP-OFL-1.1.txt'
$thirdPartyNotices = Join-Path $repoRoot 'THIRD-PARTY-NOTICES.txt'
Assert-RegularNonReparseFile `
    -Path $lineSeedLicense `
    -Description 'LINE Seed JP OFL 1.1ライセンス'
Assert-RegularNonReparseFile `
    -Path $thirdPartyNotices `
    -Description '第三者ライセンス通知'
$lineSeedLicenseReport = [ordered]@{
    name = 'LINE Seed JP'
    version = 'LINESeedJP_20241105'
    license = 'OFL-1.1'
    path = $lineSeedLicense
    length = (Get-Item -LiteralPath $lineSeedLicense).Length
    sha256 = (Get-FileHash -LiteralPath $lineSeedLicense `
        -Algorithm SHA256).Hash
}

$preflight = [ordered]@{
    schemaVersion = 1
    outputRoot = $outputFullPath
    outputExists = $false
    certificateGeneration = $CertificateGeneration
    validationScenario = $ValidationScenario
    adkVersion = [string]$candidate.deploymentToolsVersion
    dismVersion = [string]$candidate.dismFileVersion
    servicingUpdate = 'KB5101684'
    sourceWim = [ordered]@{
        path = $sourceWim
        length = (Get-Item -LiteralPath $sourceWim).Length
        sha256 = (Get-FileHash -LiteralPath $sourceWim `
            -Algorithm SHA256).Hash
    }
    japaneseFontSupport = [ordered]@{
        source = 'Installed ADK WinPE optional component'
        repositoryCopy = $false
        path = $japaneseFontSupport
        length = (Get-Item -LiteralPath $japaneseFontSupport).Length
        sha256 = (Get-FileHash -LiteralPath $japaneseFontSupport `
            -Algorithm SHA256).Hash
    }
    winpeApp = $appReport
    winpeGui = $guiReport
    lineSeedLicense = $lineSeedLicenseReport
    finalIsoPath = $finalIsoFullPath
    finalManifestPath = $finalManifestFullPath
    buildUsbRequested = [bool]$BuildUsb
    targetUsbDrive = $TargetUsbDrive.ToUpperInvariant()
    expectedUsbDiskNumber = $ExpectedUsbDiskNumber
    expectedUsbSizeBytes = $ExpectedUsbSizeBytes
    expectedUsbSerialSuffix = $ExpectedUsbSerialSuffix
    buildRequested = [bool]$BuildMedia
    administrator = Test-IsAdministrator
}

if (-not $BuildMedia) {
    Write-Output ('WINPE_APP_MEDIA_PREFLIGHT_PASS=' +
        ($preflight | ConvertTo-Json -Depth 6 -Compress))
    return
}

if (-not $preflight.administrator) {
    throw '実WIMのマウント/コミットには管理者権限が必要です。UACを承認したPowerShellで-BuildMediaを実行してください。'
}

$initialUsbTarget = $null
if ($BuildUsb) {
    $initialUsbTarget = Get-VerifiedUsbTarget `
        -Drive $TargetUsbDrive `
        -DiskNumber $ExpectedUsbDiskNumber `
        -SizeBytes $ExpectedUsbSizeBytes `
        -SerialSuffix $ExpectedUsbSerialSuffix `
        -DeviceInstanceId $ExpectedUsbDeviceInstanceId `
        -AllowUnpartitioned
}

Write-MediaProgress -Percent 5 -Stage 'preflight'

$sourceReparse = Get-ChildItem -LiteralPath $sourceMedia -Recurse -Force |
    Where-Object {
        ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
    } |
    Select-Object -First 1
if ($null -ne $sourceReparse) {
    throw "WinPE Media内のreparse pointはコピーしません: $($sourceReparse.FullName)"
}

$workingRoot = Join-Path $outputFullPath 'working'
$mediaRoot = Join-Path $workingRoot 'media'
$sourcesRoot = Join-Path $mediaRoot 'sources'
$mountRoot = Join-Path $workingRoot 'mount'
$bootBinsRoot = Join-Path $workingRoot 'bootbins'
$bootWim = Join-Path $sourcesRoot 'boot.wim'
$isoBaseName = if ($ValidationScenario -eq 'StandaloneBootRepair') {
    'YDC-Standalone-Boot-Repair'
} else {
    'YDC-WinPEApp'
}
$isoPath = Join-Path $outputFullPath `
    "$isoBaseName-amd64-$CertificateGeneration.iso"
$manifestPath = Join-Path $outputFullPath 'winpe-app-media-manifest.json'

New-Item -ItemType Directory -Path $outputFullPath | Out-Null
New-Item -ItemType Directory -Path $workingRoot | Out-Null
New-Item -ItemType Directory -Path $bootBinsRoot | Out-Null
Write-MediaProgress -Percent 12 -Stage 'created-working-area'
Copy-Item -LiteralPath $sourceMedia -Destination $mediaRoot -Recurse
if (-not (Test-Path -LiteralPath $sourcesRoot)) {
    New-Item -ItemType Directory -Path $sourcesRoot | Out-Null
}
Copy-Item -LiteralPath $sourceWim -Destination $bootWim
New-Item -ItemType Directory -Path $mountRoot | Out-Null
Write-MediaProgress -Percent 24 -Stage 'staged-adk-media'

$stagedWimBefore = [ordered]@{
    length = (Get-Item -LiteralPath $bootWim).Length
    sha256 = (Get-FileHash -LiteralPath $bootWim -Algorithm SHA256).Hash
}

$mounted = $false
$uefiBootManagers = @()
try {
    Invoke-CheckedNative `
        -Command $dism `
        -Arguments @(
            '/Mount-Image',
            "/ImageFile:$bootWim",
            '/Index:1',
            "/MountDir:$mountRoot"
        ) `
        -Operation 'WinPE boot.wimのマウント'
    $mounted = $true
    Write-MediaProgress -Percent 38 -Stage 'mounted-wim'

    $normalBootManager = Join-Path `
        $mountRoot 'Windows\Boot\EFI\bootmgfw.efi'
    $uefiBootManagers += Copy-VerifiedMountedWimEfiFile `
        -MountRoot $mountRoot `
        -SourcePath $normalBootManager `
        -DestinationPath (Join-Path $bootBinsRoot 'bootmgfw.efi') `
        -FsutilPath $fsutil `
        -Description 'WinPE 2011 CA UEFIブートマネージャー'
    if ($CertificateGeneration -eq '2023CA') {
        $bootExManager = Join-Path `
            $mountRoot 'Windows\Boot\EFI_EX\bootmgfw_EX.efi'
        $uefiBootManagers += Copy-VerifiedMountedWimEfiFile `
            -MountRoot $mountRoot `
            -SourcePath $bootExManager `
            -DestinationPath (Join-Path $bootBinsRoot 'bootmgfw_EX.efi') `
            -FsutilPath $fsutil `
            -Description 'WinPE 2023 CA UEFIブートマネージャー'
    }

    Invoke-CheckedNative `
        -Command $dism `
        -Arguments @(
            "/Image:$mountRoot",
            '/Add-Package',
            "/PackagePath:$japaneseFontSupport"
        ) `
        -Operation 'WinPE日本語フォントサポートの追加'
    Write-MediaProgress -Percent 50 -Stage 'added-japanese-font'

    $payloadRoot = Join-Path $mountRoot 'YtecDiskClone'
    $mountedApp = Join-Path $payloadRoot 'ytec-winpe-app.exe'
    $mountedGui = Join-Path $payloadRoot 'ytec-winpe-gui.exe'
    $mountedNotices = Join-Path $payloadRoot 'THIRD-PARTY-NOTICES.txt'
    $mountedLicenses = Join-Path $payloadRoot 'licenses'
    $mountedLineSeedLicense = Join-Path $mountedLicenses `
        'LINE-Seed-JP-OFL-1.1.txt'
    $launchScript = Join-Path $payloadRoot 'launch.cmd'
    $uefiDiskPart = Join-Path $payloadRoot 'assign-uefi.txt'
    $biosDiskPart = Join-Path $payloadRoot 'assign-bios.txt'
    $winpeshl = Join-Path $mountRoot 'Windows\System32\winpeshl.ini'
    foreach ($reserved in @($payloadRoot, $winpeshl)) {
        if (Test-Path -LiteralPath $reserved) {
            throw "標準WIM内の予約先が既に存在するため上書きしません: $reserved"
        }
    }

    New-Item -ItemType Directory -Path $payloadRoot | Out-Null
    New-Item -ItemType Directory -Path $mountedLicenses | Out-Null
    Copy-Item -LiteralPath $winpeApp -Destination $mountedApp
    Copy-Item -LiteralPath $winpeGui -Destination $mountedGui
    Copy-Item -LiteralPath $thirdPartyNotices -Destination $mountedNotices
    Copy-Item -LiteralPath $lineSeedLicense `
        -Destination $mountedLineSeedLicense
    if ($ValidationScenario -eq 'StandaloneBootRepair') {
        @(
            'select disk 0',
            'select partition 1',
            'assign letter=W noerr',
            'select partition 2',
            'assign letter=S noerr',
            'exit'
        ) | Set-Content -LiteralPath $uefiDiskPart -Encoding ascii
        @(
            'select disk 0',
            'select partition 1',
            'assign letter=W noerr',
            'exit'
        ) | Set-Content -LiteralPath $biosDiskPart -Encoding ascii
        @(
            '@echo off',
            'setlocal EnableExtensions',
            'chcp 65001 >nul',
            'wpeutil UpdateBootInfo',
            'set "YTEC_PEFW="',
            'for /f "tokens=3" %%F in (''reg query HKLM\System\CurrentControlSet\Control /v PEFirmwareType 2^>nul ^| find "PEFirmwareType"'') do set "YTEC_PEFW=%%F"',
            'if /i "%YTEC_PEFW%"=="0x2" goto uefi',
            'if /i "%YTEC_PEFW%"=="0x1" goto bios',
            'echo YDC_STANDALONE_BOOT_REPAIR_FAIL stage=firmware',
            'goto failed_no_volume',
            ':uefi',
            'diskpart /s %SYSTEMDRIVE%\YtecDiskClone\assign-uefi.txt',
            'set "YTEC_FIRMWARE=uefi"',
            'set "YTEC_SYSTEM_ROOT=S:\"',
            'goto mapped',
            ':bios',
            'diskpart /s %SYSTEMDRIVE%\YtecDiskClone\assign-bios.txt',
            'set "YTEC_FIRMWARE=bios"',
            'set "YTEC_SYSTEM_ROOT=W:\"',
            ':mapped',
            'if not exist W:\Windows\System32\ntoskrnl.exe goto failed_no_volume',
            'set "YTEC_EVIDENCE=W:\YtecValidation\StandaloneBootRepair"',
            'if not exist "%YTEC_EVIDENCE%" mkdir "%YTEC_EVIDENCE%"',
            'if not exist "%YTEC_EVIDENCE%" goto failed_no_volume',
            'icacls "%YTEC_EVIDENCE%" /grant *S-1-1-0:(OI)(CI)F >nul',
            'if exist "%YTEC_EVIDENCE%\break.request" goto break_boot',
            'if exist "%YTEC_EVIDENCE%\confirmation.txt" goto repair_boot',
            ':preflight',
            '%SYSTEMDRIVE%\YtecDiskClone\ytec-winpe-app.exe --boot-repair-preflight --disk 0 --windows-root W:\ --system-root %YTEC_SYSTEM_ROOT% --firmware %YTEC_FIRMWARE% --json > "%YTEC_EVIDENCE%\preflight.json" 2> "%YTEC_EVIDENCE%\preflight.err.txt"',
            'if errorlevel 1 goto failed',
            'echo PREFLIGHT_PASS> "%YTEC_EVIDENCE%\stage.txt"',
            'echo YDC_STANDALONE_BOOT_REPAIR_PREFLIGHT_PASS firmware=%YTEC_FIRMWARE%',
            'goto shutdown',
            ':break_boot',
            'if /i "%YTEC_FIRMWARE%"=="uefi" goto break_uefi',
            'if not exist W:\Boot\BCD goto failed',
            'if exist W:\Boot\BCD.ytec-broken goto failed',
            'ren W:\Boot\BCD BCD.ytec-broken',
            'if exist W:\Boot\BCD goto failed',
            'goto broken',
            ':break_uefi',
            'if not exist S:\EFI\Microsoft\Boot\BCD goto failed',
            'if exist S:\EFI\Microsoft\Boot\BCD.ytec-broken goto failed',
            'ren S:\EFI\Microsoft\Boot\BCD BCD.ytec-broken',
            'if exist S:\EFI\Microsoft\Boot\BCD goto failed',
            ':broken',
            'del /q "%YTEC_EVIDENCE%\break.request"',
            'echo BOOT_STORE_BROKEN> "%YTEC_EVIDENCE%\stage.txt"',
            'echo YDC_STANDALONE_BOOT_REPAIR_BREAK_PASS firmware=%YTEC_FIRMWARE%',
            'goto shutdown',
            ':repair_boot',
            'set "YTEC_CONFIRMATION="',
            'set /p "YTEC_CONFIRMATION="< "%YTEC_EVIDENCE%\confirmation.txt"',
            'if not defined YTEC_CONFIRMATION goto failed',
            '%SYSTEMDRIVE%\YtecDiskClone\ytec-winpe-app.exe --boot-repair-execute --disk 0 --windows-root W:\ --system-root %YTEC_SYSTEM_ROOT% --firmware %YTEC_FIRMWARE% --acknowledge-boot-files-change --confirmation "%YTEC_CONFIRMATION%" --json > "%YTEC_EVIDENCE%\repair.json" 2> "%YTEC_EVIDENCE%\repair.err.txt"',
            'if errorlevel 1 goto failed',
            'echo REPAIR_PASS> "%YTEC_EVIDENCE%\stage.txt"',
            'echo YDC_STANDALONE_BOOT_REPAIR_PASS firmware=%YTEC_FIRMWARE%',
            'goto shutdown',
            ':failed',
            'echo FAIL> "%YTEC_EVIDENCE%\stage.txt"',
            'echo YDC_STANDALONE_BOOT_REPAIR_FAIL firmware=%YTEC_FIRMWARE%',
            'goto shutdown',
            ':failed_no_volume',
            'echo YDC_STANDALONE_BOOT_REPAIR_FAIL stage=volume-mapping',
            ':shutdown',
            'wpeutil shutdown'
        ) | Set-Content -LiteralPath $launchScript -Encoding ascii
    } else {
        @(
            '@echo off',
            'chcp 65001 >nul',
            'echo Y-TEC WinPE read-only disk inventory',
            '%SYSTEMDRIVE%\YtecDiskClone\ytec-winpe-app.exe --text',
            'echo.',
            'echo Read-only diagnostics completed. Use the Tsumugi GUI for verified clone/restore jobs.'
        ) | Set-Content -LiteralPath $launchScript -Encoding ascii
    }
    if ($ValidationScenario -eq 'StandaloneBootRepair') {
        @(
            '[LaunchApps]',
            '%SYSTEMROOT%\System32\wpeinit.exe',
            '%SYSTEMROOT%\System32\cmd.exe, /k %SYSTEMDRIVE%\YtecDiskClone\launch.cmd'
        ) | Set-Content -LiteralPath $winpeshl -Encoding ascii
    } else {
        @(
            '[LaunchApps]',
            '%SYSTEMROOT%\System32\wpeinit.exe',
            '%SYSTEMDRIVE%\YtecDiskClone\ytec-winpe-gui.exe'
        ) | Set-Content -LiteralPath $winpeshl -Encoding ascii
    }

    $mountedHash = (Get-FileHash -LiteralPath $mountedApp `
        -Algorithm SHA256).Hash
    if ($mountedHash -ne $appReport.sha256) {
        throw 'WIM内へコピーしたWinPEAppのSHA-256が元ファイルと一致しません。'
    }
    $mountedGuiHash = (Get-FileHash -LiteralPath $mountedGui `
        -Algorithm SHA256).Hash
    if ($mountedGuiHash -ne $guiReport.sha256) {
        throw 'WIM内へコピーしたWinPE GUIのSHA-256が元ファイルと一致しません。'
    }
    if ((Get-FileHash -LiteralPath $mountedLineSeedLicense `
            -Algorithm SHA256).Hash -ne $lineSeedLicenseReport.sha256) {
        throw 'WIM内へコピーしたLINE Seed JPライセンスのSHA-256が一致しません。'
    }
    if ((Get-FileHash -LiteralPath $mountedNotices `
            -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $thirdPartyNotices `
            -Algorithm SHA256).Hash) {
        throw 'WIM内へコピーした第三者ライセンス通知のSHA-256が一致しません。'
    }
    Write-MediaProgress -Percent 62 -Stage 'verified-product-payload'

    $payloadFiles = @(
        $mountedApp,
        $mountedGui,
        $mountedNotices,
        $mountedLineSeedLicense,
        $launchScript,
        $winpeshl)
    if ($ValidationScenario -eq 'StandaloneBootRepair') {
        $payloadFiles += @($uefiDiskPart, $biosDiskPart)
    }
    $addedFiles = @(
        foreach ($file in $payloadFiles) {
            [ordered]@{
                relativePath = $file.Substring($mountRoot.Length).TrimStart('\')
                length = (Get-Item -LiteralPath $file).Length
                sha256 = (Get-FileHash -LiteralPath $file `
                    -Algorithm SHA256).Hash
            }
        }
    )

    Invoke-CheckedNative `
        -Command $dism `
        -Arguments @(
            '/Unmount-Image',
            "/MountDir:$mountRoot",
            '/Commit',
            '/CheckIntegrity'
        ) `
        -Operation 'WinPE boot.wimのコミット'
    $mounted = $false
    Write-MediaProgress -Percent 74 -Stage 'committed-wim'
} catch {
    if ($mounted) {
        & $dism '/Unmount-Image' "/MountDir:$mountRoot" '/Discard'
    }
    throw
}

$stagedWimAfter = [ordered]@{
    length = (Get-Item -LiteralPath $bootWim).Length
    sha256 = (Get-FileHash -LiteralPath $bootWim -Algorithm SHA256).Hash
}
if ($stagedWimAfter.sha256 -eq $stagedWimBefore.sha256) {
    throw 'WinPEApp追加後もboot.wimのSHA-256が変化していません。'
}

if ($BuildUsb) {
    $writeTarget = Get-VerifiedUsbTarget `
        -Drive $TargetUsbDrive `
        -DiskNumber $ExpectedUsbDiskNumber `
        -SizeBytes $ExpectedUsbSizeBytes `
        -SerialSuffix $ExpectedUsbSerialSuffix `
        -DeviceInstanceId $ExpectedUsbDeviceInstanceId `
        -AllowUnpartitioned
    if ($writeTarget.partitionNumber -ne
            $initialUsbTarget.partitionNumber -or
        $writeTarget.partitionStyle -ne
            $initialUsbTarget.partitionStyle -or
        -not $writeTarget.deviceInstanceId.Equals(
            $initialUsbTarget.deviceInstanceId,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'WIM準備中にUSBの対象情報が変化したため、書込み前に停止しました。'
    }

    Write-MediaProgress -Percent 88 -Stage 'writing-usb'
    $preparedTarget = Initialize-VerifiedUsbTarget `
        -Drive $TargetUsbDrive `
        -DiskNumber $ExpectedUsbDiskNumber `
        -SizeBytes $ExpectedUsbSizeBytes `
        -SerialSuffix $ExpectedUsbSerialSuffix `
        -DeviceInstanceId $ExpectedUsbDeviceInstanceId
    if ($preparedTarget.partitionStyle -ne 'MBR' -or
        $preparedTarget.partitionNumber -ne 1) {
        throw '選択USBをMBR・単一FAT32パーティションへ初期化できませんでした。'
    }
    $bootExArgument = if ($CertificateGeneration -eq '2023CA') {
        ' /bootex'
    } else {
        ''
    }
    $makeCommandLine = (
        '"{0}" /UFD /F "{1}" {2}{3}' -f
            $makeWinPEMedia,
            $workingRoot,
            $TargetUsbDrive.ToUpperInvariant(),
            $bootExArgument)
    $originalPath = $env:Path
    try {
        $env:Path = @(
            $bootsectRoot,
            $oscdimgRoot,
            (Join-Path $env:SystemRoot 'System32'),
            $env:SystemRoot
        ) -join ';'
        Invoke-CheckedNative `
            -Command $systemCmd `
            -Arguments @('/d', '/c', $makeCommandLine) `
            -Operation '対象限定WinPE USBの作成'
    } finally {
        $env:Path = $originalPath
    }

    $verifiedTarget = Get-VerifiedUsbTarget `
        -Drive $TargetUsbDrive `
        -DiskNumber $ExpectedUsbDiskNumber `
        -SizeBytes $ExpectedUsbSizeBytes `
        -SerialSuffix $ExpectedUsbSerialSuffix `
        -DeviceInstanceId $ExpectedUsbDeviceInstanceId `
        -RequireMbr
    $sourceFiles = @(
        Get-ChildItem -LiteralPath $mediaRoot -Recurse -File -Force
    )
    if ($sourceFiles.Count -eq 0) {
        throw '検証するWinPE媒体ファイルがありません。'
    }
    $verifiedFiles = @(
        foreach ($sourceFile in $sourceFiles) {
            $relativePath = $sourceFile.FullName.Substring(
                $mediaRoot.Length).TrimStart('\')
            $usbPath = Join-Path $verifiedTarget.root $relativePath
            Assert-RegularNonReparseFile `
                -Path $usbPath `
                -Description "USB媒体ファイル $relativePath"
            $usbItem = Get-Item -LiteralPath $usbPath
            if ($usbItem.Length -ne $sourceFile.Length) {
                throw "USB媒体ファイルの長さが一致しません: $relativePath"
            }
            $sourceHash = (Get-FileHash `
                -LiteralPath $sourceFile.FullName `
                -Algorithm SHA256).Hash
            $usbHash = (Get-FileHash `
                -LiteralPath $usbItem.FullName `
                -Algorithm SHA256).Hash
            if ($usbHash -ne $sourceHash) {
                throw "USB媒体ファイルのSHA-256が一致しません: $relativePath"
            }
            [ordered]@{
                relativePath = $relativePath
                length = $usbItem.Length
                sha256 = $usbHash
            }
        }
    )
    foreach ($requiredRelativePath in @(
            'sources\boot.wim',
            'bootmgr',
            'EFI\BOOT\bootx64.efi')) {
        if ($verifiedFiles.relativePath -notcontains $requiredRelativePath) {
            throw "USBの必須起動ファイルを検証できません: $requiredRelativePath"
        }
    }

    $usbManifestPath = Join-Path `
        $outputFullPath 'usb-media-manifest.json'
    $usbBootWim = Join-Path `
        $verifiedTarget.root 'sources\boot.wim'
    $usbManifest = [ordered]@{
        schemaVersion = 1
        purpose = 'Y-TEC Tsumugi Drive WinPE rescue USB'
        generated = (Get-Date).ToString('o')
        repositoryContainsMicrosoftPayload = $false
        certificateGeneration = $CertificateGeneration
        validationScenario = $ValidationScenario
        adkVersion = [string]$candidate.deploymentToolsVersion
        dismVersion = [string]$candidate.dismFileVersion
        servicingUpdate = 'KB5101684'
        makeWinPEMedia = [ordered]@{
            path = $makeWinPEMedia
            length = (Get-Item -LiteralPath $makeWinPEMedia).Length
            sha256 = (Get-FileHash `
                -LiteralPath $makeWinPEMedia `
                -Algorithm SHA256).Hash
        }
        target = [ordered]@{
            diskNumber = $verifiedTarget.diskNumber
            drive = $verifiedTarget.drive
            sizeBytes = $verifiedTarget.sizeBytes
            serialSuffix = $verifiedTarget.serialSuffix
            partitionNumber = $verifiedTarget.partitionNumber
        }
        sourceWim = $preflight.sourceWim
        stagedWimBefore = $stagedWimBefore
        addedFiles = $addedFiles
        stagedWimAfter = $stagedWimAfter
        winpeApp = $appReport
        verifiedFileCount = $verifiedFiles.Count
        verifiedFiles = $verifiedFiles
        bootWimSha256 = (Get-FileHash `
            -LiteralPath $usbBootWim `
            -Algorithm SHA256).Hash
        retainedWorkRoot = $outputFullPath
    }
    [IO.File]::WriteAllText(
        $usbManifestPath,
        ($usbManifest | ConvertTo-Json -Depth 10),
        [Text.UTF8Encoding]::new($false))
    Write-MediaProgress -Percent 94 -Stage 'verified-usb'
    Write-MediaProgress -Percent 100 -Stage 'completed-usb'
    Write-Output "WINPE_APP_USB_PASS=$usbManifestPath"
    return
}

$bootData = '-bootdata:2#p0,e,b{0}#pEF,e,b{1}' -f `
    $etfsboot, $efiBootImage
Invoke-CheckedNative `
    -Command $oscdimg `
    -Arguments @($bootData, '-u1', '-udfver102', $mediaRoot, $isoPath) `
    -Operation 'WinPEApp検証ISOの作成'
Assert-RegularNonReparseFile -Path $isoPath -Description '生成ISO'
Write-MediaProgress -Percent 88 -Stage 'generated-iso'

$stagedIsoLength = (Get-Item -LiteralPath $isoPath).Length
$stagedIsoHash = (Get-FileHash -LiteralPath $isoPath `
    -Algorithm SHA256).Hash
$publishedIsoPath = if ([string]::IsNullOrWhiteSpace($finalIsoFullPath)) {
    $isoPath
} else {
    $finalIsoFullPath
}

$manifest = [ordered]@{
    schemaVersion = 1
    purpose = 'Y-TEC Tsumugi Drive WinPEApp validation media'
    generated = (Get-Date).ToString('o')
    repositoryContainsMicrosoftPayload = $false
    certificateGeneration = $CertificateGeneration
    validationScenario = $ValidationScenario
    adkVersion = [string]$candidate.deploymentToolsVersion
    dismVersion = [string]$candidate.dismFileVersion
    servicingUpdate = 'KB5101684'
    japaneseFontSupport = $preflight.japaneseFontSupport
    sourceWim = $preflight.sourceWim
    uefiBootManagers = $uefiBootManagers
    stagedWimBefore = $stagedWimBefore
    addedFiles = $addedFiles
    stagedWimAfter = $stagedWimAfter
    winpeApp = $appReport
    iso = [ordered]@{
        path = $publishedIsoPath
        length = $stagedIsoLength
        sha256 = $stagedIsoHash
    }
    retainedWorkRoot = $outputFullPath
}
$manifestJson = $manifest | ConvertTo-Json -Depth 8
[IO.File]::WriteAllText(
    $manifestPath,
    $manifestJson,
    [Text.UTF8Encoding]::new($false))
Write-MediaProgress -Percent 94 -Stage 'verified-iso'

$publishedManifestPath = $manifestPath
if (-not [string]::IsNullOrWhiteSpace($finalIsoFullPath)) {
    [IO.File]::Move($isoPath, $finalIsoFullPath)
    Assert-RegularNonReparseFile `
        -Path $finalIsoFullPath `
        -Description '確定済みISO'
    $publishedHash = (Get-FileHash -LiteralPath $finalIsoFullPath `
        -Algorithm SHA256).Hash
    if ($publishedHash -ne $stagedIsoHash) {
        throw '完成名へ移動したISOのSHA-256がステージング時と一致しません。'
    }
    [IO.File]::Move($manifestPath, $finalManifestFullPath)
    $publishedManifestPath = $finalManifestFullPath
}

Write-MediaProgress -Percent 100 -Stage 'completed-iso'
Write-Output "WINPE_APP_MEDIA_PASS=$publishedManifestPath"
